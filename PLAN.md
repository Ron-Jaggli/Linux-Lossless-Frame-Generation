# PLAN — Linux-Lossless-Frame-Generation (lsfg-cap)

## History

- **2026-07-05 (morning):** Phase 1 recon of the then-empty repository; the
  bootstrap plan was reviewed and merged as PR #1.
- **2026-07-05 (midday):** the project pivoted to the concrete tool described
  in the README. Milestones 0/1 (portal/PipeWire capture → Vulkan passthrough
  window) were implemented on `claude/wizardly-clarke-4fk1ip`.
- **2026-07-05 (evening):** Milestone 2 (duplicate detection + cadence
  recovery, unit tests, CI) implemented and merged as PR #2.
- **2026-07-06:** recon of the milestone-2 codebase plus the Milestone 3a
  scaffolding plan, merged as PR #3. **The 3a code itself was not written**
  — that merge delivered the plan only.
- **2026-07-08 (this pass):** fresh recon on `claude/wizardly-clarke-ar87jv`,
  baseline re-verified, and the 3a plan revalidated against today's tree and
  this container's tighter network policy. Phase 2 will execute it.

## Phase 1 recon — current state (2026-07-08)

### Module map (unchanged since the 2026-07-06 recon)

```
CMakeLists.txt           C++20; lsfg_core static lib (pure logic, no system
                         deps) + lsfg-cap app (gated by LSFG_BUILD_APP) +
                         CTest unit tests (BUILD_TESTING)
.github/workflows/ci.yml ubuntu-latest, LSFG_BUILD_APP=OFF, builds core and
                         runs ctest on every push/PR
src/
  main.cpp          272L CLI parsing, portal pump, main loop, per-second
                         stats line, DRM black-frame verdict   [uses SDL3]
  options.hpp        20L Options struct; multiplier parsed but inert so far
  log.hpp            48L leveled logging + monotonic nowSeconds()
  portal.{hpp,cpp}  183L xdg-desktop-portal ScreenCast handshake (libportal)
  capture.{hpp,cpp} 947L PipeWire consumer: DMA-BUF w/ modifier fixation,
                         SHM fallback, copy into FramePool, 64×64 probe →
                         duplicate compare → CadenceTracker (mutex-guarded)
  renderer.{hpp,cpp}225L blits latest pool frame to swapchain, letterboxed;
                         per-frame fence wait; latency EMA
  core/cadence.*    203L CadenceTracker: recovers source fps + repeat
                         pattern from (timestamp, dup) events; unit-tested
  vk/context.*      402L instance/device/queue/swapchain     [uses SDL3]
  vk/frame_pool.*   202L triple-buffered VkImage pool, writer=capture
                         thread, reader=render thread, publish/acquireRead
  vk/dmabuf_import.*150L VkImage import of PipeWire DMA-BUF planes
tests/test_cadence.cpp   7 scenario tests via a tiny assert harness
```

### Build & test baseline (this container, 2026-07-08)

- Ubuntu 24.04, cmake 3.28.3, ninja 1.11.1, g++ 13.3.
- **Core: green.** `cmake -B build-core -G Ninja -DLSFG_BUILD_APP=OFF` builds
  with zero warnings; `ctest` **1/1 passed** (cadence suite). CI is green on
  `main`.
- **App: partially compile-checkable here.** `libpipewire-0.3-dev
  libportal-dev libvulkan-dev` installed from apt. SDL3 is still not packaged
  on 24.04, and unlike the 2026-07-06 pass this session's network policy
  blocks every SDL3 source (GitHub releases, libsdl.org, crates.io) — so no
  scratch SDL3 build this time. What was verified instead: `-fsyntax-only`
  with full warnings on every translation unit that doesn't include SDL
  headers — `capture.cpp`, `portal.cpp`, `renderer.cpp`, `vk/frame_pool.cpp`,
  `vk/dmabuf_import.cpp` — **all clean**. Only `main.cpp` and
  `vk/context.cpp` include `SDL3/SDL.h` and cannot be compiled here.
