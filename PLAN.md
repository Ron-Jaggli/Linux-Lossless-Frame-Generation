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
  scaffolding plan, merged as PR #3. The plan was approved but the code was
  **not** implemented in that pass.
- **2026-07-08 (this pass):** fresh recon in a new container, recorded below.
  The goal of this pass is to *implement* Milestone 3a as planned in PR #3,
  with two small corrections discovered during recon (see "Plan corrections").

## Phase 1 recon — current state (2026-07-08)

### Module map (verified against source; unchanged since PR #3)

```
CMakeLists.txt           C++20; lsfg_core static lib (pure logic, no system
                         deps) + lsfg-cap app (gated by LSFG_BUILD_APP) +
                         CTest unit tests (BUILD_TESTING)
.github/workflows/ci.yml ubuntu-latest, LSFG_BUILD_APP=OFF, builds core and
                         runs ctest on every push/PR
src/
  main.cpp         273L  CLI parsing, portal pump, main loop, per-second
                         stats line (capture/source cadence/present/latency),
                         DRM black-frame verdict
  options.hpp       21L  Options struct; multiplier parsed but inert so far
  log.hpp           48L  leveled logging + monotonic nowSeconds()
  portal.{hpp,cpp} 183L  xdg-desktop-portal ScreenCast handshake (libportal),
                         picker, restore token, close signal
  capture.{hpp,cpp}~950L PipeWire consumer: DMA-BUF negotiation w/ modifier
                         fixation, SHM fallback + mid-stream renegotiation,
                         copy into FramePool, 64×64 probe every frame →
                         duplicate compare → CadenceTracker (mutex-guarded),
                         luminance for the DRM test
  renderer.{hpp,cpp}225L blits latest pool frame to SDL3/Vulkan swapchain,
                         letterboxed; per-frame fence wait; latency EMA
  core/cadence.*   ~200L CadenceTracker: recovers source fps + repeat pattern
                         ("3:2", "2:2", …) from (timestamp, dup) events; pure,
                         unit-tested
  vk/context.*      402L instance/device/queue/swapchain, DRM modifier query
  vk/frame_pool.*   202L triple-buffered VkImage pool, writer=capture thread,
                         reader=render thread, publish/acquireRead leases
  vk/dmabuf_import.*150L VkImage import of PipeWire DMA-BUF planes
tests/test_cadence.cpp   7 scenario tests (3:2, 2:2, passthrough, damage-
                         driven, jitter, cadence change, pause) via a tiny
                         assert harness, no external framework
```

Since PR #3 only README/PLAN/.gitignore changed (three README wording commits
by the owner sit on this branch, `claude/wizardly-clarke-0c6fc2`, ahead of
main). None of the Milestone 3a code (`core/pacer.*`, pair leases,
`vk/interpolate.*`) exists yet.

### Build & test baseline (this environment, fresh container)

- Ubuntu 24.04 container, cmake 3.28 / ninja / g++ 13. Installed
  `libpipewire-0.3-dev libportal-dev libvulkan-dev glslang-tools` from apt.
  SDL3 is still not packaged on 24.04; built a minimal SDL 3.2.20 from the
  `sdl3-src` crate tarball (static.crates.io is reachable; GitHub is not)
  into a scratch prefix for compile/link validation.
- Core: `cmake -B build-core -G Ninja -DLSFG_BUILD_APP=OFF` → builds clean,
  `ctest` **1/1 passed** (the cadence suite).
- App: full `lsfg-cap` compiles and links clean against the scratch SDL3,
  zero warnings. Nothing app-side is runtime-testable here: no display, no
  portal, no GPU. Runtime behavior is validated on the owner's desktop.
- `glslangValidator` **is** available in this container (unlike the 07-06
  pass), so the blend compute shader's SPIR-V can be generated and committed
  from here, not just hand-checked.
- CI (core tests only) is green on `main`.

### Plan corrections found during recon

1. **`publish()` does not yet know the duplicate verdict.** In
   `Capture::handleProcess()` the pool publish happens *before*
   `readProbe()` computes the duplicate flag (capture.cpp:407–409). The
   probe readback is already fence-waited by that point (`submitAndWait`
   precedes the whole block), so the fix is a small safe reorder: run
   `readProbe()` first, have it return the duplicate verdict, and pass
   `unique = !duplicate` into `publish()`. Frames without a probe
   (`probe_pending_ == false`, e.g. first frame after renegotiation) count
   as unique.
2. **SDL version drift:** baseline validated against SDL 3.2.20 (not 3.4.x
   as in the 07-06 note). No API impact; recorded for reproducibility.

## Phase 2 — implement Milestone 3a: interpolation engine scaffolding

Rationale unchanged from PR #3: the real LSFG shader lift needs the
user-owned `Lossless.dll` and a GPU, neither present here. What can land
now, fully buildable and partly unit-testable, is everything *around* that
shader kernel — so dropping the LSFG chain in later means implementing one
interface on real hardware:

