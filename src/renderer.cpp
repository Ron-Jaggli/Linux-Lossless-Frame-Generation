#include "renderer.hpp"

#include "log.hpp"
#include "vk/context.hpp"
#include "vk/frame_pool.hpp"

#include <algorithm>
#include <mutex>

namespace lsfg {

static const char* TAG = "render";

bool Renderer::init(vk::Context& ctx, vk::FramePool& pool) {
    ctx_ = &ctx;
    pool_ = &pool;

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ctx.queue_family;
    if (vkCreateCommandPool(ctx.device, &pci, nullptr, &cmd_pool_) != VK_SUCCESS)
        return false;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = cmd_pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(ctx.device, &cai, &cmd_) != VK_SUCCESS)
        return false;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(ctx.device, &fci, nullptr, &fence_) != VK_SUCCESS)
        return false;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(ctx.device, &sci, nullptr, &sem_acquire_) != VK_SUCCESS ||
        vkCreateSemaphore(ctx.device, &sci, nullptr, &sem_render_) != VK_SUCCESS)
        return false;
    return true;
}

void Renderer::enableFrameGen(FramePacer* pacer, std::mutex* pacer_mutex) {
    pacer_ = pacer;
    pacer_mutex_ = pacer_mutex;
    interp_ready_ = interp_.init(*ctx_);
    if (!interp_ready_)
        logWarn(TAG, "interpolator setup failed; staying passthrough");
}

void Renderer::destroy() {
    if (!ctx_ || !ctx_->device)
        return;
    vkDeviceWaitIdle(ctx_->device);
    if (interp_ready_ || pacer_)
        interp_.destroy();
    if (sem_acquire_)
        vkDestroySemaphore(ctx_->device, sem_acquire_, nullptr);
    if (sem_render_)
        vkDestroySemaphore(ctx_->device, sem_render_, nullptr);
    if (fence_)
        vkDestroyFence(ctx_->device, fence_, nullptr);
    if (cmd_pool_)
        vkDestroyCommandPool(ctx_->device, cmd_pool_, nullptr);
    sem_acquire_ = sem_render_ = VK_NULL_HANDLE;
    fence_ = VK_NULL_HANDLE;
    cmd_pool_ = VK_NULL_HANDLE;
}

bool Renderer::drawFrame() {
    if (needs_recreate_) {
        vkDeviceWaitIdle(ctx_->device);
        if (!ctx_->createSwapchain())
            return true; // window probably minimized; retry next frame
        needs_recreate_ = false;
    }

    uint32_t image_index = 0;
    VkResult res = vkAcquireNextImageKHR(ctx_->device, ctx_->swapchain,
                                         100000000ull, // 100ms
                                         sem_acquire_, VK_NULL_HANDLE,
                                         &image_index);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
        needs_recreate_ = true;
        return true;
    }
    if (res == VK_TIMEOUT || res == VK_NOT_READY)
        return true;
    if (res != VK_SUCCESS) {
        logError(TAG, "vkAcquireNextImageKHR failed (%d)", res);
        return false;
    }

    // What to show this refresh: a pacer decision when frame generation is
    // on, otherwise (or on any fallback) the latest real frame.
    PaceDecision dec;
    if (fg_enabled_ && pacer_ && interp_ready_) {
        std::lock_guard lock(*pacer_mutex_);
        dec = pacer_->decide(nowSeconds());
    }

    vk::FramePool::ReadLease lease;
    vk::FramePool::PairLease pair;
    VkImage src_image = VK_NULL_HANDLE;
    uint32_t src_width = 0, src_height = 0;
    double content_t = 0.0;
    bool run_interp = false;

    if (dec.mode == PaceMode::Interpolate) {
        pair = pool_->acquirePairRead();
        if (pair.valid()) {
            if (dec.phase > 0.0) {
                run_interp = true; // source image set after recording
            } else {
                // phase 0 is exactly frame A; no compute pass needed
                src_image = pair.image_a;
                src_width = pair.width;
                src_height = pair.height;
                content_t = pair.t_capture_a;
            }
        }
    }
    if (!pair.valid()) {
        lease = pool_->acquireRead();
        if (lease.index >= 0) {
            src_image = lease.image;
            src_width = lease.width;
            src_height = lease.height;
            content_t = lease.t_capture;
        }
    }

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);

    if (run_interp) {
        if (interp_.record(cmd_, pair.image_a, pair.image_b, pool_->format(),
                           pair.width, pair.height, float(dec.phase))) {
            src_image = interp_.output();
            src_width = pair.width;
            src_height = pair.height;
            content_t = pair.t_capture_a +
                        dec.phase * (pair.t_capture_b - pair.t_capture_a);
        } else {
            run_interp = false; // record() fails before emitting any command
            src_image = pair.image_b;
            src_width = pair.width;
            src_height = pair.height;
            content_t = pair.t_capture_b;
        }
    }

    VkImage swap_img = ctx_->swap_images[image_index];
    VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = swap_img;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_dst);

    VkClearColorValue clear{{0.05f, 0.05f, 0.05f, 1.0f}};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd_, swap_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clear, 1, &range);

    if (src_image != VK_NULL_HANDLE) {
        // letterboxed fit
        double sw = ctx_->swap_extent.width, sh = ctx_->swap_extent.height;
        double scale = std::min(sw / src_width, sh / src_height);
        int32_t dw = int32_t(src_width * scale);
        int32_t dh = int32_t(src_height * scale);
        int32_t dx = (int32_t(sw) - dw) / 2;
        int32_t dy = (int32_t(sh) - dh) / 2;

        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(src_width), int32_t(src_height), 1};
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[0] = {dx, dy, 0};
        blit.dstOffsets[1] = {dx + dw, dy + dh, 1};
        vkCmdBlitImage(cmd_, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       swap_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
    }

    VkImageMemoryBarrier to_present = to_dst;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_present);
    vkEndCommandBuffer(cmd_);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &sem_acquire_;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &sem_render_;
    {
        std::lock_guard lock(ctx_->queue_mutex);
        if (vkQueueSubmit(ctx_->queue, 1, &si, fence_) != VK_SUCCESS) {
            logError(TAG, "vkQueueSubmit failed");
            pool_->releaseRead(lease);
            pool_->releasePairRead(pair);
            return false;
        }
    }
    // Wait for the GPU work before releasing the pool lease(s); keeps
    // capture-side recreation safe and the interpolator's per-frame
    // resources hazard-free. Cheap at these workloads.
    vkWaitForFences(ctx_->device, 1, &fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(ctx_->device, 1, &fence_);
    pool_->releaseRead(lease);
    pool_->releasePairRead(pair);

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &sem_render_;
    pi.swapchainCount = 1;
    pi.pSwapchains = &ctx_->swapchain;
    pi.pImageIndices = &image_index;
    {
        std::lock_guard lock(ctx_->queue_mutex);
        res = vkQueuePresentKHR(ctx_->queue, &pi);
    }
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
        needs_recreate_ = true;
    else if (res != VK_SUCCESS) {
        logError(TAG, "vkQueuePresentKHR failed (%d)", res);
        return false;
    }

    presented_++;
    if (run_interp)
        interpolated_++;
    if (src_image != VK_NULL_HANDLE) {
        // For an in-between, content time sits between the pair's captures,
        // so this honestly includes the one-source-period interpolation hold.
        double lat = (nowSeconds() - content_t) * 1000.0;
        latency_ms_ = latency_ms_ < 0 ? lat : latency_ms_ * 0.9 + lat * 0.1;
    }
    return true;
}

} // namespace lsfg
