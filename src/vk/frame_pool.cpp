#include "vk/frame_pool.hpp"

#include "log.hpp"
#include "vk/context.hpp"

#include <cassert>

namespace lsfg::vk {

static const char* TAG = "pool";

bool FramePool::create(Context& ctx, VkFormat format, uint32_t width,
                       uint32_t height) {
    ctx_ = &ctx;
    std::unique_lock lock(mutex_);
    return createSlotsLocked(format, width, height);
}

bool FramePool::recreate(VkFormat format, uint32_t width, uint32_t height) {
    std::unique_lock lock(mutex_);
    reader_done_.wait(lock, [&] { return reading_ == -1 && pair_a_ == -1; });
    // The reader fence-waits its blit before releasing, and the writer
    // fence-waits before publishing, so no GPU work references these images.
    destroySlotsLocked();
    latest_ = -1;
    unique_latest_ = unique_prev_ = -1;
    return createSlotsLocked(format, width, height);
}

bool FramePool::createSlotsLocked(VkFormat format, uint32_t width,
                                  uint32_t height) {
    for (auto& slot : slots_) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = format;
        ici.extent = {width, height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(ctx_->device, &ici, nullptr, &slot.image) != VK_SUCCESS) {
            logError(TAG, "vkCreateImage failed (%ux%u)", width, height);
            return false;
        }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(ctx_->device, slot.image, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = ctx_->findMemoryType(
            req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(ctx_->device, &mai, nullptr, &slot.memory) !=
                VK_SUCCESS ||
            vkBindImageMemory(ctx_->device, slot.image, slot.memory, 0) !=
                VK_SUCCESS) {
            logError(TAG, "image memory allocation failed");
            return false;
        }
    }
    format_ = format;
    width_ = width;
    height_ = height;
    logInfo(TAG, "frame pool: %dx %ux%u", SLOT_COUNT, width, height);
    return true;
}

void FramePool::destroySlotsLocked() {
    for (auto& slot : slots_) {
        if (slot.image)
            vkDestroyImage(ctx_->device, slot.image, nullptr);
        if (slot.memory)
            vkFreeMemory(ctx_->device, slot.memory, nullptr);
        slot = {};
    }
    width_ = height_ = 0;
}

void FramePool::destroy() {
    if (!ctx_)
        return;
    std::unique_lock lock(mutex_);
    destroySlotsLocked();
    latest_ = -1;
    reading_ = -1;
    unique_latest_ = unique_prev_ = -1;
    pair_a_ = pair_b_ = -1;
}

int FramePool::acquireWrite() {
    std::unique_lock lock(mutex_);
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (i != latest_ && i != reading_ && i != unique_latest_ &&
            i != unique_prev_ && i != pair_a_ && i != pair_b_)
            return i;
    }
    assert(!"FramePool::acquireWrite: no free slot");
    return 0; // unreachable: at most 5 slots can be excluded
}

void FramePool::publish(int index, uint64_t seq, double t_capture,
                        bool unique) {
    std::unique_lock lock(mutex_);
    slots_[index].seq = seq;
    slots_[index].t_capture = t_capture;
    latest_ = index;
    if (unique) {
        unique_prev_ = unique_latest_;
        unique_latest_ = index;
    }
}

FramePool::ReadLease FramePool::acquireRead() {
    std::unique_lock lock(mutex_);
    ReadLease lease;
    if (latest_ < 0 || !valid())
        return lease;
    reading_ = latest_;
    lease.index = latest_;
    lease.image = slots_[latest_].image;
    lease.width = width_;
    lease.height = height_;
    lease.seq = slots_[latest_].seq;
    lease.t_capture = slots_[latest_].t_capture;
    return lease;
}

void FramePool::releaseRead(const ReadLease& lease) {
    if (lease.index < 0)
        return;
    std::unique_lock lock(mutex_);
    reading_ = -1;
    reader_done_.notify_all();
}

FramePool::PairLease FramePool::acquirePairRead() {
    std::unique_lock lock(mutex_);
    PairLease lease;
    if (unique_prev_ < 0 || !valid())
        return lease;
    pair_a_ = unique_prev_;
    pair_b_ = unique_latest_;
    lease.index_a = pair_a_;
    lease.index_b = pair_b_;
    lease.image_a = slots_[pair_a_].image;
    lease.image_b = slots_[pair_b_].image;
    lease.seq_a = slots_[pair_a_].seq;
    lease.seq_b = slots_[pair_b_].seq;
    lease.t_capture_a = slots_[pair_a_].t_capture;
    lease.t_capture_b = slots_[pair_b_].t_capture;
    lease.width = width_;
    lease.height = height_;
    return lease;
}

void FramePool::releasePairRead(const PairLease& lease) {
    if (!lease.valid())
        return;
    std::unique_lock lock(mutex_);
    pair_a_ = pair_b_ = -1;
    reader_done_.notify_all();
}

uint64_t FramePool::latestSeq() {
    std::unique_lock lock(mutex_);
    return latest_ >= 0 ? slots_[latest_].seq : 0;
}

} // namespace lsfg::vk
