#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>

#include <vulkan/vulkan.h>

namespace lsfg::vk {

class Context;

// Pool of GPU images that decouples the capture thread (writer) from the
// render thread (reader). Published images are always in
// TRANSFER_SRC_OPTIMAL layout. Besides the latest frame, the pool tracks the
// two most recent *unique* frames (per the capture probe's duplicate
// verdict) so an interpolator can lease the (A, B) pair. Six slots keep the
// writer non-blocking in the worst case: two reader-held slots plus the
// latest frame (possibly a duplicate distinct from any unique) plus the two
// tracked uniques still leaves one free.
class FramePool {
public:
    static constexpr int SLOT_COUNT = 6;

    struct ReadLease {
        int index = -1;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;
        uint64_t seq = 0;
        double t_capture = 0.0;
    };

    // The two most recent unique frames; A is the older one.
    struct PairLease {
        int index_a = -1, index_b = -1;
        VkImage image_a = VK_NULL_HANDLE, image_b = VK_NULL_HANDLE;
        VkImageView view_a = VK_NULL_HANDLE, view_b = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;
        uint64_t seq_a = 0, seq_b = 0;
        double t_capture_a = 0.0, t_capture_b = 0.0;
    };

    bool create(Context& ctx, VkFormat format, uint32_t width, uint32_t height);
    // Recreate for a new source size/format. Blocks until the reader has
    // released its lease. Writer-thread only.
    bool recreate(VkFormat format, uint32_t width, uint32_t height);
    void destroy();

    // Writer side (capture thread). acquireWrite never blocks: there is
    // always a slot that is neither protected nor being read.
    int acquireWrite();
    VkImage image(int index) const { return slots_[index].image; }
    void publish(int index, uint64_t seq, double t_capture, bool unique);

    // Reader side (render thread): at most one lease of either kind
    // outstanding at a time. acquireRead returns the latest published frame;
    // acquirePairRead the two latest uniques. Empty lease (index == -1 /
    // index_b == -1) when not available yet.
    ReadLease acquireRead();
    void releaseRead(const ReadLease& lease);
    PairLease acquirePairRead();
    void releasePairRead(const PairLease& lease);

    VkFormat format() const { return format_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint64_t latestSeq();
    bool valid() const { return width_ > 0; }

private:
    struct Slot {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint64_t seq = 0;
        double t_capture = 0.0;
    };

    void destroySlotsLocked();
    bool createSlotsLocked(VkFormat format, uint32_t width, uint32_t height);
    bool anyReaderLocked() const {
        return reading_ != -1 || reading_a_ != -1 || reading_b_ != -1;
    }

    Context* ctx_ = nullptr;
    Slot slots_[SLOT_COUNT];
    int latest_ = -1;
    int latest_unique_ = -1;
    int prev_unique_ = -1;
    int reading_ = -1;
    int reading_a_ = -1, reading_b_ = -1;
    std::mutex mutex_;
    std::condition_variable reader_done_;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    uint32_t width_ = 0, height_ = 0;
};

} // namespace lsfg::vk
