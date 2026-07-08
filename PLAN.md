# PLAN — Linux-Lossless-Frame-Generation (lsfg-cap)

## History

- **2026-07-05 (morning):** Phase 1 recon of the then-empty repository; the
  bootstrap plan was reviewed and merged as PR #1.
- **2026-07-05 (midday):** the project pivoted to the concrete tool described
  in the README. Milestones 0/1 (portal/PipeWire capture → Vulkan passthrough
  window) were implemented on `claude/wizardly-clarke-4fk1ip`.
- **2026-07-05 (evening):** Milestone 2 (duplicate detection + cadence
  recovery, unit tests, CI) implemented and merged as PR #2.
- **2026-07-06:** recon of the milestone-2 codebase and the plan for
  Milestone 3a (interpolation engine scaffolding) written and merged as
  PR #3 — **but the 3a increments themselves were never implemented**; only
  README edits landed afterwards.
- **2026-07-08 (this pass):** fresh recon, baseline re-verified, plan below.
  The scope is unchanged from the approved 3a plan; this revision folds in
  concrete findings from re-reading the code (publish/probe ordering, pool
  lease mechanics, renderer submit path) and adds an optional CI increment.

## Phase 1 recon — current state (2026-07-08)

### Repository state

- Working branch `claude/wizardly-clarke-4wz6jq` is identical to
  `origin/main` (7385e4a), tree clean. No PR is open for it.
- Since the 3a plan merged, only README/docs commits landed. **Milestone 3a
  is approved but unimplemented** — `src/core/pacer.*`, `src/vk/interpolate.*`,
  pair leases, and `tests/test_pacer.cpp` do not exist; `Options::multiplier`
  is still parsed but inert (`src/options.hpp`).

### Module map (unchanged since 2026-07-06 except line counts)

```
CMakeLists.txt           C++20; lsfg_core static lib (pure logic, no system
                         deps) + lsfg-cap app (gated by LSFG_BUILD_APP) +
                         CTest unit tests (BUILD_TESTING)
.github/workflows/ci.yml ubuntu-latest, LSFG_BUILD_APP=OFF, core build +
                         ctest on every push/PR
src/
  main.cpp          272L CLI parsing, portal pump, main loop, per-second
                         stats line, DRM black-frame verdict
  options.hpp        20L Options struct; multiplier parsed but inert
  log.hpp            48L leveled logging + monotonic nowSeconds()
  portal.{hpp,cpp}  183L ScreenCast handshake (libportal), picker, restore
                         token, close signal
  capture.{hpp,cpp} 947L PipeWire consumer: DMA-BUF negotiation w/ modifier
                         fixation, SHM fallback + mid-stream renegotiation,
                         copy into FramePool, 64×64 probe every frame →
                         duplicate compare → CadenceTracker (mutex-guarded),
                         luminance for the DRM test
  renderer.{hpp,cpp}225L blits latest pool frame to SDL3/Vulkan swapchain,
                         letterboxed; per-frame fence wait; latency EMA
  core/cadence.*    203L CadenceTracker: recovers source fps + repeat pattern
                         from (timestamp, dup) events; pure, unit-tested
  vk/context.*      402L instance/device/queue/swapchain, DRM modifier query
  vk/frame_pool.*   202L triple-buffered VkImage pool, writer=capture thread,
                         reader=render thread, publish/acquireRead leases
  vk/dmabuf_import.*150L VkImage import of PipeWire DMA-BUF planes
tests/test_cadence.cpp   7 scenario tests via a tiny CHECK macro harness
```

### Build & test baseline (this environment, fresh container)

- Ubuntu 24.04 container, cmake 3.28 / ninja / g++ 13. Installed
  `libpipewire-0.3-dev libportal-dev libvulkan-dev glslang-tools` from apt.
  SDL3 is still not packaged on 24.04; built a console-only SDL 3.4.12
  (`-DSDL_UNIX_CONSOLE_BUILD=ON`, X11/Wayland off) from the `sdl3-src`
  tarball on static.crates.io (the crates.io API endpoint now 403s the
  default curl UA; static.crates.io works) into a scratch prefix.
- Core: `cmake -B build-core -G Ninja -DLSFG_BUILD_APP=OFF` → builds clean,
  `ctest` **1/1 passed** (cadence suite).
- App: full `lsfg-cap` compiles and links **clean, zero warnings** against
  the scratch SDL3. Not runtime-testable here (no display/portal/GPU);
  runtime behavior is validated on the owner's desktop.
- CI (core tests only) is green on `main`.
- `glslangValidator` 15.1.0 available for compiling the blend shader.

### Recon findings that sharpen the 3a design

1. **Duplicate status is decidable at publish time.** In
   `Capture::handleProcess()` the pool blit and the 64×64 probe downscale
   are recorded into one command buffer and fence-waited by
   `submitAndWait()` *before* `pool_->publish()` runs; the probe compare
   (`readProbe()`) merely reads the already-mapped buffer but is currently
   called *after* publish. Reordering `readProbe()` before `publish()` is
   safe and lets `publish()` carry an accurate `unique` flag — no extra
   synchronization needed.
2. **The renderer already fence-waits per frame** before releasing its read
   lease (`Renderer::drawFrame()`), so extending the same submit with a
   compute blend pass keeps the existing lease-release and queue-mutex
   discipline unchanged.
