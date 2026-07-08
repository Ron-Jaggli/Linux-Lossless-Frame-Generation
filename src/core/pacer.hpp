#pragma once

#include <cstdint>

namespace lsfg {

enum class PaceMode {
    Passthrough, // show the latest real frame
    Interpolate, // show the (A,B) pair blended at `phase`
};

struct PaceDecision {
    PaceMode mode = PaceMode::Passthrough;
    double phase = 0.0;  // blend position in [0,1); 0 means "exactly frame A"
    uint64_t seq_b = 0;  // sequence of the newer pair frame `phase` refers to
};

// Decides, at every display refresh, what to present: the latest real frame,
// or an in-between of the last two unique source frames. When unique frame B
// arrives, the A→B interval is presented over the *next* source period (the
// one-source-frame display delay inherent to interpolation); the phase
// advances linearly against the recovered source period and is quantized to
// multiplier steps so the output cadence is m × source fps. Falls back to
// passthrough whenever the cadence is not locked, the multiplier is 1, fewer
// than two unique frames have arrived, the pair spans a stall, or arrivals
// stop (pause → hold the latest real frame; the gap rule matches the cadence
// tracker's 0.5 s reset). Not thread-safe; the caller serializes access.
class FramePacer {
public:
    // Clamped to [1, 4]; 1 disables interpolation.
    void setMultiplier(int m);
    int multiplier() const { return multiplier_; }

    // Latest source-cadence estimate (CadenceStats::source_fps / locked).
    void setCadence(double source_fps, bool locked);

    // A unique (non-duplicate) source frame arrived. seq must increase.
    void onUniqueFrame(uint64_t seq, double t_arrival);

    PaceDecision decide(double t_now) const;

    void reset();

private:
    int multiplier_ = 2;
    double source_fps_ = 0.0;
    bool locked_ = false;
    uint64_t seq_a_ = 0, seq_b_ = 0;
    double t_a_ = 0.0, t_b_ = 0.0;
    int uniques_seen_ = 0; // saturates at 2; only "do we have a pair" matters
};

} // namespace lsfg
