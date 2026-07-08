// Unit tests for FramePacer. Plain CTest executable: returns nonzero on any
// failure. Same harness style as test_cadence.cpp.

#include "core/pacer.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                  \
    do {                                                                       \
        double va = (a), vb = (b);                                             \
        if (std::abs(va - vb) > (tol)) {                                       \
            std::printf("FAIL %s:%d: %s = %f, expected %f +/- %f\n", __FILE__, \
                        __LINE__, #a, va, vb, double(tol));                    \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

// One presented output: what the renderer would show at some instant.
struct Shown {
    lsfg::PaceMode mode;
    uint64_t seq; // pair B seq (Interpolate) or latest real seq (Passthrough)
    double phase;
    bool operator==(const Shown& o) const {
        return mode == o.mode && seq == o.seq && phase == o.phase;
    }
};

// Feeds unique frames at source_fps while sampling decide() at display_hz
// for `seconds`, like a render loop against a live capture. Returns every
// decision in tick order.
static std::vector<Shown> simulate(lsfg::FramePacer& p, double source_fps,
                                   double display_hz, double seconds,
                                   double t0 = 0.0, uint64_t first_seq = 1) {
    std::vector<Shown> shown;
    long ticks = long(seconds * display_hz);
    uint64_t seq = first_seq;
    long delivered = 0;
    for (long i = 0; i < ticks; i++) {
        double t = t0 + double(i) / display_hz;
        while (t0 + double(delivered) / source_fps <= t + 1e-9) {
            p.onUniqueFrame(seq, t0 + double(delivered) / source_fps);
            seq++;
            delivered++;
        }
        auto d = p.decide(t);
        shown.push_back({d.mode, d.mode == lsfg::PaceMode::Interpolate
                                     ? d.seq_b
                                     : seq - 1,
                         d.phase});
    }
    return shown;
}

static double distinctPerSecond(const std::vector<Shown>& shown,
                                double display_hz) {
    long changes = shown.empty() ? 0 : 1;
    for (size_t i = 1; i < shown.size(); i++)
        if (!(shown[i] == shown[i - 1]))
            changes++;
    return double(changes) / (double(shown.size()) / display_hz);
}

// Warm a locked pacer: cadence set, two uniques already seen.
static void warm(lsfg::FramePacer& p, double source_fps, int m) {
    p.setMultiplier(m);
    p.setCadence(source_fps, true);
}

static void testOutputRate2xAt24() {
    lsfg::FramePacer p;
    warm(p, 24.0, 2);
    // Sample well above the output rate so no phase window is missed.
    auto shown = simulate(p, 24.0, 240.0, 4.0);
    CHECK_NEAR(distinctPerSecond(shown, 240.0), 48.0, 2.0);
}

static void testOutputRate3xAt24() {
    lsfg::FramePacer p;
    warm(p, 24.0, 3);
    auto shown = simulate(p, 24.0, 240.0, 4.0);
    CHECK_NEAR(distinctPerSecond(shown, 240.0), 72.0, 3.0);
}

static void testOutputRate4xAt30() {
    lsfg::FramePacer p;
    warm(p, 30.0, 4);
    auto shown = simulate(p, 30.0, 240.0, 4.0);
    CHECK_NEAR(distinctPerSecond(shown, 240.0), 120.0, 5.0);
}

static void testPhaseScheduleAt2x() {
    // The quantized phases must be exactly {0, 1/2} and phase 0 must map to
    // the pair whose B frame just arrived.
    lsfg::FramePacer p;
    warm(p, 24.0, 2);
    auto shown = simulate(p, 24.0, 240.0, 2.0);
    for (const auto& s : shown) {
        if (s.mode != lsfg::PaceMode::Interpolate)
            continue;
        CHECK(s.phase == 0.0 || s.phase == 0.5);
    }
}

static void testPhaseMonotonicWithinPair() {
    lsfg::FramePacer p;
    warm(p, 24.0, 4);
    auto shown = simulate(p, 24.0, 480.0, 2.0);
    for (size_t i = 1; i < shown.size(); i++) {
        if (shown[i].mode == lsfg::PaceMode::Interpolate &&
            shown[i - 1].mode == lsfg::PaceMode::Interpolate &&
            shown[i].seq == shown[i - 1].seq)
            CHECK(shown[i].phase >= shown[i - 1].phase);
    }
}

static void testPassthroughWhenUnlocked() {
    lsfg::FramePacer p;
    p.setMultiplier(2);
    p.setCadence(24.0, false); // measured but not locked
    auto shown = simulate(p, 24.0, 60.0, 1.0);
    for (const auto& s : shown)
        CHECK(s.mode == lsfg::PaceMode::Passthrough);
}

static void testPassthroughAtMultiplier1() {
    lsfg::FramePacer p;
    warm(p, 24.0, 1);
    auto shown = simulate(p, 24.0, 60.0, 1.0);
    for (const auto& s : shown)
        CHECK(s.mode == lsfg::PaceMode::Passthrough);
}

static void testPassthroughBeforePair() {
    lsfg::FramePacer p;
    warm(p, 24.0, 2);
    CHECK(p.decide(0.0).mode == lsfg::PaceMode::Passthrough);
    p.onUniqueFrame(1, 0.0);
    CHECK(p.decide(0.01).mode == lsfg::PaceMode::Passthrough);
    p.onUniqueFrame(2, 1.0 / 24.0);
    CHECK(p.decide(1.0 / 24.0 + 0.01).mode == lsfg::PaceMode::Interpolate);
}

static void testPauseFallsBackAndResumeRecovers() {
    lsfg::FramePacer p;
    warm(p, 24.0, 2);
    simulate(p, 24.0, 60.0, 2.0);
    // Playback pauses: 1 s with no arrivals.
    CHECK(p.decide(3.0).mode == lsfg::PaceMode::Passthrough);
    // Resume: the first pair after the pause spans the gap and must not be
    // blended; the second pair may.
    p.onUniqueFrame(100, 3.0);
    CHECK(p.decide(3.01).mode == lsfg::PaceMode::Passthrough);
    p.onUniqueFrame(101, 3.0 + 1.0 / 24.0);
    CHECK(p.decide(3.0 + 1.0 / 24.0 + 0.01).mode ==
          lsfg::PaceMode::Interpolate);
}

static void testLateFrameClampsPhase() {
    lsfg::FramePacer p;
    warm(p, 24.0, 2);
    p.onUniqueFrame(1, 0.0);
    p.onUniqueFrame(2, 1.0 / 24.0);
    // 1.8 source periods after B: still under the stall threshold, so hold
    // the last generated phase instead of wrapping past 1.
    auto d = p.decide(1.0 / 24.0 + 1.8 / 24.0);
    CHECK(d.mode == lsfg::PaceMode::Interpolate);
    CHECK_NEAR(d.phase, 0.5, 1e-9);
}

static void testSourceDriftFollows() {
    // Cadence estimate drifts 24 → 25 fps; the pacer must keep producing a
    // sane schedule from the updated period without a reset.
    lsfg::FramePacer p;
    warm(p, 24.0, 2);
    simulate(p, 24.0, 240.0, 2.0);
    p.setCadence(25.0, true);
    auto shown = simulate(p, 25.0, 240.0, 2.0, 2.0, 1000);
    CHECK_NEAR(distinctPerSecond(shown, 240.0), 50.0, 3.0);
}

static void testMultiplierClamp() {
    lsfg::FramePacer p;
    p.setMultiplier(9);
    CHECK(p.multiplier() == 4);
    p.setMultiplier(0);
    CHECK(p.multiplier() == 1);
}

static void testReset() {
    lsfg::FramePacer p;
    warm(p, 24.0, 2);
    p.onUniqueFrame(1, 0.0);
    p.onUniqueFrame(2, 1.0 / 24.0);
    p.reset();
    CHECK(p.decide(1.0 / 24.0 + 0.01).mode == lsfg::PaceMode::Passthrough);
}

int main() {
    testOutputRate2xAt24();
    testOutputRate3xAt24();
    testOutputRate4xAt30();
    testPhaseScheduleAt2x();
    testPhaseMonotonicWithinPair();
    testPassthroughWhenUnlocked();
    testPassthroughAtMultiplier1();
    testPassthroughBeforePair();
    testPauseFallsBackAndResumeRecovers();
    testLateFrameClampsPhase();
    testSourceDriftFollows();
    testMultiplierClamp();
    testReset();
    if (g_failures) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
