#include "core/pacer.hpp"

#include <cmath>

namespace lsfg {

// Matches CadenceTracker's GAP_RESET_S: a longer gap is a pause or seek,
// and interpolating across it would animate a still image.
static constexpr double GAP_RESET_S = 0.5;

void FramePacer::onUniqueFrame(uint64_t seq, double t_arrival) {
    if (uniques_seen_ > 0 && t_arrival - t_b_ > GAP_RESET_S) {
        // Pause/seek: the previous frame is stale, restart pairing so the
        // first frame after resume is not blended against pre-pause content.
        uniques_seen_ = 0;
    }
    seq_a_ = seq_b_;
    seq_b_ = seq;
    t_b_ = t_arrival;
    if (uniques_seen_ < 2)
        uniques_seen_++;
}

void FramePacer::updateCadence(double source_fps, bool locked) {
    source_fps_ = source_fps;
    locked_ = locked;
}

PaceDecision FramePacer::decide(double t_now) const {
    PaceDecision d;
    if (!enabled_ || multiplier_ <= 1 || !locked_ || source_fps_ <= 0.0 ||
        uniques_seen_ < 2)
        return d;
    if (t_now - t_b_ > GAP_RESET_S)
        return d; // arrivals stopped: hold the latest real frame

    // B arrived at t_b_; present A→B over the following source period.
    double period = 1.0 / source_fps_;
    double progress = (t_now - t_b_) / period;
    if (progress < 0.0)
        progress = 0.0;
    if (progress >= 1.0)
        return d; // ran past the period (late frame): show latest real frame

    d.mode = PaceDecision::Mode::Interpolate;
    d.phase = std::floor(progress * multiplier_) / multiplier_;
    d.seq_a = seq_a_;
    d.seq_b = seq_b_;
    return d;
}

void FramePacer::reset() {
    seq_a_ = seq_b_ = 0;
    t_b_ = 0.0;
    uniques_seen_ = 0;
    source_fps_ = 0.0;
    locked_ = false;
}

} // namespace lsfg
