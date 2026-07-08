#include "renderer.hpp"

#include "log.hpp"
#include "vk/context.hpp"
#include "vk/frame_pool.hpp"
#include "vk/interpolate.hpp"

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

void Renderer::enableFrameGen(FramePacer& pacer, std::mutex& pacer_mutex,
                              vk::Interpolator& interp) {
    pacer_ = &pacer;
    pacer_mutex_ = &pacer_mutex;
    interp_ = &interp;
}

void Renderer::destroy() {
    if (!ctx_ || !ctx_->device)
        return;
    vkDeviceWaitIdle(ctx_->device);
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

    PaceDecision decision;
    decision.mode = PaceDecision::Mode::Passthrough;
    if (pacer_) {
        std::lock_guard lock(*pacer_mutex_);
        decision = pacer_->decide(nowSeconds());
    }

    // Interpolation needs the unique pair; anything else shows the latest
    // real frame. An unavailable pair (startup, pool just recreated) falls
    // back to passthrough for this refresh.
    vk::FramePool::PairLease pair;
    vk::FramePool::ReadLease lease;
    bool interpolating = false;
    if (decision.mode == PaceDecision::Mode::Interpolate && interp_) {
        pair = pool_->acquirePairRead();
        interpolating = pair.index_b >= 0;
    }
    if (!interpolating)
        lease = pool_->acquireRead();

    auto releaseLeases = [&] {
        if (interpolating)
            pool_->releasePairRead(pair);
        else
            pool_->releaseRead(lease);
    };

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);

    // Pick the blit source: pool frame, exact pair frame A (step 0 needs no
    // compute), or the interpolator's output.
    VkImage src = VK_NULL_HANDLE;
    uint32_t src_w = 0, src_h = 0;
    double t_content = 0.0;
    if (interpolating) {
        src_w = pair.width;
        src_h = pair.height;
        t_content = pair.t_capture_a +
                    decision.phase * (pair.t_capture_b - pair.t_capture_a);
        if (decision.step == 0) {
            src = pair.image_a;
        } else if (interp_->record(cmd_, pair, float(decision.phase))) {
            src = interp_->result();
        } else {
            src = pair.image_b; // resource failure: newest real frame
        }
    } else if (lease.index >= 0) {
        src = lease.image;
        src_w = lease.width;
        src_h = lease.height;
        t_content = lease.t_capture;
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

    if (src != VK_NULL_HANDLE) {
        // letterboxed fit
        double sw = ctx_->swap_extent.width, sh = ctx_->swap_extent.height;
        double scale = std::min(sw / src_w, sh / src_h);
        int32_t dw = int32_t(src_w * scale);
        int32_t dh = int32_t(src_h * scale);
        int32_t dx = (int32_t(sw) - dw) / 2;
        int32_t dy = (int32_t(sh) - dh) / 2;

        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(src_w), int32_t(src_h), 1};
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[0] = {dx, dy, 0};
        blit.dstOffsets[1] = {dx + dw, dy + dh, 1};
        vkCmdBlitImage(cmd_, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
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
            releaseLeases();
            return false;
        }
    }
    // Wait for the GPU work before releasing the pool lease(s); keeps
    // capture-side recreation safe and the interpolator's per-frame
    // resources idle for the next record.
    vkWaitForFences(ctx_->device, 1, &fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(ctx_->device, 1, &fence_);
    releaseLeases();

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
    if (src != VK_NULL_HANDLE) {
        uint64_t seq_a = interpolating ? pair.seq_a : lease.seq;
        uint64_t seq_b = interpolating ? pair.seq_b : lease.seq;
        int step = interpolating ? decision.step : 0;
        bool same = have_last_shown_ && interpolating == last_was_interp_ &&
                    seq_a == last_seq_a_ && seq_b == last_seq_b_ &&
                    step == last_step_;
        if (!same)
            outputs_++;
        have_last_shown_ = true;
        last_was_interp_ = interpolating;
        last_seq_a_ = seq_a;
        last_seq_b_ = seq_b;
        last_step_ = step;

        double lat = (nowSeconds() - t_content) * 1000.0;
        latency_ms_ = latency_ms_ < 0 ? lat : latency_ms_ * 0.9 + lat * 0.1;
    }
    return true;
}

} // namespace lsfg
