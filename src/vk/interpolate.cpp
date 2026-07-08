#include "vk/interpolate.hpp"

#include "log.hpp"
#include "vk/context.hpp"

#include "shaders/blend_comp_spv.h"

namespace lsfg::vk {

static const char* TAG = "interp";

bool BlendInterpolator::init(Context& ctx) {
    ctx_ = &ctx;

    // texelFetch ignores the sampler state, but combined image samplers
    // still need one bound.
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
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
        bindings[i].pImmutableSamplers = &sampler_;
    }
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo lci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 3;
    lci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(ctx.device, &lci, nullptr, &set_layout_) !=
        VK_SUCCESS)
        return false;

    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float)};
    VkPipelineLayoutCreateInfo plci{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(ctx.device, &plci, nullptr, &pipeline_layout_) !=
        VK_SUCCESS)
        return false;

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = sizeof(blend_comp_spv);
    smci.pCode = blend_comp_spv;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(ctx.device, &smci, nullptr, &module) != VK_SUCCESS)
        return false;

    VkComputePipelineCreateInfo pci{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pci.stage.module = module;
    pci.stage.pName = "main";
    pci.layout = pipeline_layout_;
    VkResult res = vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &pci,
                                            nullptr, &pipeline_);
    vkDestroyShaderModule(ctx.device, module, nullptr);
    if (res != VK_SUCCESS)
        return false;

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
        VK_SUCCESS)
        return false;
    VkDescriptorSetAllocateInfo dsai{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = desc_pool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &set_layout_;
    if (vkAllocateDescriptorSets(ctx.device, &dsai, &desc_set_) != VK_SUCCESS)
        return false;

    logInfo(TAG, "blend interpolator ready");
    return true;
}

bool BlendInterpolator::ensureIntermediate(uint32_t width, uint32_t height) {
    if (dst_image_ && dst_width_ == width && dst_height_ == height)
        return true;
    destroyIntermediate();

    // Own format, independent of the source: R8G8B8A8_UNORM storage-image
    // support is mandatory in Vulkan; the blit to the swapchain converts.
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
    if (vkCreateImage(ctx_->device, &ici, nullptr, &dst_image_) != VK_SUCCESS) {
        logError(TAG, "intermediate image creation failed (%ux%u)", width,
                 height);
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
        logError(TAG, "intermediate image memory allocation failed");
        return false;
    }

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = dst_image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(ctx_->device, &vci, nullptr, &dst_view_) !=
        VK_SUCCESS)
        return false;
    dst_width_ = width;
    dst_height_ = height;
    return true;
}

VkImage BlendInterpolator::record(VkCommandBuffer cmd, VkImage a, VkImage b,
                                  VkFormat src_format, uint32_t width,
                                  uint32_t height, float phase) {
    if (!ensureIntermediate(width, height))
        return VK_NULL_HANDLE;
    destroySourceViews(); // last record()'s submission has retired

    VkImage srcs[2] = {a, b};
    for (int i = 0; i < 2; i++) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = srcs[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = src_format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(ctx_->device, &vci, nullptr, &src_views_[i]) !=
            VK_SUCCESS)
            return VK_NULL_HANDLE;
    }

    VkDescriptorImageInfo img_infos[3]{};
    img_infos[0] = {VK_NULL_HANDLE, src_views_[0],
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    img_infos[1] = {VK_NULL_HANDLE, src_views_[1],
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    img_infos[2] = {VK_NULL_HANDLE, dst_view_, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = desc_set_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType =
            i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                  : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &img_infos[i];
    }
    vkUpdateDescriptorSets(ctx_->device, 3, writes, 0, nullptr);

    // Sources: TRANSFER_SRC -> shader read; dst: undefined -> GENERAL.
    VkImageMemoryBarrier barriers[3]{};
    for (int i = 0; i < 2; i++) {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].image = srcs[i];
        barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    barriers[2] = barriers[0];
    barriers[2].srcAccessMask = 0;
    barriers[2].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[2].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[2].image = dst_image_;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 3, barriers);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout_, 0, 1, &desc_set_, 0, nullptr);
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(float), &phase);
    vkCmdDispatch(cmd, (width + 15) / 16, (height + 15) / 16, 1);

    // Sources back to TRANSFER_SRC (pool invariant); dst becomes a blit
    // source for the swapchain copy.
    for (int i = 0; i < 2; i++) {
        barriers[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    barriers[2].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[2].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[2].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[2].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 3, barriers);
    return dst_image_;
}

void BlendInterpolator::destroySourceViews() {
    for (auto& v : src_views_) {
        if (v)
            vkDestroyImageView(ctx_->device, v, nullptr);
        v = VK_NULL_HANDLE;
    }
}

void BlendInterpolator::destroyIntermediate() {
    if (dst_view_)
        vkDestroyImageView(ctx_->device, dst_view_, nullptr);
    if (dst_image_)
        vkDestroyImage(ctx_->device, dst_image_, nullptr);
    if (dst_memory_)
        vkFreeMemory(ctx_->device, dst_memory_, nullptr);
    dst_view_ = VK_NULL_HANDLE;
    dst_image_ = VK_NULL_HANDLE;
    dst_memory_ = VK_NULL_HANDLE;
    dst_width_ = dst_height_ = 0;
}

void BlendInterpolator::destroy() {
    if (!ctx_ || !ctx_->device)
        return;
    destroySourceViews();
    destroyIntermediate();
    if (desc_pool_)
        vkDestroyDescriptorPool(ctx_->device, desc_pool_, nullptr);
    if (pipeline_)
        vkDestroyPipeline(ctx_->device, pipeline_, nullptr);
    if (pipeline_layout_)
        vkDestroyPipelineLayout(ctx_->device, pipeline_layout_, nullptr);
    if (set_layout_)
        vkDestroyDescriptorSetLayout(ctx_->device, set_layout_, nullptr);
    if (sampler_)
        vkDestroySampler(ctx_->device, sampler_, nullptr);
    desc_pool_ = VK_NULL_HANDLE;
    desc_set_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
}

} // namespace lsfg::vk
