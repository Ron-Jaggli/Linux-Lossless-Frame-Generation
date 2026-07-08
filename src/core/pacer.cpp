#include "core/pacer.hpp"

namespace lsfg {

// A pair older than this many source periods with no successor is a pause
// or stall, not material to interpolate over; hold the latest real frame.
// (The cadence tracker's 0.5 s gap rule unlocks shortly after anyway.)
static constexpr double STALE_PERIODS = 1.0;

void FramePacer::setCadence(double source_fps, bool locked) {
    source_fps_ = source_fps;
    locked_ = locked && source_fps > 0.0;
}

void FramePacer::onUniqueFrame(uint64_t seq, double t_arrival) {
    seq_a_ = seq_b_;
    seq_b_ = seq;
    t_b_ = t_arrival;
    if (uniques_seen_ < 2)
        uniques_seen_++;
}

PaceDecision FramePacer::decide(double t_now) const {
    PaceDecision d;
    if (multiplier_ < 2 || !locked_ || uniques_seen_ < 2)
        return d;

    // Present A→B over the source period following B's arrival.
    double period = 1.0 / source_fps_;
    double raw = (t_now - t_b_) / period;
    if (raw < 0.0)
        raw = 0.0;
    if (raw >= STALE_PERIODS)
        return d; // window played out, next frame late or stream paused

    // Quantize to the multiplier's steps; step 0 is frame A itself.
    d.interpolate = true;
    d.phase = double(int(raw * multiplier_)) / multiplier_;
    d.seq_a = seq_a_;
    d.seq_b = seq_b_;
    return d;
}

void FramePacer::reset() {
    seq_a_ = seq_b_ = 0;
    t_b_ = 0.0;
    uniques_seen_ = 0;
    locked_ = false;
    source_fps_ = 0.0;
}

} // namespace lsfg
