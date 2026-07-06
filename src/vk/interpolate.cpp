#include "vk/interpolate.hpp"

#include "log.hpp"
#include "vk/context.hpp"

#include "shaders/blend_comp_spv.h"

namespace lsfg::vk {

static const char* TAG = "interp";

// The output stays a fixed UNORM format: it only needs storage-image
// support (guaranteed for R8G8B8A8_UNORM) and blits to the swapchain with
// per-component conversion, whatever the source format is.
static constexpr VkFormat OUT_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;

bool BlendInterpolator::create(Context& ctx) {
    ctx_ = &ctx;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(ctx.device, &sci, nullptr, &sampler_) != VK_SUCCESS) {
        logError(TAG, "vkCreateSampler failed");
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dlci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = 3;
    dlci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(ctx.device, &dlci, nullptr, &set_layout_) !=
        VK_SUCCESS) {
        logError(TAG, "vkCreateDescriptorSetLayout failed");
        return false;
    }

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float)};
    VkPipelineLayoutCreateInfo plci{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(ctx.device, &plci, nullptr, &pipe_layout_) !=
        VK_SUCCESS) {
        logError(TAG, "vkCreatePipelineLayout failed");
        return false;
    }

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = sizeof(blend_comp_spv);
    smci.pCode = blend_comp_spv;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(ctx.device, &smci, nullptr, &module) !=
        VK_SUCCESS) {
        logError(TAG, "vkCreateShaderModule failed");
        return false;
    }
    VkComputePipelineCreateInfo cpci{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  nullptr,
                  0,
                  VK_SHADER_STAGE_COMPUTE_BIT,
                  module,
                  "main",
                  nullptr};
    cpci.layout = pipe_layout_;
    VkResult res = vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1,
                                            &cpci, nullptr, &pipeline_);
    vkDestroyShaderModule(ctx.device, module, nullptr);
    if (res != VK_SUCCESS) {
        logError(TAG, "vkCreateComputePipelines failed (%d)", res);
        return false;
    }

    VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
    };
    VkDescriptorPoolCreateInfo dpci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(ctx.device, &dpci, nullptr, &desc_pool_) !=
        VK_SUCCESS) {
        logError(TAG, "vkCreateDescriptorPool failed");
        return false;
    }
    VkDescriptorSetAllocateInfo dsai{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = desc_pool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &set_layout_;
    if (vkAllocateDescriptorSets(ctx.device, &dsai, &desc_set_) !=
        VK_SUCCESS) {
        logError(TAG, "vkAllocateDescriptorSets failed");
        return false;
    }
    return true;
}

void BlendInterpolator::destroyTarget() {
    if (out_view_)
        vkDestroyImageView(ctx_->device, out_view_, nullptr);
    if (out_image_)
        vkDestroyImage(ctx_->device, out_image_, nullptr);
    if (out_memory_)
        vkFreeMemory(ctx_->device, out_memory_, nullptr);
    out_view_ = VK_NULL_HANDLE;
    out_image_ = VK_NULL_HANDLE;
    out_memory_ = VK_NULL_HANDLE;
    out_initialized_ = false;
    width_ = height_ = 0;
}

void BlendInterpolator::destroy() {
    if (!ctx_ || !ctx_->device)
        return;
    onFrameComplete();
    destroyTarget();
    if (desc_pool_)
        vkDestroyDescriptorPool(ctx_->device, desc_pool_, nullptr);
    if (pipeline_)
        vkDestroyPipeline(ctx_->device, pipeline_, nullptr);
    if (pipe_layout_)
        vkDestroyPipelineLayout(ctx_->device, pipe_layout_, nullptr);
    if (set_layout_)
        vkDestroyDescriptorSetLayout(ctx_->device, set_layout_, nullptr);
    if (sampler_)
        vkDestroySampler(ctx_->device, sampler_, nullptr);
    desc_pool_ = VK_NULL_HANDLE;
    desc_set_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    pipe_layout_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
}

