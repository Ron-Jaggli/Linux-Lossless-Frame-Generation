#pragma once

#include "core/pacer.hpp"
#include "vk/interpolate.hpp"

#include <cstdint>
#include <mutex>

#include <vulkan/vulkan.h>

namespace lsfg {

namespace vk {
class Context;
class FramePool;
}

// Presents to the swapchain, letterboxed, once per display refresh (or as
// fast as the present mode allows). With frame generation enabled, the
// pacer decides per refresh whether to show the latest real frame or the
// unique pair blended by the interpolator.
class Renderer {
public:
    bool init(vk::Context& ctx, vk::FramePool& pool);
    void destroy();

    // Turns frame generation on: decisions come from pacer, which is shared
    // with the capture thread and guarded by pacer_mutex. Call after init();
    // if the interpolator cannot be created the renderer stays passthrough.
    void enableFrameGen(FramePacer* pacer, std::mutex* pacer_mutex);
    // Runtime toggle (G key). Main-thread only, like drawFrame.
    void setFrameGenEnabled(bool on) { fg_enabled_ = on; }
    bool frameGenEnabled() const { return fg_enabled_; }

    // Returns false on unrecoverable error.
    bool drawFrame();
    void notifyResize() { needs_recreate_ = true; }

    uint64_t presentedFrames() const { return presented_; }
    // presented frames that were GPU-generated in-betweens (phase > 0)
    uint64_t interpolatedFrames() const { return interpolated_; }
    // EMA of (present time - content time) in milliseconds, -1 before data
    double latencyMs() const { return latency_ms_; }

private:
    vk::Context* ctx_ = nullptr;
    vk::FramePool* pool_ = nullptr;
    FramePacer* pacer_ = nullptr;
    std::mutex* pacer_mutex_ = nullptr;
    vk::BlendInterpolator interp_;
    bool interp_ready_ = false;
    bool fg_enabled_ = true;
    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkSemaphore sem_acquire_ = VK_NULL_HANDLE;
    VkSemaphore sem_render_ = VK_NULL_HANDLE;
    bool needs_recreate_ = false;
    uint64_t presented_ = 0;
    uint64_t interpolated_ = 0;
    double latency_ms_ = -1.0;
};

} // namespace lsfg
