#pragma once

#include <cstdint>

namespace lsfg {

// What the renderer should put on screen this refresh.
struct PaceDecision {
    bool interpolate = false; // false: blit the latest real frame as always
    // Quantized blend position between the pair (A, B), in [0, 1).
    // 0 means "show A exactly" — no compute pass needed.
    double phase = 0.0;
    // Sequence numbers identifying the pair the phase refers to. The
    // renderer checks these against the pool's pair lease and falls back to
    // passthrough for one refresh if they disagree (pair advanced between
    // the decision and the lease).
    uint64_t seq_a = 0, seq_b = 0;
};

// Decides, at every display refresh, what to present: the latest real frame
// (passthrough) or an in-between blend of the last two unique frames. When
// unique frame B arrives, the A→B interval is presented over the *next*
// source period — the one-source-frame display delay inherent to
// interpolation — with the phase advancing against the recovered source
// period and quantized to the multiplier's steps.
//
// Falls back to passthrough whenever the cadence is not locked, the
// multiplier is 1, fewer than two unique frames have arrived, or the current
// pair's window has been played out and no new frame arrived (pause: hold
// the latest real frame).
//
// Pure logic, std-only. Not thread-safe; the caller serializes access
// (same contract as CadenceTracker).
class FramePacer {
public:
    void setMultiplier(int m) { multiplier_ = m < 1 ? 1 : m; }
    // Source rate estimate from the cadence tracker; unlocked disables
    // interpolation until the estimate stabilizes again.
    void setCadence(double source_fps, bool locked);
    // A unique (non-duplicate) captured frame was published to the pool.
    void onUniqueFrame(uint64_t seq, double t_arrival);
    PaceDecision decide(double t_now) const;
    void reset();

private:
    int multiplier_ = 1;
    double source_fps_ = 0.0;
    bool locked_ = false;
    uint64_t seq_a_ = 0, seq_b_ = 0; // previous and latest unique frame
    double t_b_ = 0.0;               // arrival time of the latest unique
    int uniques_seen_ = 0;           // saturates at 2; enough for a pair
};

} // namespace lsfg
