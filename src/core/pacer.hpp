#pragma once

#include <cstdint>

namespace lsfg {

// What to show at one display refresh.
struct PaceDecision {
    enum class Mode {
        Passthrough, // show the latest real frame
        Interpolate, // show the (A, B) pair at `phase`
    };
    Mode mode = Mode::Passthrough;
    // Position between the last two unique frames, quantized to k/m for
    // k in [0, m). 0 means "frame A exactly" — the caller can skip the
    // blend pass and present A directly.
    double phase = 0.0;
    // The unique-frame pair the phase refers to (Interpolate only).
    uint64_t seq_a = 0, seq_b = 0;
};

// Decides, at every display refresh, what to present: the latest real frame
// or an interpolated in-between of the last two unique frames. When unique
// frame B arrives, the A→B interval is presented over the *next* source
// period (the one-source-frame display delay inherent to interpolation);
// the phase advances linearly against the recovered source period and is
// quantized to multiples of 1/multiplier, so the output rate approaches
// multiplier × source fps. Falls back to Passthrough whenever cadence is
// not locked, the multiplier is 1, frame generation is disabled, or
// arrivals stop (pause → hold the latest frame; the >0.5 s gap rule
// matches CadenceTracker's reset). Not thread-safe; the caller serializes
// access.
class FramePacer {
public:
    // Capture side: called once per *unique* source frame.
    void onUniqueFrame(uint64_t seq, double t_arrival);
    // Current cadence estimate (source unique-frame rate, and whether the
    // estimate is stable enough to act on).
    void updateCadence(double source_fps, bool locked);

    void setMultiplier(int m) { multiplier_ = m; }
    void setEnabled(bool on) { enabled_ = on; }
    int multiplier() const { return multiplier_; }
    bool enabled() const { return enabled_; }

    // Render side: what should the display show at t_now?
    PaceDecision decide(double t_now) const;

    void reset();

private:
    int multiplier_ = 2;
    bool enabled_ = true;
    double source_fps_ = 0.0;
    bool locked_ = false;
    uint64_t seq_a_ = 0, seq_b_ = 0;
    double t_b_ = 0.0;  // arrival time of the newest unique frame
    int uniques_seen_ = 0;
};

} // namespace lsfg
