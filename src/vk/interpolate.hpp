#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace lsfg::vk {

class Context;

// Records the GPU work that produces an in-between image from a pair of
// source frames. Implementations own their output image; the caller blits
// it to the swapchain. All methods run on the render thread, which
// fence-waits each frame's submission before recording the next, so
// implementations may reuse per-frame resources without extra
// synchronization. The LSFG shader chain (milestone 3b) will be a second
// implementation of this interface.
class Interpolator {
public:
    virtual ~Interpolator() = default;

    virtual bool create(Context& ctx) = 0;
    virtual void destroy() = 0;

    // Record into cmd: produce the in-between of a and b (both in
    // TRANSFER_SRC_OPTIMAL layout, returned to it) at phase in (0,1).
    // src_format is the pool image format. The returned image is in
    // TRANSFER_SRC_OPTIMAL layout once cmd executes; VK_NULL_HANDLE on
    // failure (the caller falls back to showing a real frame).
    virtual VkImage record(VkCommandBuffer cmd, VkImage a, VkImage b,
                           VkFormat src_format, uint32_t width,
                           uint32_t height, float phase) = 0;

    // Called after the frame's submission has been fence-waited, while the
    // pair lease is still held; release per-frame resources here.
    virtual void onFrameComplete() {}
};

// Baseline: dst = mix(A, B, phase) in one compute pass. Visually mediocre
// on motion (ghosting), but it exercises pairing, pacing, the extra GPU
// pass and present timing end to end.
class BlendInterpolator final : public Interpolator {
public:
    bool create(Context& ctx) override;
    void destroy() override;
    VkImage record(VkCommandBuffer cmd, VkImage a, VkImage b,
                   VkFormat src_format, uint32_t width, uint32_t height,
                   float phase) override;
    void onFrameComplete() override;

private:
    bool ensureTarget(uint32_t width, uint32_t height);
    void destroyTarget();

    Context* ctx_ = nullptr;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet desc_set_ = VK_NULL_HANDLE;

    // owned output image (recreated when the source size changes)
    VkImage out_image_ = VK_NULL_HANDLE;
    VkDeviceMemory out_memory_ = VK_NULL_HANDLE;
    VkImageView out_view_ = VK_NULL_HANDLE;
    uint32_t width_ = 0, height_ = 0;
    bool out_initialized_ = false; // first use transitions from UNDEFINED

    // per-frame views of the leased pair, destroyed in onFrameComplete
    VkImageView view_a_ = VK_NULL_HANDLE, view_b_ = VK_NULL_HANDLE;
};

} // namespace lsfg::vk