3. **Pool slot arithmetic:** with pair leases the reader holds 2 slots and
   the writer must always find a free one while the 2 latest uniques stay
   protected → 5 slots (was 3), as in the approved plan. Verified against
   `frame_pool.cpp`: `acquireWrite()` picks any slot that is neither
   `latest_` nor `reading_`; the generalization is to exclude the reader's
   held set and the last two unique slots.

## Phase 2 — Milestone 3a: interpolation engine scaffolding

Unchanged in scope from the approved 2026-07-06 plan. Rationale recap: the
real LSFG shader lift needs `Lossless.dll` and a GPU, neither available
here; what can land now — fully buildable, partly unit-testable — is
everything around that kernel, so the LSFG chain later drops in behind one
interface.

### Increments (each: build clean, tests green, commit)

1. **Add frame pacer with unit tests**
   - `src/core/pacer.{hpp,cpp}`: `FramePacer`, pure std-only, single-threaded
     by contract (like `CadenceTracker`), added to `lsfg_core`.
   - Input: `onUniqueFrame(seq, t_arrival)` plus current source-fps estimate
     and multiplier m. Query: `decide(t_now) -> {mode, phase}`, mode ∈
     {`Passthrough`, `Interpolate`}, phase ∈ [0,1).
   - Model: when unique frame B arrives, the A→B interval is presented over
     the *next* source period (one-source-period display delay, inherent to
     interpolation); phase advances linearly against the recovered source
     period. Falls back to `Passthrough` when cadence is unlocked, m == 1,
     or arrivals stop (>0.5 s gap → hold last frame, matching the cadence
     tracker's reset rule).
   - `tests/test_pacer.cpp`, same CHECK-macro harness as the cadence tests:
     2x/3x/4x at 24-in-60 and 30-in-60, passthrough fallback when unlocked,
     pause/resume, source-rate drift, phase monotonicity, output rate ≈
     m × source fps.
2. **Frame pool: unique-frame pair leases**
   - Reorder `readProbe()` before `publish()` in `handleProcess()` (finding
     1) so `publish()` gains a `unique` flag.
   - New `acquirePairRead()` → the two most recent unique frames (A, B) with
     capture timestamps; slot count 3 → 5 so the writer never blocks while a
     reader holds a pair (finding 3; asserted in debug builds).
   - Existing `acquireRead()` path unchanged — passthrough behavior
     byte-identical after this commit.
3. **Interpolator interface + blend baseline**
   - `src/vk/interpolate.{hpp,cpp}`: `Interpolator` (record commands
     producing `dst` from `(A, B, phase)`) and `BlendInterpolator` — one
     compute pipeline sampling A and B, writing an owned intermediate image
     (format chosen by us, so storage-image support is a non-issue).
   - Shader `src/shaders/blend.comp` (GLSL); compiled SPIR-V committed as a
     generated header + `tools/gen_shaders.sh` regen script — no new hard
     build dependency (`glslangValidator` needed only when the GLSL
     changes).
   - Deliverable of this commit: app still builds; interpolator constructed
     but not yet wired.
4. **Wire it up: pacer + interpolator in the render path**
   - Capture feeds `onUniqueFrame` (same lock as the cadence tracker).
   - `Renderer::drawFrame()` consults the pacer: `Interpolate` → blend into
     the intermediate, blit that to the swapchain; else blit the latest real
     frame exactly as today.
   - `-m N` takes effect (m=1 forces passthrough); `G` key toggles frame
     generation at runtime; stats line grows `output 120.0 fps (2x gen)`;
     the video-delay figure now includes the inherent one-source-period
     hold.
   - README: milestone 3a status, usage, honest note on blend quality
     (ghosting) and added latency (~42 ms at 24 fps source vs the 50 ms
     lipsync target — measured and displayed, tunable later).
5. **CI: pacer tests ride the existing core job automatically.**
   *Optional, if time allows:* a second CI job that apt-installs
   pipewire/portal/vulkan dev packages, builds the console-only SDL3 from
   the static.crates.io tarball (cached via `actions/cache`), and compiles
   the full app — turning "app builds are validated locally" into CI
   coverage. Skipped without complaint if the runner network blocks
   static.crates.io.

### Explicitly out of scope for this pass

- The LSFG shader lift itself (`Lossless.dll` extraction / DXBC→SPIR-V per
  lsfg-vk). Next milestone, on hardware with the DLL and a GPU; it slots in
  as a second `Interpolator` implementation.
- Motion-compensated anything — blend is knowingly naive.
- Config file, GUI, latency tuning beyond honest measurement.

### Risks

- **No GPU here:** increments 2–4 are compile-tested only; runtime
  validation happens on the owner's desktop. Mitigated by keeping every
  commit's default path (passthrough) byte-identical to today and gating FG
  behind pacer lock + multiplier + the `G` toggle.
- **Pool growth / pair-lease deadlock:** writer-never-blocks invariant
  preserved by slot arithmetic (5 ≥ 2 held by reader + 2 latest uniques +
  1 write); asserted in debug builds.
- **Fades/overlays produce "unique" frames at pulldown positions:** the
  pacer just sees more uniques and shorter periods; worst case it presents
  real frames — degrades to passthrough, never worse than today.
