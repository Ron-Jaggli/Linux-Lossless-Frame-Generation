#include "core/pacer.hpp"

namespace lsfg {

// Arrivals older than this mean playback paused or stopped; hold the latest
// real frame instead of blending into stale history. Matches the cadence
// tracker's gap-reset rule.
static constexpr double GAP_HOLD_S = 0.5;

void FramePacer::setMultiplier(int m) {
    multiplier_ = m < 1 ? 1 : (m > 4 ? 4 : m);
}

void FramePacer::setCadence(double source_fps, bool locked) {
    source_fps_ = source_fps;
    locked_ = locked;
}

void FramePacer::onUniqueFrame(uint64_t seq, double t_arrival) {
    // A pause invalidates the pair: blending across the gap would show a
    // transition that never happened. Restart pairing from this frame.
    if (uniques_seen_ > 0 && t_arrival - t_b_ > GAP_HOLD_S)
        uniques_seen_ = 0;
    seq_a_ = seq_b_;
    seq_b_ = seq;
    t_b_ = t_arrival;
    if (uniques_seen_ < 2)
        uniques_seen_++;
}

PaceDecision FramePacer::decide(double t_now) const {
    PaceDecision d;
    if (uniques_seen_ == 0)
        return d; // Hold
    d.mode = PaceDecision::Mode::Passthrough;
    if (multiplier_ < 2 || !locked_ || source_fps_ <= 0.0 ||
        uniques_seen_ < 2)
        return d;

    double period = 1.0 / source_fps_;
    double dt = t_now - t_b_;
    // Past the pair's window (period elapsed, or arrivals stopped): the
    // latest real frame is the right thing to show.
    if (dt < 0.0 || dt >= period || dt > GAP_HOLD_S)
        return d;

    d.mode = PaceDecision::Mode::Interpolate;
    d.step = int(double(multiplier_) * dt / period); // 0..m-1 by the dt check
    d.phase = double(d.step) / double(multiplier_);
    d.seq_a = seq_a_;
    d.seq_b = seq_b_;
    return d;
}

void FramePacer::reset() {
    source_fps_ = 0.0;
    locked_ = false;
    seq_a_ = seq_b_ = 0;
    t_b_ = 0.0;
    uniques_seen_ = 0;
}

} // namespace lsfg