- **Frame pacing** (pure logic, unit-tested, CI-covered): decide, at every
  display refresh, what to show — which real frame, or which (A,B,phase)
  in-between — from the cadence tracker's output and the multiplier.
- **Unique-frame pairing** in the frame pool: an interpolator consumes the
  *last two unique* source frames; today the pool only exposes "latest".
- **A pluggable `Interpolator` interface with a linear-blend baseline**:
  `mix(A, B, phase)` in a small compute shader. Blend is visually mediocre
  (ghosting on motion) but it exercises the entire pipeline — pairing,
  pacing, extra GPU pass, present timing — and gives a working `-m 2/3/4`
  end-to-end. The LSFG chain later replaces the blend behind the same
  interface.

### Increments (each: build clean, tests green, commit)

1. **Add frame pacer with unit tests**
   - `src/core/pacer.{hpp,cpp}`: `FramePacer`, pure std-only, single-threaded
     by contract (like `CadenceTracker`).
   - Input: `onUniqueFrame(seq, t_arrival)` from the capture side, plus the
     current source-fps estimate and multiplier m. Query:
     `decide(t_now) -> {mode, phase}` where mode is `Passthrough` (show
     latest real frame) or `Interpolate` (show pair blend at `phase` ∈ [0,1)).
   - Model: when unique frame B arrives, the A→B interval is presented over
     the *next* source period (one-source-frame display delay, inherent to
     interpolation); phase advances linearly against the recovered source
     period. Falls back to `Passthrough` whenever cadence is not locked,
     m == 1, or arrivals stop (pause → hold last frame; the >0.5 s gap rule
     matches the cadence tracker's reset).
   - `tests/test_pacer.cpp`, same harness style as the cadence tests:
     2x/3x/4x schedules at 24-in-60 and 30-in-60, passthrough fallback when
     unlocked, pause/resume, source-rate drift, phase monotonicity and
     output-rate ≈ m × source fps. Registered in CMake next to the cadence
     test; runs in CI automatically.
2. **Frame pool: unique flag + pair leases**
   - Reorder `readProbe()` before `publish()` in `handleProcess()` (see
     correction 1) so `publish()` gains a `unique` flag.
   - New `acquirePairRead()` → the two most recent unique frames (A, B) with
     their capture timestamps; slot count grows from 3 to 5 so the writer
     still never blocks while a reader holds a pair (5 slots ≥ 2 held by
     reader + 2 latest uniques + 1 write target; asserted in debug builds).
     Existing single-frame `acquireRead()` path unchanged — passthrough
     behavior identical after this commit.
3. **Interpolator interface + blend baseline**
   - `src/vk/interpolate.{hpp,cpp}`: `Interpolator` (record commands to
     produce `dst` from `(A, B, phase)`) and `BlendInterpolator` — one
     compute pipeline sampling A and B, writing an owned intermediate
     image (format chosen by us, so storage support is a non-issue).
   - Shader: `src/shaders/blend.comp` (GLSL) with the compiled SPIR-V
     committed as a generated header plus a `tools/gen_shaders.sh` regen
     script — no new hard build dependency; `glslangValidator` is needed
     only when the GLSL changes (and is available here to generate it).
   - Unit of this commit: "app still builds; interpolator constructed but
     not yet wired".
4. **Wire it up: pacer + interpolator in the render path**
   - Capture feeds `onUniqueFrame` (same lock as the cadence tracker).
   - `Renderer::drawFrame()` consults the pacer: `Interpolate` → run the
     blend into the intermediate and blit that to the swapchain, else blit
     the latest real frame as today.
   - `-m N` now takes effect (m=1 forces passthrough); `G` key toggles frame
     generation at runtime; stats line grows `output 120.0 fps (2x gen)` and
     the video-delay figure now includes the inherent one-source-period hold.
   - README: milestone 3a status, usage, and an honest note on blend quality
     and the added latency (~42 ms at 24 fps source), which will exceed the
     50 ms lipsync target on some setups — measured and displayed, tunable
     later.
5. **CI unchanged** — new pacer tests run under the existing
   `LSFG_BUILD_APP=OFF` job automatically.

### Explicitly out of scope for this pass

- The LSFG shader lift itself (`Lossless.dll` extraction / DXBC→SPIR-V, per
  lsfg-vk). Next milestone, on hardware that has the DLL and a GPU; it slots
  in as a second `Interpolator` implementation.
- Motion-compensated anything — blend is knowingly naive.
- Config file, GUI, latency tuning beyond honest measurement.

### Risks

- **No GPU here**: increments 2–4 are compile-tested only in this
  environment; runtime validation happens on the owner's desktop. Mitigated
  by keeping every commit's default path (passthrough) byte-identical to
  today's behavior and gating FG behind the pacer's lock + multiplier.
- **Pool growth / pair leasing deadlock**: the writer-never-blocks invariant
  is preserved by slot arithmetic (see increment 2); asserted in debug
  builds.
- **Fades/overlays produce "unique" frames at pulldown positions**: pacer
  simply sees more uniques and shorter periods; worst case it presents real
  frames — degrades to passthrough, never worse than today.
