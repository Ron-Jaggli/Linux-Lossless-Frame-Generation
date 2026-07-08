#pragma once

#include "core/pacer.hpp"

#include <cstdint>
#include <functional>

#include <vulkan/vulkan.h>

namespace lsfg {

namespace vk {
class Context;
class FramePool;
class Interpolator;
}

// Presents to the swapchain, letterboxed, once per display refresh (or as
// fast as the present mode allows). Shows the latest pool frame, or — when
// a pace source and an interpolator are set and the pacer asks for it — an
// in-between of the last two unique frames.
class Renderer {
public:
    bool init(vk::Context& ctx, vk::FramePool& pool);
    void destroy();

    // Optional frame generation; without these every refresh is
    // passthrough. The pace source is polled once per refresh.
    void setPaceSource(std::function<PaceDecision(double)> f) {
        pace_source_ = std::move(f);
    }
    void setInterpolator(vk::Interpolator* interp) { interp_ = interp; }

    // Returns false on unrecoverable error.
    bool drawFrame();
    void notifyResize() { needs_recreate_ = true; }

    uint64_t presentedFrames() const { return presented_; }
    // presented frames that were interpolated (not real captured frames)
    uint64_t generatedFrames() const { return generated_; }
    // EMA of (present time - capture time) in milliseconds, -1 before data.
    // For generated frames the content timestamp is the pair interpolant,
    // so the figure includes the inherent one-source-period hold.
    double latencyMs() const { return latency_ms_; }

private:
    vk::Context* ctx_ = nullptr;
    vk::FramePool* pool_ = nullptr;
    vk::Interpolator* interp_ = nullptr;
    std::function<PaceDecision(double)> pace_source_;
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