- `glslangValidator` 15.1.0 installed (needed by increment 3's shader).
- Nothing app-side is runtime-testable here (no display/portal/GPU); runtime
  validation stays on the owner's desktop, as before.

### Where the project stands

Milestones 0–2 are code-complete; milestone 3 has an approved plan (PR #3)
but no code. The open items are unchanged:

1. **Milestone 0 hardware question** — one manual `--drm-test` run against
   Crunchyroll on the owner's desktop is still outstanding.
2. **Milestone 3** — actual frame interpolation. This pass implements its
   scaffolding half (3a).

## Phase 2 — implement Milestone 3a (per the PR #3 plan, revalidated)

The scope, interfaces, and increment order from the merged 2026-07-06 plan
still fit today's tree exactly; they are restated below with two adjustments
for this container's constraints (marked ✱).

- **Frame pacing** (pure logic, unit-tested, CI-covered): decide, at every
  display refresh, what to show — which real frame, or which (A,B,phase)
  in-between — from the cadence tracker's output and the multiplier.
- **Unique-frame pairing** in the frame pool: an interpolator consumes the
  *last two unique* source frames; today the pool only exposes "latest".
- **A pluggable `Interpolator` interface with a linear-blend baseline**:
  `mix(A, B, phase)` in a small compute shader. Blend is visually mediocre
  but exercises the entire pipeline and gives a working `-m 2/3/4`
  end-to-end; the LSFG chain later replaces it behind the same interface.

### Increments (each: build clean, tests green, commit)

1. **Add frame pacer with unit tests**
   - `src/core/pacer.{hpp,cpp}`: `FramePacer`, pure std-only, single-threaded
     by contract (like `CadenceTracker`); added to `lsfg_core`.
   - Input: `onUniqueFrame(seq, t_arrival)` plus the current source-fps
     estimate and multiplier m. Query: `decide(t_now) -> {mode, phase}` where
     mode is `Passthrough` (show latest real frame) or `Interpolate` (show
     pair blend at `phase` ∈ [0,1)).
   - Model: when unique frame B arrives, the A→B interval is presented over
     the *next* source period (one-source-period display delay, inherent to
     interpolation); phase advances linearly against the recovered source
     period. Falls back to `Passthrough` whenever cadence is not locked,
     m == 1, or arrivals stop (>0.5 s gap → hold last frame, matching the
     cadence tracker's reset rule).
   - `tests/test_pacer.cpp`, same harness style as the cadence tests:
     2x/3x/4x at 24-in-60 and 30-in-60, passthrough fallback when unlocked,
     pause/resume, source-rate drift, phase monotonicity, output-rate ≈
     m × source fps. Registered in CMake next to the cadence test; runs in
     CI automatically.
2. **Frame pool: unique-frame pair leases**
   - `publish()` gains a `unique` flag (capture already knows duplicate
     status at publish time from the probe compare).
   - New `acquirePairRead()` → the two most recent unique frames (A, B) with
     capture timestamps; slot count grows 3 → 5 so the writer still never
     blocks while a reader holds a pair (5 ≥ 2 held + 2 latest uniques + 1
     write; asserted in debug builds). Existing `acquireRead()` unchanged —
     passthrough behavior identical after this commit.
3. **Interpolator interface + blend baseline**
   - `src/vk/interpolate.{hpp,cpp}`: `Interpolator` (record commands
     producing `dst` from `(A, B, phase)`) and `BlendInterpolator` — one
     compute pipeline sampling A and B into an owned intermediate image.
   - Shader `src/shaders/blend.comp` (GLSL); compiled SPIR-V committed as a
     generated header via `tools/gen_shaders.sh` — no new hard build
     dependency (`glslangValidator` needed only when the GLSL changes; it is
     available in this container and the header will be regenerated and
     committed here).
   - Deliverable of this commit: app still builds; interpolator constructed
     but not yet wired.
4. **Wire it up: pacer + interpolator in the render path**
   - Capture feeds `onUniqueFrame` (same lock as the cadence tracker).
   - `Renderer::drawFrame()` consults the pacer: `Interpolate` → run the
     blend into the intermediate and blit that to the swapchain; else blit
     the latest real frame exactly as today.
   - `-m N` takes effect (m=1 forces passthrough); `G` toggles frame
     generation at runtime; stats line grows `output 120.0 fps (2x gen)`;
     the video-delay figure includes the inherent one-source-period hold.
   - README: milestone 3a status, usage, and an honest note on blend quality
     and added latency (~42 ms at 24 fps source vs the 50 ms lipsync
     target — measured and displayed, tunable later).
5. **CI unchanged** — pacer tests run under the existing
   `LSFG_BUILD_APP=OFF` job automatically.

### ✱ Adjustments vs the PR #3 plan (container constraints)

- **No SDL3 obtainable here**, so `main.cpp` and `vk/context.cpp` cannot be
  compiled in this environment. Mitigations: keep increment-4 diffs to those
  two files minimal (option plumbing, one key case, stats-line string); and
  before committing, syntax-check them against a scratchpad-only stub of the
  few `SDL3/SDL.h` declarations they use (stub lives in the scratchpad, is
  never committed, and proves nothing beyond "the diff parses and
  type-checks against declared signatures"). Every other touched file —
  pacer, pool, interpolator, renderer, capture — compiles for real here
  with full warnings.
- **Verification asymmetry is called out per commit**: commit messages for
  increments 3–4 will state what was compile-verified vs what needs the
  owner's desktop, so nothing reads as runtime-validated when it isn't.

### Explicitly out of scope for this pass

- The LSFG shader lift itself (`Lossless.dll` extraction / DXBC→SPIR-V, per
  lsfg-vk). Next milestone, on hardware that has the DLL and a GPU; it slots
  in as a second `Interpolator` implementation.
- Motion-compensated anything — blend is knowingly naive.
- Config file, GUI, latency tuning beyond honest measurement.

### Risks

- **No GPU here**: increments 2–4 are compile-tested only; runtime
  validation happens on the owner's desktop. Mitigated by keeping every
  commit's default path (passthrough) byte-identical to today's behavior
  and gating FG behind the pacer's lock + multiplier.
- **Untestable main.cpp/context.cpp edits** (new this pass, see ✱ above):
  kept minimal and stub-syntax-checked; worst case is a compile error the
  owner hits on first desktop build of increment 4, isolated to one commit.
- **Fades/overlays produce "unique" frames at pulldown positions**: the
  pacer sees more uniques and shorter periods; worst case it presents real
  frames — degrades to passthrough, never worse than today.
