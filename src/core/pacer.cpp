#include "core/pacer.hpp"

#include <algorithm>
#include <cmath>

namespace lsfg {

// A gap longer than this (or 2.5 source periods, whichever is larger) means
// the stream stalled: stop interpolating and hold the latest real frame.
// Matches the cadence tracker's pause threshold.
static constexpr double GAP_RESET_S = 0.5;

void FramePacer::setMultiplier(int m) {
    multiplier_ = std::clamp(m, 1, 4);
}

void FramePacer::setCadence(double source_fps, bool locked) {
    source_fps_ = source_fps;
    locked_ = locked && source_fps > 0.0;
}

void FramePacer::onUniqueFrame(uint64_t seq, double t_arrival) {
    seq_a_ = seq_b_;
    t_a_ = t_b_;
    seq_b_ = seq;
    t_b_ = t_arrival;
    if (uniques_seen_ < 2)
        uniques_seen_++;
}

PaceDecision FramePacer::decide(double t_now) const {
    PaceDecision d;
    if (multiplier_ <= 1 || !locked_ || uniques_seen_ < 2)
        return d;

    double period = 1.0 / source_fps_;
    double stall = std::max(GAP_RESET_S, 2.5 * period);
    // Arrivals stopped (pause) or the pair itself spans a stall (seek):
    // blending across either would morph unrelated content.
    if (t_now - t_b_ > stall || t_b_ - t_a_ > stall)
        return d;

    // B arrived at t_b_; present A→B over the following source period.
    // Clamp instead of wrapping when the next frame is late: hold the last
    // generated phase rather than jumping backwards.
    double raw = (t_now - t_b_) / period;
    int k = std::clamp(int(std::floor(raw * multiplier_)), 0, multiplier_ - 1);

    d.mode = PaceMode::Interpolate;
    d.phase = double(k) / multiplier_;
    d.seq_b = seq_b_;
    return d;
}

void FramePacer::reset() {
    source_fps_ = 0.0;
    locked_ = false;
    seq_a_ = seq_b_ = 0;
    t_a_ = t_b_ = 0.0;
    uniques_seen_ = 0;
}

} // namespace lsfg
