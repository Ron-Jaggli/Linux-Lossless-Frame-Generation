#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

struct SDL_Window;

namespace lsfg {
struct Options;
}

namespace lsfg::vk {

// Owns the SDL window, Vulkan instance/device/queue and the swapchain.
// The single graphics queue is shared between the capture thread and the
// render thread; all submits must hold queue_mutex.
class Context {
public:
    bool init(const Options& opts);
    void shutdown();

    bool createSwapchain(); // (re)creates from current window pixel size

    // DRM format modifiers usable as a blit/sample source when imported as a
    // DMA-BUF, for advertising to PipeWire. Empty if unsupported.
    std::vector<uint64_t> drmModifiersFor(VkFormat format) const;

    uint32_t findMemoryType(uint32_t type_bits, VkMemoryPropertyFlags props) const;

    SDL_Window* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    std::mutex queue_mutex;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swap_format = VK_FORMAT_UNDEFINED;
    VkExtent2D swap_extent{};
    std::vector<VkImage> swap_images;
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;

    bool dmabuf_supported = false;   // all required extensions present
    bool has_foreign_queue = false;  // VK_EXT_queue_family_foreign
    PFN_vkGetMemoryFdPropertiesKHR pfn_get_memory_fd_props = nullptr;

    std::string device_name;

private:
    bool pickPhysicalDevice();
    VkPresentModeKHR wanted_present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
};

} // namespace lsfg::vk
