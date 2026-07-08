#pragma once

#include <cstdint>

namespace lsfg {

// What to show at one display refresh.
struct PaceDecision {
    enum class Mode {
        Hold,        // nothing captured yet: keep whatever is on screen
        Passthrough, // show the latest real frame
        Interpolate, // show blend(A, B, phase) of the two latest unique frames
    };
    Mode mode = Mode::Hold;
    double phase = 0.0;   // Interpolate only: 0 => exactly A, ->1 => B
    uint64_t seq_a = 0;   // Interpolate only: the pair, by publish sequence
    uint64_t seq_b = 0;
    int step = 0;         // Interpolate only: quantized index, 0..multiplier-1
};

// Decides, at every display refresh, what belongs on screen: the latest real
// frame, or which (A, B, phase) in-between of the two most recent unique
// source frames. When unique frame B arrives, the A->B interval is presented
// over the *next* source period — the one-source-frame display delay
// inherent to interpolation — with phase advancing linearly against the
// recovered source period and quantized to the multiplier's steps (step 0 is
// exactly A, so output rate = multiplier x source fps). Falls back to
// Passthrough whenever interpolation cannot be trusted: multiplier 1, no
// cadence lock, fewer than two uniques, or arrivals stopped (pause/seek;
// same 0.5 s gap rule as the cadence tracker). Not thread-safe; the caller
// serializes access.
class FramePacer {
public:
    void setMultiplier(int m);
    int multiplier() const { return multiplier_; }

    // Cadence snapshot (source estimate + lock), typically refreshed from
    // CadenceTracker::stats() by the polling side.
    void setCadence(double source_fps, bool locked);

    // Called when a unique (non-duplicate) frame is published.
    void onUniqueFrame(uint64_t seq, double t_arrival);

    PaceDecision decide(double t_now) const;

    void reset();

private:
    int multiplier_ = 1;
    double source_fps_ = 0.0;
    bool locked_ = false;
    uint64_t seq_a_ = 0, seq_b_ = 0;
    double t_b_ = 0.0;
    int uniques_seen_ = 0;
};

} // namespace lsfg
