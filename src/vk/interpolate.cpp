#include "vk/interpolate.hpp"

#include "log.hpp"
#include "vk/context.hpp"

#include "shaders/blend.comp.spv.h"

namespace lsfg::vk {

static const char* TAG = "interp";

bool BlendInterpolator::init(Context& ctx) {
    ctx_ = &ctx;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(ctx.device, &sci, nullptr, &sampler_) != VK_SUCCESS)
        return false;

    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i < 2; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
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
                                    &dset_layout_) != VK_SUCCESS)
        return false;

    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float)};
    VkPipelineLayoutCreateInfo plci{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dset_layout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(ctx.device, &plci, nullptr, &pipe_layout_) !=
        VK_SUCCESS)
        return false;

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = sizeof(blend_comp_spv);
    smci.pCode = blend_comp_spv;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(ctx.device, &smci, nullptr, &module) !=
        VK_SUCCESS)
        return false;
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
        logError(TAG, "blend compute pipeline creation failed (%d)", res);
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
    if (vkCreateDescriptorPool(ctx.device, &dpci, nullptr, &dset_pool_) !=
        VK_SUCCESS)
        return false;
    VkDescriptorSetAllocateInfo dsai{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = dset_pool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dset_layout_;
    if (vkAllocateDescriptorSets(ctx.device, &dsai, &dset_) != VK_SUCCESS)
        return false;
    return true;
}

bool BlendInterpolator::ensureOutput(uint32_t width, uint32_t height) {
    if (out_image_ && out_width_ == width && out_height_ == height)
        return true;
    destroyOutput();

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(ctx_->device, &ici, nullptr, &out_image_) != VK_SUCCESS) {
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
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(ctx_->device, &vci, nullptr, &out_view_) !=
        VK_SUCCESS)
        return false;

    out_width_ = width;
    out_height_ = height;
    logInfo(TAG, "blend output image: %ux%u", width, height);
    return true;
}

// The pair rotates through the pool almost every frame, and pool recreation
// can reuse image handles, so caching views by handle is not worth the risk:
// recreate them per record. The previous frame's views are idle by the
// caller's fence-wait contract.
bool BlendInterpolator::ensureInputViews(VkImage a, VkImage b,
                                         VkFormat format) {
    destroyInputViews();

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vci.image = a;
    if (vkCreateImageView(ctx_->device, &vci, nullptr, &view_a_) != VK_SUCCESS)
        return false;
    vci.image = b;
    if (vkCreateImageView(ctx_->device, &vci, nullptr, &view_b_) != VK_SUCCESS)
        return false;
    return true;
}

bool BlendInterpolator::record(VkCommandBuffer cmd, VkImage a, VkImage b,
                               VkFormat src_format, uint32_t width,
                               uint32_t height, float phase) {
    if (!ensureOutput(width, height) || !ensureInputViews(a, b, src_format))
        return false;

    // Rebind every frame: the pair images rotate through the pool. The set
    // is idle here because the caller fence-waits each frame.
    VkDescriptorImageInfo in_a{sampler_, view_a_,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo in_b{sampler_, view_b_,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo out{VK_NULL_HANDLE, out_view_,
                              VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[3]{};
    for (int i = 0; i < 3; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = dset_;
        writes[i].dstBinding = uint32_t(i);
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &in_a;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &in_b;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &out;
    vkUpdateDescriptorSets(ctx_->device, 3, writes, 0, nullptr);

    // Inputs: TRANSFER_SRC -> SHADER_READ; output: whatever -> GENERAL.
    VkImageMemoryBarrier barriers[3]{};
    for (int i = 0; i < 3; i++) {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].image = a;
    barriers[1] = barriers[0];
    barriers[1].image = b;
    barriers[2].srcAccessMask = 0;
    barriers[2].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // contents replaced
    barriers[2].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[2].image = out_image_;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 3, barriers);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout_,
                            0, 1, &dset_, 0, nullptr);
    vkCmdPushConstants(cmd, pipe_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(float), &phase);
    vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);

    // Inputs back to TRANSFER_SRC for the pool; output to TRANSFER_SRC for
    // the presentation blit.
    barriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[1] = barriers[0];
    barriers[1].image = b;
    barriers[0].image = a;
    barriers[2].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[2].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[2].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[2].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 3, barriers);
    return true;
}

void BlendInterpolator::destroyOutput() {
    if (out_view_)
        vkDestroyImageView(ctx_->device, out_view_, nullptr);
    if (out_image_)
        vkDestroyImage(ctx_->device, out_image_, nullptr);
    if (out_memory_)
        vkFreeMemory(ctx_->device, out_memory_, nullptr);
    out_view_ = VK_NULL_HANDLE;
    out_image_ = VK_NULL_HANDLE;
    out_memory_ = VK_NULL_HANDLE;
    out_width_ = out_height_ = 0;
}

void BlendInterpolator::destroyInputViews() {
    if (view_a_)
        vkDestroyImageView(ctx_->device, view_a_, nullptr);
    if (view_b_)
        vkDestroyImageView(ctx_->device, view_b_, nullptr);
    view_a_ = view_b_ = VK_NULL_HANDLE;
}

void BlendInterpolator::destroy() {
    if (!ctx_ || !ctx_->device)
        return;
    vkDeviceWaitIdle(ctx_->device);
    destroyInputViews();
    destroyOutput();
    if (dset_pool_)
        vkDestroyDescriptorPool(ctx_->device, dset_pool_, nullptr);
    if (pipeline_)
        vkDestroyPipeline(ctx_->device, pipeline_, nullptr);
    if (pipe_layout_)
        vkDestroyPipelineLayout(ctx_->device, pipe_layout_, nullptr);
    if (dset_layout_)
        vkDestroyDescriptorSetLayout(ctx_->device, dset_layout_, nullptr);
    if (sampler_)
        vkDestroySampler(ctx_->device, sampler_, nullptr);
    dset_pool_ = VK_NULL_HANDLE;
    dset_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    pipe_layout_ = VK_NULL_HANDLE;
    dset_layout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
}

} // namespace lsfg::vk
