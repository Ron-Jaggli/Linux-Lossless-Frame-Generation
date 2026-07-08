#include "vk/frame_pool.hpp"

#include "log.hpp"
#include "vk/context.hpp"

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
    reader_done_.wait(lock,
                      [&] { return reading_ == -1 && pair_a_ == -1; });
    // The reader fence-waits its blit before releasing, and the writer
    // fence-waits before publishing, so no GPU work references these images.
    destroySlotsLocked();
    latest_ = -1;
    unique0_ = unique1_ = -1;
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
    logInfo(TAG, "frame pool: %dx %ux%u", kSlots, width, height);
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
    unique0_ = unique1_ = -1;
    pair_a_ = pair_b_ = -1;
}

int FramePool::acquireWrite() {
    std::unique_lock lock(mutex_);
    for (int i = 0; i < kSlots; i++) {
        if (i != latest_ && i != reading_ && i != unique0_ && i != unique1_ &&
            i != pair_a_ && i != pair_b_)
            return i;
    }
    // Unreachable: at most 5 slots are ever protected at once (latest, the
    // two current uniques, and a held pair; the reader holds either a
    // single lease or a pair, never both).
    return 0;
}

void FramePool::publish(int index, uint64_t seq, double t_capture,
                        bool unique) {
    std::unique_lock lock(mutex_);
    slots_[index].seq = seq;
    slots_[index].t_capture = t_capture;
    latest_ = index;
    if (unique) {
        unique1_ = unique0_;
        unique0_ = index;
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
    if (unique1_ < 0 || !valid())
        return lease;
    pair_a_ = unique1_;
    pair_b_ = unique0_;
    auto fill = [&](ReadLease& l, int idx) {
        l.index = idx;
        l.image = slots_[idx].image;
        l.width = width_;
        l.height = height_;
        l.seq = slots_[idx].seq;
        l.t_capture = slots_[idx].t_capture;
    };
    fill(lease.a, pair_a_);
    fill(lease.b, pair_b_);
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
