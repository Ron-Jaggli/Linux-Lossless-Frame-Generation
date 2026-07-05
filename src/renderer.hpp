#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace lsfg {

namespace vk {
class Context;
class FramePool;
}

// Presents the latest pool frame to the swapchain, letterboxed, once per
// display refresh (or as fast as the present mode allows).
class Renderer {
public:
    bool init(vk::Context& ctx, vk::FramePool& pool);
    void destroy();

    // Returns false on unrecoverable error.
    bool drawFrame();
    void notifyResize() { needs_recreate_ = true; }

    uint64_t presentedFrames() const { return presented_; }
    // EMA of (present time - capture time) in milliseconds, -1 before data
    double latencyMs() const { return latency_ms_; }

private:
    vk::Context* ctx_ = nullptr;
    vk::FramePool* pool_ = nullptr;
    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkSemaphore sem_acquire_ = VK_NULL_HANDLE;
    VkSemaphore sem_render_ = VK_NULL_HANDLE;
    bool needs_recreate_ = false;
    uint64_t presented_ = 0;
    double latency_ms_ = -1.0;
};

} // namespace lsfg
