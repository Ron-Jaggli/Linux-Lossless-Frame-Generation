#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>

#include <vulkan/vulkan.h>

namespace lsfg::vk {

class Context;

// Multi-buffered pool of GPU images that decouples the capture thread
// (writer) from the render thread (reader). Published images are always in
// TRANSFER_SRC_OPTIMAL layout. The pool also tracks the two most recent
// *unique* (non-duplicate) frames so the interpolator can lease them as a
// pair. The reader holds at most one lease — single or pair — at a time.
class FramePool {
public:
    struct ReadLease {
        int index = -1;
        VkImage image = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;
        uint64_t seq = 0;
        double t_capture = 0.0;
    };

    // Two most recent unique frames; a is the older, b the latest.
    struct PairLease {
        ReadLease a, b;
        bool valid() const { return a.index >= 0 && b.index >= 0; }
    };

    bool create(Context& ctx, VkFormat format, uint32_t width, uint32_t height);
    // Recreate for a new source size/format. Blocks until the reader has
    // released its lease. Writer-thread only.
    bool recreate(VkFormat format, uint32_t width, uint32_t height);
    void destroy();

    // Writer side (capture thread). acquireWrite never blocks: the slot
    // count is one more than the worst-case protected set (latest + two
    // uniques + a held pair), so a free slot always exists.
    int acquireWrite();
    VkImage image(int index) const { return slots_[index].image; }
    void publish(int index, uint64_t seq, double t_capture, bool unique);

    // Reader side (render thread). Returns the latest published frame, or
    // an empty lease (index == -1) when nothing has been published yet.
    ReadLease acquireRead();
    void releaseRead(const ReadLease& lease);

    // Reader side. The last two unique frames, or an invalid pair before
    // two uniques have been published. Must not be held together with a
    // single-frame lease.
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
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint64_t seq = 0;
        double t_capture = 0.0;
    };

    void destroySlotsLocked();
    bool createSlotsLocked(VkFormat format, uint32_t width, uint32_t height);

    static constexpr int kSlots = 6;

    Context* ctx_ = nullptr;
    Slot slots_[kSlots];
    int latest_ = -1;
    int reading_ = -1;
    int unique0_ = -1, unique1_ = -1; // latest and previous unique frame
    int pair_a_ = -1, pair_b_ = -1;   // slots held by a pair lease
    std::mutex mutex_;
    std::condition_variable reader_done_;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    uint32_t width_ = 0, height_ = 0;
};

} // namespace lsfg::vk
