#pragma once

#include "core/pacer.hpp"

#include <cstdint>
#include <mutex>

#include <vulkan/vulkan.h>

namespace lsfg {

namespace vk {
class Context;
class FramePool;
class Interpolator;
}

// Presents to the swapchain, letterboxed, once per display refresh (or as
// fast as the present mode allows). Without frame generation it blits the
// latest pool frame; with it, the pacer picks the real frame or an
// interpolated in-between of the latest unique pair.
class Renderer {
public:
    bool init(vk::Context& ctx, vk::FramePool& pool);
    void destroy();

    // Optional; call before the first drawFrame. The pacer is shared with
    // the capture thread and guarded by pacer_mutex.
    void enableFrameGen(FramePacer& pacer, std::mutex& pacer_mutex,
                        vk::Interpolator& interp);

    // Returns false on unrecoverable error.
    bool drawFrame();
    void notifyResize() { needs_recreate_ = true; }

    uint64_t presentedFrames() const { return presented_; }
    // distinct frames shown (real or generated); the effective output rate
    uint64_t outputFrames() const { return outputs_; }
    // EMA of (present time - displayed-content time) in milliseconds, -1
    // before data; for generated frames the content time lies between the
    // pair's captures, so the inherent one-source-period hold is included
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
    uint64_t outputs_ = 0;
    double latency_ms_ = -1.0;
    // identity of what was last shown, to count distinct outputs
    bool have_last_shown_ = false;
    bool last_was_interp_ = false;
    uint64_t last_seq_a_ = 0, last_seq_b_ = 0;
    int last_step_ = 0;
};

} // namespace lsfg
