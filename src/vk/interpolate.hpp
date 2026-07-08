#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace lsfg::vk {

class Context;

// Produces an in-between frame from a pair of captured frames. Records into
// the caller's command buffer; the implementation owns its output image and
// leaves it in TRANSFER_SRC_OPTIMAL layout, ready for the presentation blit.
// The caller leases the input pair for the duration of the GPU work and
// fence-waits before the next record(), so per-frame resources (views,
// descriptor set) can be reused without hazards. Milestone 3b drops the LSFG
// shader chain in as a second implementation of this interface.
class Interpolator {
public:
    virtual ~Interpolator() = default;

    virtual bool init(Context& ctx) = 0;
    virtual void destroy() = 0;

    // a and b are pool images in TRANSFER_SRC_OPTIMAL layout, src_format /
    // width / height as negotiated. They are returned to that layout before
    // the recording ends. phase 0 reproduces a, 1 reproduces b. Recreates
    // the output image on size changes; false on unrecoverable error.
    virtual bool record(VkCommandBuffer cmd, VkImage a, VkImage b,
                        VkFormat src_format, uint32_t width, uint32_t height,
                        float phase) = 0;

    virtual VkImage output() const = 0;
    virtual uint32_t outputWidth() const = 0;
    virtual uint32_t outputHeight() const = 0;
};

// Baseline: mix(A, B, phase) in one compute pass. Visually mediocre on
// motion (ghosting) but exercises the whole pairing/pacing/present pipeline.
class BlendInterpolator final : public Interpolator {
public:
    bool init(Context& ctx) override;
    void destroy() override;
    bool record(VkCommandBuffer cmd, VkImage a, VkImage b,
                VkFormat src_format, uint32_t width, uint32_t height,
                float phase) override;
    VkImage output() const override { return out_image_; }
    uint32_t outputWidth() const override { return out_width_; }
    uint32_t outputHeight() const override { return out_height_; }

private:
    bool ensureOutput(uint32_t width, uint32_t height);
    bool ensureInputViews(VkImage a, VkImage b, VkFormat format);
    void destroyOutput();
    void destroyInputViews();

    Context* ctx_ = nullptr;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dset_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool dset_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet dset_ = VK_NULL_HANDLE;

    // owned output image (rgba8: universally storage-capable)
    VkImage out_image_ = VK_NULL_HANDLE;
    VkDeviceMemory out_memory_ = VK_NULL_HANDLE;
    VkImageView out_view_ = VK_NULL_HANDLE;
    uint32_t out_width_ = 0, out_height_ = 0;

    // input views, recreated per record (previous frame's are fence-idle)
    VkImageView view_a_ = VK_NULL_HANDLE, view_b_ = VK_NULL_HANDLE;
};

} // namespace lsfg::vk
