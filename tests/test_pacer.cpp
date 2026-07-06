// Unit tests for FramePacer. Plain CTest executable: returns nonzero on
// any failure. No framework — see PLAN.md.

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

using lsfg::FramePacer;
using lsfg::PaceDecision;
using Mode = lsfg::PaceDecision::Mode;

// One displayed image: a real frame (identified by the latest seq at that
// moment) or an interpolated (pair, phase). Two decisions show the same
// pixels iff these compare equal.
struct Shown {
    Mode mode;
    uint64_t seq_a, seq_b;
    double phase;
    uint64_t latest_seq;
    bool operator==(const Shown& o) const {
        if (mode != o.mode)
            return false;
        if (mode == Mode::Passthrough)
            return latest_seq == o.latest_seq;
        return seq_a == o.seq_a && seq_b == o.seq_b && phase == o.phase;
    }
};

// Feed uniform unique arrivals at source_fps and query decide() at
// display_hz; collect the sequence of distinct displayed images.
static std::vector<Shown> runSchedule(FramePacer& p, double source_fps,
                                      double display_hz, double seconds,
                                      double t0 = 0.0) {
    std::vector<Shown> shown;
    uint64_t seq = 0;
    long n_disp = long(seconds * display_hz);
    long next_src = 0;
    for (long i = 0; i < n_disp; i++) {
        double t = t0 + double(i) / display_hz;
        // deliver any source frames due by now (arrival just before refresh)
        while (t0 + double(next_src) / source_fps <= t) {
            p.onUniqueFrame(++seq, t0 + double(next_src) / source_fps);
            next_src++;
        }
        PaceDecision d = p.decide(t + 1e-6);
        Shown s{d.mode, d.seq_a, d.seq_b, d.phase, seq};
        if (shown.empty() || !(shown.back() == s))
            shown.push_back(s);
    }
    return shown;
}

static void testPassthroughFallbacks() {
    FramePacer p;
    p.setMultiplier(2);
    p.updateCadence(24.0, true);
    // cold start: no pair yet
    CHECK(p.decide(0.0).mode == Mode::Passthrough);
    p.onUniqueFrame(1, 0.0);
    CHECK(p.decide(0.01).mode == Mode::Passthrough); // one unique isn't a pair
    p.onUniqueFrame(2, 1.0 / 24.0);
    CHECK(p.decide(0.05).mode == Mode::Interpolate);
    // not locked
    p.updateCadence(24.0, false);
    CHECK(p.decide(0.05).mode == Mode::Passthrough);
    p.updateCadence(24.0, true);
    // multiplier 1
    p.setMultiplier(1);
    CHECK(p.decide(0.05).mode == Mode::Passthrough);
    p.setMultiplier(2);
    // disabled at runtime
    p.setEnabled(false);
    CHECK(p.decide(0.05).mode == Mode::Passthrough);
    p.setEnabled(true);
    CHECK(p.decide(0.05).mode == Mode::Interpolate);
    // reset drops the pair and the cadence
    p.reset();
    CHECK(p.decide(0.05).mode == Mode::Passthrough);
}

// Uniform arrivals sampled well above m x source: every phase slot is hit,
// so each source period yields exactly m distinct images with phases k/m.
static void testSchedule(int m, double source_fps) {
    FramePacer p;
    p.setMultiplier(m);
    p.updateCadence(source_fps, true);
    auto shown = runSchedule(p, source_fps, 240.0, 2.0);

    // skip the passthrough warmup and any trailing partial period
    size_t begin = 0;
    while (begin < shown.size() && shown[begin].mode == Mode::Passthrough)
        begin++;
    CHECK(begin <= 2); // locks on quickly: at most the initial real frames

    int interp = 0;
    double prev_phase = -1.0;
    uint64_t prev_b = 0;
    for (size_t i = begin; i < shown.size(); i++) {
        const Shown& s = shown[i];
        if (s.mode != Mode::Interpolate)
            continue;
        interp++;
        // phase is a multiple of 1/m in [0,1)
        double k = s.phase * m;
        CHECK_NEAR(k, std::round(k), 1e-9);
        CHECK(s.phase >= 0.0 && s.phase < 1.0);
        CHECK(s.seq_b == s.seq_a + 1);
        if (s.seq_b == prev_b)
            CHECK(s.phase > prev_phase); // monotonic within a pair
        else if (prev_b != 0)
            CHECK_NEAR(s.phase, 0.0, 1e-9); // each new pair starts at A
        prev_phase = s.phase;
        prev_b = s.seq_b;
    }
    // output rate ~= m x source fps over the ~2 s run (tolerate the ragged
    // first and last period)
    CHECK_NEAR(double(interp), 2.0 * m * source_fps, double(m + 2));
}

