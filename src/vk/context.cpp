#include "vk/context.hpp"

#include "log.hpp"
#include "options.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstring>
#include <set>

namespace lsfg::vk {

static const char* TAG = "vulkan";

bool Context::init(const Options& opts) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        logError(TAG, "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    window = SDL_CreateWindow("lsfg-cap", 1280, 720,
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                                  SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        logError(TAG, "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    if (opts.fullscreen)
        SDL_SetWindowFullscreen(window, true);

    // --- instance ---
    Uint32 n_sdl_ext = 0;
    const char* const* sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n_sdl_ext);
    std::vector<const char*> inst_ext(sdl_ext, sdl_ext + n_sdl_ext);

    std::vector<const char*> layers;
    if (opts.validate)
        layers.push_back("VK_LAYER_KHRONOS_validation");

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "lsfg-cap";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(inst_ext.size());
    ici.ppEnabledExtensionNames = inst_ext.data();
    ici.enabledLayerCount = uint32_t(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        logError(TAG, "vkCreateInstance failed%s",
                 opts.validate ? " (validation layers installed?)" : "");
        return false;
    }

    if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
        logError(TAG, "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return false;
    }

    if (!pickPhysicalDevice())
        return false;

    // --- device ---
    uint32_t n_ext = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &n_ext, nullptr);
    std::vector<VkExtensionProperties> avail(n_ext);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &n_ext, avail.data());
    auto has = [&](const char* name) {
        return std::any_of(avail.begin(), avail.end(), [&](const auto& e) {
            return std::strcmp(e.extensionName, name) == 0;
        });
    };

    std::vector<const char*> dev_ext = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    dmabuf_supported = has(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
                       has(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) &&
                       has(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    if (dmabuf_supported) {
        dev_ext.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        dev_ext.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        dev_ext.push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
        if (has(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) {
            dev_ext.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
            has_foreign_queue = true;
        }
    } else {
        logWarn(TAG, "DMA-BUF import extensions missing; SHM capture only");
    }
    if (opts.no_dmabuf)
        dmabuf_supported = false;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(dev_ext.size());
    dci.ppEnabledExtensionNames = dev_ext.data();
    if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) {
        logError(TAG, "vkCreateDevice failed");
        return false;
    }
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    if (dmabuf_supported) {
        pfn_get_memory_fd_props = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
            vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR"));
        if (!pfn_get_memory_fd_props)
            dmabuf_supported = false;
    }

    if (opts.present_mode == "mailbox")
        wanted_present_mode_ = VK_PRESENT_MODE_MAILBOX_KHR;
    else if (opts.present_mode == "immediate")
        wanted_present_mode_ = VK_PRESENT_MODE_IMMEDIATE_KHR;
    else
        wanted_present_mode_ = VK_PRESENT_MODE_FIFO_KHR;

    if (!createSwapchain())
        return false;

    logInfo(TAG, "device: %s | dmabuf: %s | present: %s", device_name.c_str(),
            dmabuf_supported ? "yes" : "no",
            present_mode == VK_PRESENT_MODE_MAILBOX_KHR    ? "mailbox"
            : present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate"
                                                            : "fifo (vsync)");
    return true;
}

bool Context::pickPhysicalDevice() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    if (n == 0) {
        logError(TAG, "no Vulkan devices found");
        return false;
    }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(instance, &n, devs.data());

    int best_score = -1;
    for (auto d : devs) {
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qs(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, qs.data());
        for (uint32_t i = 0; i < nq; i++) {
            if (!(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                continue;
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface, &present);
            if (!present)
                continue;
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(d, &props);
            int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2
                        : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1
                                                                                     : 0;
            if (score > best_score) {
                best_score = score;
                phys = d;
                queue_family = i;
                device_name = props.deviceName;
            }
            break;
        }
    }
    if (best_score < 0) {
        logError(TAG, "no device with graphics+present queue found");
        return false;
    }
    return true;
}

bool Context::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);

    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(uint32_t(pw), caps.minImageExtent.width,
                                  caps.maxImageExtent.width);
        extent.height = std::clamp(uint32_t(ph), caps.minImageExtent.height,
                                   caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0)
        return false; // minimized; try again later

    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(nf);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &nf, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats) {
        // UNORM (not sRGB) keeps the blit a raw byte copy: captured frames are
        // already sRGB-encoded, presenting them as-is is correct.
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM ||
             f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }

    uint32_t nm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &nm, nullptr);
    std::vector<VkPresentModeKHR> modes(nm);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &nm, modes.data());
    present_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (std::find(modes.begin(), modes.end(), wanted_present_mode_) != modes.end())
        present_mode = wanted_present_mode_;

    uint32_t min_images = caps.minImageCount + 1;
    if (caps.maxImageCount > 0)
        min_images = std::min(min_images, caps.maxImageCount);

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface;
    sci.minImageCount = min_images;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = present_mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = swapchain;

    VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
    VkResult res = vkCreateSwapchainKHR(device, &sci, nullptr, &new_swapchain);
    if (sci.oldSwapchain != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(device, sci.oldSwapchain, nullptr);
    if (res != VK_SUCCESS) {
        logError(TAG, "vkCreateSwapchainKHR failed (%d)", res);
        swapchain = VK_NULL_HANDLE;
        return false;
    }
    swapchain = new_swapchain;
    swap_format = chosen.format;
    swap_extent = extent;

    uint32_t ni = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &ni, nullptr);
    swap_images.resize(ni);
    vkGetSwapchainImagesKHR(device, swapchain, &ni, swap_images.data());
    logDebug(TAG, "swapchain %ux%u, %u images", extent.width, extent.height, ni);
    return true;
}

