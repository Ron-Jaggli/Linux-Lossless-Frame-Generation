#include "vk/dmabuf_import.hpp"

#include "log.hpp"
#include "vk/context.hpp"

#include <fcntl.h>
#include <unistd.h>

namespace lsfg::vk {

bool importDmaBuf(Context& ctx, VkFormat format, uint32_t width, uint32_t height,
                  uint64_t modifier, const DmaBufPlane* planes, uint32_t n_planes,
                  ImportedImage& out, std::string& error) {
    VkSubresourceLayout layouts[4]{};
    if (n_planes == 0 || n_planes > 4) {
        error = "unsupported plane count " + std::to_string(n_planes);
        return false;
    }
    for (uint32_t i = 0; i < n_planes; i++) {
        layouts[i].offset = planes[i].offset;
        layouts[i].rowPitch = planes[i].stride;
    }

    VkImageDrmFormatModifierExplicitCreateInfoEXT mod_info{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    mod_info.drmFormatModifier = modifier;
    mod_info.drmFormatModifierPlaneCount = n_planes;
    mod_info.pPlaneLayouts = layouts;

    VkExternalMemoryImageCreateInfo ext_info{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &mod_info};
    ext_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext_info};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult res = vkCreateImage(ctx.device, &ici, nullptr, &out.image);
    if (res != VK_SUCCESS) {
        error = "vkCreateImage(modifier) failed: " + std::to_string(res);
        return false;
    }

    VkMemoryFdPropertiesKHR fd_props{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    res = ctx.pfn_get_memory_fd_props(
        ctx.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, planes[0].fd,
        &fd_props);
    if (res != VK_SUCCESS) {
        error = "vkGetMemoryFdPropertiesKHR failed: " + std::to_string(res);
        destroyImported(ctx, out);
        return false;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(ctx.device, out.image, &req);
    uint32_t type_bits = req.memoryTypeBits & fd_props.memoryTypeBits;
    uint32_t mem_type = ctx.findMemoryType(type_bits, 0);
    if (mem_type == UINT32_MAX) {
        error = "no compatible memory type for dmabuf";
        destroyImported(ctx, out);
        return false;
    }

    int dup_fd = fcntl(planes[0].fd, F_DUPFD_CLOEXEC, 0);
    if (dup_fd < 0) {
        error = "fcntl(F_DUPFD_CLOEXEC) failed";
        destroyImported(ctx, out);
        return false;
    }

    VkMemoryDedicatedAllocateInfo dedicated{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = out.image;
    VkImportMemoryFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                   &dedicated};
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import.fd = dup_fd;

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mem_type;
    res = vkAllocateMemory(ctx.device, &mai, nullptr, &out.memory);
    if (res != VK_SUCCESS) {
        close(dup_fd); // on failure the fd was not consumed
        error = "vkAllocateMemory(import) failed: " + std::to_string(res);
        destroyImported(ctx, out);
        return false;
    }
    res = vkBindImageMemory(ctx.device, out.image, out.memory, 0);
    if (res != VK_SUCCESS) {
        error = "vkBindImageMemory failed: " + std::to_string(res);
        destroyImported(ctx, out);
        return false;
    }
    out.width = width;
    out.height = height;
    return true;
}

void destroyImported(Context& ctx, ImportedImage& img) {
    if (img.image)
        vkDestroyImage(ctx.device, img.image, nullptr);
    if (img.memory)
        vkFreeMemory(ctx.device, img.memory, nullptr);
    img = {};
}

} // namespace lsfg::vk
