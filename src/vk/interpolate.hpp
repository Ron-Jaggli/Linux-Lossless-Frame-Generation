#pragma once

#include "vk/frame_pool.hpp"

#include <vulkan/vulkan.h>

namespace lsfg::vk {

class Context;

// Produces an in-between frame from a unique-frame pair. Implementations
// own their output image; record() emits the commands into the caller's
// command buffer and leaves the result in TRANSFER_SRC_OPTIMAL layout, so
// the renderer can blit it to the swapchain like a pool frame. The pair's
// images are returned to TRANSFER_SRC_OPTIMAL as well. The caller
// fence-waits each submission, so per-call resources (descriptor set,
// output image) are never in flight during the next record(). The LSFG
// shader chain (milestone 3b) will slot in as a second implementation.
class Interpolator {
public:
    virtual ~Interpolator() = default;
    virtual bool create(Context& ctx) = 0;
    virtual void destroy() = 0;
    // Recreates internal resources when the pair's size differs from the
    // last call. Returns false on resource errors (caller should fall back
    // to passthrough).
    virtual bool record(VkCommandBuffer cmd, const FramePool::PairLease& pair,
                        float phase) = 0;
    virtual VkImage result() const = 0;
    virtual VkExtent2D resultExtent() const = 0;
};

// Baseline implementation: one compute pass writing mix(A, B, phase).
// Visually naive (ghosts on motion) but exercises the whole frame-gen
// path: pairing, pacing, the extra GPU pass, and present timing.
class BlendInterpolator : public Interpolator {
public:
    bool create(Context& ctx) override;
    void destroy() override;
    bool record(VkCommandBuffer cmd, const FramePool::PairLease& pair,
                float phase) override;
    VkImage result() const override { return dst_image_; }
    VkExtent2D resultExtent() const override { return extent_; }

private:
    bool ensureOutput(uint32_t width, uint32_t height);
    void destroyOutput();

    Context* ctx_ = nullptr;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dset_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool dpool_ = VK_NULL_HANDLE;
    VkDescriptorSet dset_ = VK_NULL_HANDLE;
    VkImage dst_image_ = VK_NULL_HANDLE;
    VkDeviceMemory dst_memory_ = VK_NULL_HANDLE;
    VkImageView dst_view_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
};

} // namespace lsfg::vk