bool BlendInterpolator::ensureTarget(uint32_t width, uint32_t height) {
    if (out_image_ && width == width_ && height == height_)
        return true;
    // The previous frame is fence-waited before record() runs again, so the
    // old target is idle and safe to destroy.
    destroyTarget();

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = OUT_FORMAT;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(ctx_->device, &ici, nullptr, &out_image_) !=
        VK_SUCCESS) {
        logError(TAG, "output image creation failed (%ux%u)", width, height);
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(ctx_->device, out_image_, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = ctx_->findMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(ctx_->device, &mai, nullptr, &out_memory_) !=
            VK_SUCCESS ||
        vkBindImageMemory(ctx_->device, out_image_, out_memory_, 0) !=
            VK_SUCCESS) {
        logError(TAG, "output image memory allocation failed");
        return false;
    }
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = out_image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = OUT_FORMAT;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(ctx_->device, &vci, nullptr, &out_view_) !=
        VK_SUCCESS) {
        logError(TAG, "output image view creation failed");
        return false;
    }
    width_ = width;
    height_ = height;
    out_initialized_ = false;
    logInfo(TAG, "blend target: %ux%u", width, height);
    return true;
}

VkImage BlendInterpolator::record(VkCommandBuffer cmd, VkImage a, VkImage b,
                                  VkFormat src_format, uint32_t width,
                                  uint32_t height, float phase) {
    if (!pipeline_ || !ensureTarget(width, height))
        return VK_NULL_HANDLE;

    // Per-frame views of the pair (the leased slots differ every frame).
    // The previous frame's views were released in onFrameComplete.
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = src_format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vci.image = a;
    if (vkCreateImageView(ctx_->device, &vci, nullptr, &view_a_) !=
        VK_SUCCESS)
        return VK_NULL_HANDLE;
    vci.image = b;
    if (vkCreateImageView(ctx_->device, &vci, nullptr, &view_b_) !=
        VK_SUCCESS)
        return VK_NULL_HANDLE;

    VkDescriptorImageInfo img_a{sampler_, view_a_,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo img_b{sampler_, view_b_,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo img_out{VK_NULL_HANDLE, out_view_,
                                  VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[3]{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, desc_set_,
                 0,       0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                 &img_a,  nullptr, nullptr};
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, desc_set_,
                 1,       0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                 &img_b,  nullptr, nullptr};
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, desc_set_,
                 2,       0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                 &img_out, nullptr, nullptr};
    vkUpdateDescriptorSets(ctx_->device, 3, writes, 0, nullptr);

    // Pair: TRANSFER_SRC -> SHADER_READ_ONLY. Output: (UNDEFINED on first
    // use, else TRANSFER_SRC) -> GENERAL for storage writes.
    VkImageMemoryBarrier pre[3]{};
    for (auto& bar : pre) {
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    pre[0].srcAccessMask = 0;
    pre[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    pre[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    pre[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    pre[0].image = a;
    pre[1] = pre[0];
    pre[1].image = b;
    pre[2].srcAccessMask = 0;
    pre[2].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    pre[2].oldLayout = out_initialized_ ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                        : VK_IMAGE_LAYOUT_UNDEFINED;
    pre[2].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre[2].image = out_image_;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 3, pre);
    out_initialized_ = true;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout_,
                            0, 1, &desc_set_, 0, nullptr);
    vkCmdPushConstants(cmd, pipe_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(float), &phase);
    vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);

    // Pair back to TRANSFER_SRC for passthrough blits; output becomes a
    // blit source.
    VkImageMemoryBarrier post[3] = {pre[0], pre[1], pre[2]};
    post[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    post[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    post[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    post[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    post[1] = post[0];
    post[1].image = b;
    post[2].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    post[2].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    post[2].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    post[2].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    post[2].image = out_image_;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 3, post);
    return out_image_;
}

void BlendInterpolator::onFrameComplete() {
    if (view_a_)
        vkDestroyImageView(ctx_->device, view_a_, nullptr);
    if (view_b_)
        vkDestroyImageView(ctx_->device, view_b_, nullptr);
    view_a_ = view_b_ = VK_NULL_HANDLE;
}

} // namespace lsfg::vk
