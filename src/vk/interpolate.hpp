#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace lsfg::vk {

class Context;

// Produces an in-between frame from a pair of source frames. The LSFG
// shader chain (milestone 3 proper) will be a second implementation behind
// this interface; BlendInterpolator is the baseline that exercises the
// whole frame-generation path.
//
// Contract: record() is called from the render thread between
// vkBeginCommandBuffer and vkEndCommandBuffer, and the previous record()'s
// submission has completed (the renderer fence-waits every frame). a and b
// are pool images in TRANSFER_SRC_OPTIMAL layout; record() must return
// them to that layout. The returned image is owned by the interpolator and
// is in TRANSFER_SRC_OPTIMAL layout once the commands execute.
class Interpolator {
public:
    virtual ~Interpolator() = default;
    virtual bool init(Context& ctx) = 0;
    virtual void destroy() = 0;
    virtual VkImage record(VkCommandBuffer cmd, VkImage a, VkImage b,
                           VkFormat src_format, uint32_t width,
                           uint32_t height, float phase) = 0;
};

// mix(A, B, phase) in one compute pass. Visually mediocre on motion
// (ghosting) but end-to-end correct.
class BlendInterpolator final : public Interpolator {
public:
    bool init(Context& ctx) override;
    void destroy() override;
    VkImage record(VkCommandBuffer cmd, VkImage a, VkImage b,
                   VkFormat src_format, uint32_t width, uint32_t height,
                   float phase) override;

private:
    bool ensureIntermediate(uint32_t width, uint32_t height);
    void destroyIntermediate();
    void destroySourceViews();

    Context* ctx_ = nullptr;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet desc_set_ = VK_NULL_HANDLE;

    // Owned output image; recreated when the source size changes.
    VkImage dst_image_ = VK_NULL_HANDLE;
    VkDeviceMemory dst_memory_ = VK_NULL_HANDLE;
    VkImageView dst_view_ = VK_NULL_HANDLE;
    uint32_t dst_width_ = 0, dst_height_ = 0;

    // Views into the leased pool images, remade every record(); the
    // previous pair is destroyed at the next record() (or destroy()), by
    // which point its submission has retired.
    VkImageView src_views_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
};

} // namespace lsfg::vk
