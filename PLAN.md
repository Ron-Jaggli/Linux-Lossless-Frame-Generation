# PLAN — Linux-Lossless-Frame-Generation (lsfg-cap)

## History

- **2026-07-05 (morning):** Phase 1 recon of the then-empty repository; the
  bootstrap plan was reviewed and merged as PR #1.
- **2026-07-05 (midday):** the project pivoted to the concrete tool described
  in the README. Milestones 0/1 (portal/PipeWire capture → Vulkan passthrough
  window) were implemented on `claude/wizardly-clarke-4fk1ip`.
- **2026-07-05 (evening):** Milestone 2 (duplicate detection + cadence
  recovery, unit tests, CI) implemented and merged as PR #2.
- **2026-07-06:** fresh recon of the milestone-2 codebase, recorded below,
  plus the plan for the next increment: the interpolation engine scaffolding
  for Milestone 3. Reviewed and approved by merge of PR #3.
- **2026-07-08 (this pass):** re-established the build/test baseline in a
  fresh container (identical result: core `ctest` 1/1 green, full app
  compiles and links against a scratch SDL 3.4.12 built with
  `-DSDL_UNIX_CONSOLE_BUILD=ON`; no GPU/display for runtime testing), then
  executed the approved milestone-3a increments below. One correction found
  during implementation: guaranteeing a never-blocking writer alongside a
  held pair lease needs **6** pool slots, not 5 — worst case the writer must
  avoid 2 reader-held slots + `latest` (a duplicate, distinct from any
  unique) + the 2 tracked uniques.

## Phase 1 recon — current state (2026-07-06)

### Module map

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

### Build & test baseline (this environment, fresh container)

- Ubuntu 24.04 container, cmake 3.28 / ninja / g++ 13. Installed
  `libpipewire-0.3-dev libportal-dev libvulkan-dev` from apt; SDL3 is still
  not packaged on 24.04, so a minimal console-only SDL 3.4.12 was built from
  the `sdl3-src` crate tarball (crates.io is reachable; GitHub is not) into a
  scratch prefix for compile/link validation.
- Core: `cmake -B build-core -G Ninja -DLSFG_BUILD_APP=OFF` → builds clean,
  `ctest` **1/1 passed** (the cadence suite).
- App: compiles and links clean against the scratch SDL3 (see baseline note
  in the commit that accompanies this plan). Nothing app-side is runtime
  testable here: no display, no portal, no GPU. Runtime behavior is
  validated on the owner's desktop.
- CI (core tests only) is green on `main`.

### Where the project stands

Milestones 0–2 are code-complete. The two open questions from the last plan
are unchanged and cannot be answered from a container:

1. **The Milestone 0 hardware question** — is Crunchyroll-in-Firefox capture
   black? Needs one manual `--drm-test` run on the owner's desktop.
2. **Milestone 3** — actual frame interpolation.

## Phase 2 — Milestone 3a: interpolation engine scaffolding

**Status (2026-07-08): implemented.** All five increments below landed as
individual commits on `claude/wizardly-clarke-8r6dim`; core tests (cadence +
new pacer suite) green, full app compiles and links. Deviations from the
plan as written: the pool grew to 6 slots (see History) rather than 5, the
`G` runtime toggle simply forces multiplier 1 rather than a separate flag,
and `--drm-test` forces passthrough so the verdict measures the raw
capture. Runtime validation on real hardware (GPU + portal) is the
remaining step, as anticipated.

Milestone 3 as originally scoped ("lift the LSFG shader chain from
Lossless.dll the way lsfg-vk does") has two hard external dependencies:
the user-owned `Lossless.dll` and a real GPU to validate against — neither
exists in this environment, and the lsfg-vk sources aren't reachable from
here either. Attempting it now would mean stacking a large amount of
unverifiable code.

What *can* land now, fully buildable and partly unit-testable, is everything
around that shader kernel — so that dropping the LSFG chain in later is a
matter of implementing one interface on real hardware:

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
     output-rate ≈ m × source fps.
2. **Frame pool: unique-frame pair leases**
   - `publish()` gains a `unique` flag (capture already knows duplicate
     status at publish time from the probe compare).
   - New `acquirePairRead()` → the two most recent unique frames (A, B) with
     their capture timestamps; slot count grows from 3 to 5 so the writer
     still never blocks while a reader holds a pair. Existing single-frame
     `acquireRead()` path unchanged — passthrough behavior identical after
     this commit.
3. **Interpolator interface + blend baseline**
   - `src/vk/interpolate.{hpp,cpp}`: `Interpolator` (record commands to
     produce `dst` from `(A, B, phase)`) and `BlendInterpolator` — one
     compute pipeline sampling A and B, writing an owned intermediate
     image (format chosen by us, so storage support is a non-issue).
   - Shader: `src/shaders/blend.comp` (GLSL) with the compiled SPIR-V
     committed as a generated header plus a `tools/gen_shaders.sh` regen
     script — no new hard build dependency; `glslangValidator` is needed
     only when the GLSL changes.
   - Compiles and unit of this commit is "app still builds; interpolator
     constructed but not yet wired".
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
  is preserved by slot arithmetic (5 slots ≥ 2 held by reader + 2 latest
  uniques + 1 write); asserted in debug builds.
- **Fades/overlays produce "unique" frames at pulldown positions**: pacer
  simply sees more uniques and shorter periods; worst case it presents real
  frames — degrades to passthrough, never worse than today.
