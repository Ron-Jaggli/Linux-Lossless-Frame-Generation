#include "vk/interpolate.hpp"

#include "log.hpp"
#include "vk/context.hpp"

#include "shaders/blend_comp_spv.h"

namespace lsfg::vk {

static const char* TAG = "interp";

// The output format is our choice (nothing imports it), so pick one with
// universal storage-image and blit-source support.
static constexpr VkFormat DST_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;

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
    for (uint32_t i = 0; i < 2; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[i].pImmutableSamplers = &sampler_;
    }
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dlci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = 3;
    dlci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(ctx.device, &dlci, nullptr,
                                    &dset_layout_) != VK_SUCCESS) {
        logError(TAG, "vkCreateDescriptorSetLayout failed");
        return false;
    }

    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float)};
    VkPipelineLayoutCreateInfo plci{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dset_layout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &push;
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
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = module;
    cpci.stage.pName = "main";
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
    if (vkCreateDescriptorPool(ctx.device, &dpci, nullptr, &dpool_) !=
        VK_SUCCESS) {
        logError(TAG, "vkCreateDescriptorPool failed");
        return false;
    }
    VkDescriptorSetAllocateInfo dsai{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = dpool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dset_layout_;
    if (vkAllocateDescriptorSets(ctx.device, &dsai, &dset_) != VK_SUCCESS) {
        logError(TAG, "vkAllocateDescriptorSets failed");
        return false;
    }
    return true;
}

bool BlendInterpolator::ensureOutput(uint32_t width, uint32_t height) {
    if (dst_image_ && extent_.width == width && extent_.height == height)
        return true;
    destroyOutput();

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = DST_FORMAT;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(ctx_->device, &ici, nullptr, &dst_image_) != VK_SUCCESS) {
        logError(TAG, "output vkCreateImage failed (%ux%u)", width, height);
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(ctx_->device, dst_image_, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = ctx_->findMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(ctx_->device, &mai, nullptr, &dst_memory_) !=
            VK_SUCCESS ||
        vkBindImageMemory(ctx_->device, dst_image_, dst_memory_, 0) !=
            VK_SUCCESS) {
        logError(TAG, "output memory allocation failed");
        return false;
    }
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = dst_image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = DST_FORMAT;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(ctx_->device, &vci, nullptr, &dst_view_) !=
        VK_SUCCESS) {
        logError(TAG, "output vkCreateImageView failed");
        return false;
    }
    extent_ = {width, height};
    logInfo(TAG, "blend output: %ux%u", width, height);
    return true;
}

void BlendInterpolator::destroyOutput() {
    if (dst_view_)
        vkDestroyImageView(ctx_->device, dst_view_, nullptr);
    if (dst_image_)
        vkDestroyImage(ctx_->device, dst_image_, nullptr);
    if (dst_memory_)
        vkFreeMemory(ctx_->device, dst_memory_, nullptr);
    dst_view_ = VK_NULL_HANDLE;
    dst_image_ = VK_NULL_HANDLE;
    dst_memory_ = VK_NULL_HANDLE;
    extent_ = {};
}

bool BlendInterpolator::record(VkCommandBuffer cmd,
                               const FramePool::PairLease& pair, float phase) {
    if (!ensureOutput(pair.width, pair.height))
        return false;

    // The caller fence-waits every submission, so the set is idle here.
    VkDescriptorImageInfo imgs[3]{};
    imgs[0] = {VK_NULL_HANDLE, pair.view_a,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imgs[1] = {VK_NULL_HANDLE, pair.view_b,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imgs[2] = {VK_NULL_HANDLE, dst_view_, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = dset_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType =
            i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                  : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &imgs[i];
    }
    vkUpdateDescriptorSets(ctx_->device, 3, writes, 0, nullptr);

    // Pool images live in TRANSFER_SRC_OPTIMAL between uses; borrow them as
    // shader inputs and hand them back in the same layout.
    VkImageMemoryBarrier barriers[3]{};
    for (auto& b : barriers) {
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].image = pair.image_a;
    barriers[1] = barriers[0];
    barriers[1].image = pair.image_b;
    barriers[2].srcAccessMask = 0;
    barriers[2].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[2].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[2].image = dst_image_;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 3, barriers);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout_,
                            0, 1, &dset_, 0, nullptr);
    vkCmdPushConstants(cmd, pipe_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(float), &phase);
    vkCmdDispatch(cmd, (pair.width + 7) / 8, (pair.height + 7) / 8, 1);

    barriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].image = pair.image_a;
    barriers[1] = barriers[0];
    barriers[1].image = pair.image_b;
    barriers[2].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[2].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[2].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[2].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[2].image = dst_image_;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 3, barriers);
    return true;
}

void BlendInterpolator::destroy() {
    if (!ctx_ || !ctx_->device)
        return;
    destroyOutput();
    if (dpool_)
        vkDestroyDescriptorPool(ctx_->device, dpool_, nullptr);
    if (pipeline_)
        vkDestroyPipeline(ctx_->device, pipeline_, nullptr);
    if (pipe_layout_)
        vkDestroyPipelineLayout(ctx_->device, pipe_layout_, nullptr);
    if (dset_layout_)
        vkDestroyDescriptorSetLayout(ctx_->device, dset_layout_, nullptr);
    if (sampler_)
        vkDestroySampler(ctx_->device, sampler_, nullptr);
    dpool_ = VK_NULL_HANDLE;
    dset_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    pipe_layout_ = VK_NULL_HANDLE;
    dset_layout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
}

} // namespace lsfg::vk
