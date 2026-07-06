#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>

#include <vulkan/vulkan.h>

namespace lsfg::vk {

class Context;

// Pool of GPU images that decouples the capture thread (writer) from the
// render thread (reader). Published images are always in
// TRANSFER_SRC_OPTIMAL layout. Besides the latest frame, the pool tracks
// the two most recent *unique* frames (duplicates flagged by the capture
// probe don't advance the pair), which the interpolator consumes.
//
// Six slots keep acquireWrite non-blocking in the worst case: a pair lease
// holds 2 slots, the current unique pair (advanced since the lease was
// taken) pins 2 more, and the latest frame can be a duplicate in its own
// slot — 5 excluded, so a 6th is always free for the writer.
class FramePool {
public:
    static constexpr int SLOT_COUNT = 6;

    struct ReadLease {
        int index = -1;
        VkImage image = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;
        uint64_t seq = 0;
        double t_capture = 0.0;
    };

    // The two most recent unique frames, A older than B.
    struct PairLease {
        int index_a = -1, index_b = -1;
        VkImage image_a = VK_NULL_HANDLE, image_b = VK_NULL_HANDLE;
        uint64_t seq_a = 0, seq_b = 0;
        double t_capture_a = 0.0, t_capture_b = 0.0;
        uint32_t width = 0, height = 0;
        bool valid() const { return index_a >= 0; }
    };

    bool create(Context& ctx, VkFormat format, uint32_t width, uint32_t height);
    // Recreate for a new source size/format. Blocks until the reader has
    // released its leases. Writer-thread only.
    bool recreate(VkFormat format, uint32_t width, uint32_t height);
    void destroy();

    // Writer side (capture thread). acquireWrite never blocks: see the
    // slot-count note above.
    int acquireWrite();
    VkImage image(int index) const { return slots_[index].image; }
    // `unique` = false marks a duplicate repaint: it becomes the latest
    // frame but does not advance the unique pair.
    void publish(int index, uint64_t seq, double t_capture, bool unique = true);

    // Reader side (render thread): at most one lease held at a time.
    // Returns the latest published frame, or an empty lease (index == -1)
    // when nothing has been published yet.
    ReadLease acquireRead();
    void releaseRead(const ReadLease& lease);
    // The two most recent unique frames, or an invalid lease until two
    // unique frames have been published.
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

    Context* ctx_ = nullptr;
    Slot slots_[SLOT_COUNT];
    int latest_ = -1;
    int reading_ = -1;
    int unique_latest_ = -1, unique_prev_ = -1;
    int pair_a_ = -1, pair_b_ = -1; // held by the reader's pair lease
    std::mutex mutex_;
    std::condition_variable reader_done_;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    uint32_t width_ = 0, height_ = 0;
};

} // namespace lsfg::vk
