#pragma once

#include <cstdint>
#include <mutex>

#include <vulkan/vulkan.h>

namespace lsfg {

class FramePacer;

namespace vk {
class Context;
class FramePool;
class Interpolator;
}

// Presents to the swapchain once per display refresh (or as fast as the
// present mode allows). Without frame generation, shows the latest pool
// frame letterboxed; with it, consults the pacer each refresh and either
// shows a real frame or runs the interpolator on the last unique pair.
class Renderer {
public:
    bool init(vk::Context& ctx, vk::FramePool& pool);
    void destroy();

    // Optional: enable frame generation. The pacer is shared with the
    // capture thread; the mutex serializes access. The interpolator is
    // used only on the render thread.
    void setFrameGen(FramePacer* pacer, std::mutex* pacer_mutex,
                     vk::Interpolator* interp) {
        pacer_ = pacer;
        pacer_mutex_ = pacer_mutex;
        interp_ = interp;
    }

    // Returns false on unrecoverable error.
    bool drawFrame();
    void notifyResize() { needs_recreate_ = true; }

    uint64_t presentedFrames() const { return presented_; }
    // presents that showed an interpolated (generated) image
    uint64_t generatedFrames() const { return generated_; }
    // EMA of (present time - capture time) in milliseconds, -1 before data.
    // For generated frames the capture time is interpolated between the
    // pair, so the inherent one-source-period hold shows up here.
    double latencyMs() const { return latency_ms_; }

private:
    vk::Context* ctx_ = nullptr;
    vk::FramePool* pool_ = nullptr;
    FramePacer* pacer_ = nullptr;
    std::mutex* pacer_mutex_ = nullptr;
    vk::Interpolator* interp_ = nullptr;
    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkSemaphore sem_acquire_ = VK_NULL_HANDLE;
    VkSemaphore sem_render_ = VK_NULL_HANDLE;
    bool needs_recreate_ = false;
    uint64_t presented_ = 0;
    uint64_t generated_ = 0;
    double latency_ms_ = -1.0;
};

} // namespace lsfg