static void testSchedules() {
    testSchedule(2, 24.0);
    testSchedule(3, 24.0);
    testSchedule(4, 24.0);
    testSchedule(2, 30.0);
    testSchedule(3, 30.0);
}

// Real 24-in-60 delivery: uniques arrive on the 60 Hz vsync grid in a 3:2
// pattern (alternating ~50/~33 ms), the display also refreshes at 60 Hz.
// The pacer must stay sane: valid quantized phases, monotonic within each
// pair, and never fewer distinct images than passthrough would give.
static void testPulldownArrivals() {
    FramePacer p;
    p.setMultiplier(2);
    p.updateCadence(24.0, true);

    std::vector<Shown> shown;
    uint64_t seq = 0;
    double t_arrival = 0.0;
    long vsync_of_next = 0;
    long src = 0;
    double prev_phase = -1.0;
    uint64_t prev_b = 0;
    for (long i = 0; i < 240; i++) { // 4 s at 60 Hz
        double t = double(i) / 60.0;
        if (i == vsync_of_next) {
            t_arrival = t;
            p.onUniqueFrame(++seq, t_arrival);
            src++;
            vsync_of_next += (src % 2) ? 3 : 2; // 3:2 pulldown
        }
        PaceDecision d = p.decide(t + 1e-4);
        if (d.mode == Mode::Interpolate) {
            double k = d.phase * 2.0;
            CHECK_NEAR(k, std::round(k), 1e-9);
            if (d.seq_b == prev_b && prev_phase >= 0.0)
                CHECK(d.phase >= prev_phase);
            prev_phase = d.phase;
            prev_b = d.seq_b;
        }
        Shown s{d.mode, d.seq_a, d.seq_b, d.phase, seq};
        if (shown.empty() || !(shown.back() == s))
            shown.push_back(s);
    }
    // 4 s of 24 fps source: passthrough would show ~96 distinct images; 2x
    // must add in-betweens (up to the 60 Hz display ceiling of 240).
    CHECK(shown.size() > 100);
    CHECK(shown.size() <= 240);
}

static void testPauseHoldsLastFrame() {
    FramePacer p;
    p.setMultiplier(2);
    p.updateCadence(24.0, true);
    double t = 0.0;
    for (int i = 0; i < 24; i++, t += 1.0 / 24.0)
        p.onUniqueFrame(i + 1, t);
    double t_last = t - 1.0 / 24.0;
    CHECK(p.decide(t_last + 0.01).mode == Mode::Interpolate);
    // arrivals stop: past the gap threshold the pacer holds the real frame
    CHECK(p.decide(t_last + 0.6).mode == Mode::Passthrough);
    // resume: the first unique after the gap must not blend across it
    p.onUniqueFrame(25, t_last + 2.0);
    CHECK(p.decide(t_last + 2.0 + 0.01).mode == Mode::Passthrough);
    p.onUniqueFrame(26, t_last + 2.0 + 1.0 / 24.0);
    auto d = p.decide(t_last + 2.0 + 1.0 / 24.0 + 0.001);
    CHECK(d.mode == Mode::Interpolate);
    CHECK(d.seq_a == 25 && d.seq_b == 26);
}

static void testLateFrameShowsLatest() {
    FramePacer p;
    p.setMultiplier(2);
    p.updateCadence(24.0, true);
    p.onUniqueFrame(1, 0.0);
    p.onUniqueFrame(2, 1.0 / 24.0);
    // Past one source period with no new unique (delivery hiccup shorter
    // than the pause gap): show the latest real frame, don't freeze on the
    // last in-between.
    CHECK(p.decide(1.0 / 24.0 + 1.5 / 24.0).mode == Mode::Passthrough);
}

static void testRateDrift() {
    FramePacer p;
    p.setMultiplier(2);
    p.updateCadence(24.0, true);
    auto a = runSchedule(p, 24.0, 240.0, 1.0);
    CHECK(a.size() > 40);
    // The stream drifts to 25 fps and the cadence estimate follows; the
    // pacer keeps producing valid phases at the new rate.
    p.updateCadence(25.0, true);
    auto b = runSchedule(p, 25.0, 240.0, 1.0, 2.0);
    int interp = 0;
    for (const Shown& s : b)
        if (s.mode == Mode::Interpolate)
            interp++;
    CHECK_NEAR(double(interp), 50.0, 4.0);
}

int main() {
    testPassthroughFallbacks();
    testSchedules();
    testPulldownArrivals();
    testPauseHoldsLastFrame();
    testLateFrameShowsLatest();
    testRateDrift();
    if (g_failures) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