std::vector<uint64_t> Context::drmModifiersFor(VkFormat format) const {
    std::vector<uint64_t> out;
    if (!dmabuf_supported)
        return out;

    VkDrmFormatModifierPropertiesListEXT list{
        VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 props{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &list};
    vkGetPhysicalDeviceFormatProperties2(phys, format, &props);
    if (list.drmFormatModifierCount == 0)
        return out;
    std::vector<VkDrmFormatModifierPropertiesEXT> mods(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = mods.data();
    vkGetPhysicalDeviceFormatProperties2(phys, format, &props);

    for (const auto& m : mods) {
        constexpr VkFormatFeatureFlags need =
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
        if ((m.drmFormatModifierTilingFeatures & need) != need)
            continue;

        // Confirm the driver can actually import this (format, modifier) pair
        // as a DMA-BUF image before advertising it to the compositor.
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_info{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
        mod_info.drmFormatModifier = m.drmFormatModifier;
        mod_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkPhysicalDeviceExternalImageFormatInfo ext_info{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            &mod_info};
        ext_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkPhysicalDeviceImageFormatInfo2 fmt_info{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ext_info};
        fmt_info.format = format;
        fmt_info.type = VK_IMAGE_TYPE_2D;
        fmt_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        fmt_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        VkExternalImageFormatProperties ext_props{
            VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 img_props{
            VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &ext_props};
        if (vkGetPhysicalDeviceImageFormatProperties2(phys, &fmt_info,
                                                      &img_props) != VK_SUCCESS)
            continue;
        if (!(ext_props.externalMemoryProperties.externalMemoryFeatures &
              VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
            continue;
        out.push_back(m.drmFormatModifier);
    }
    return out;
}

uint32_t Context::findMemoryType(uint32_t type_bits,
                                 VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(phys, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

void Context::shutdown() {
    if (device)
        vkDeviceWaitIdle(device);
    if (swapchain)
        vkDestroySwapchainKHR(device, swapchain, nullptr);
    if (device)
        vkDestroyDevice(device, nullptr);
    if (surface)
        vkDestroySurfaceKHR(instance, surface, nullptr);
    if (instance)
        vkDestroyInstance(instance, nullptr);
    if (window)
        SDL_DestroyWindow(window);
    SDL_Quit();
}

} // namespace lsfg::vk
