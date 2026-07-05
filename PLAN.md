# PLAN — Linux-Lossless-Frame-Generation (lsfg-cap)

## History

- **2026-07-05 (morning):** Phase 1 recon of the then-empty repository; the
  bootstrap plan was reviewed and merged as PR #1.
- **2026-07-05 (midday):** the project pivoted from the generic skeleton in
  that plan to a concrete tool (see README): capture an arbitrary window via
  the desktop portal and re-present it, with LSFG interpolation to follow.
  Milestones 0/1 (portal/PipeWire capture → Vulkan passthrough window) were
  implemented on `claude/wizardly-clarke-4fk1ip`.
- **2026-07-05 (afternoon):** Milestone 2 (duplicate detection + cadence
  recovery, with unit tests and CI) implemented and merged as PR #2.
- **2026-07-05 (this pass):** fresh recon of the post-milestone-2 codebase,
  recorded below, plus the plan for the milestone 3 frame-generation engine.

## Phase 1 recon — current state (2026-07-05, post-milestone-2)

### Module map

```
CMakeLists.txt          C++20; lsfg_core static lib (pure logic, no system
                        deps) + lsfg-cap app (Vulkan, libpipewire-0.3,
                        libportal, SDL3) + CTest tests. LSFG_BUILD_APP=OFF
                        builds just core+tests (used by CI).
.github/workflows/ci.yml  ubuntu-latest, core-only build, ctest.
src/
  main.cpp        272L  CLI parsing, portal pump, main loop, per-second stats
                        (capture/source/present fps, latency, luma), DRM
                        black-frame verdict (--drm-test)
  options.hpp      20L  Options struct shared across modules
  log.hpp          48L  leveled logging + monotonic nowSeconds()
  portal.{hpp,cpp} 183L PortalSession: ScreenCast handshake via libportal
                        (picker, restore token, close signal)
  capture.{hpp,cpp}947L Capture: PipeWire consumer; DMA-BUF w/ modifier
                        fixation, SHM fallback + mid-stream renegotiation;
                        copies each frame into the pool; 64×64 probe every
                        frame → duplicate compare (1-LSB tolerance, xx-byte
                        skipped) feeds CadenceTracker; luma for DRM test.
                        NOTE: probe is read AFTER publish(), so the pool
                        frame does not carry its own duplicate flag yet.
  renderer.{hpp,cpp}225L Renderer: blits latest pool frame to the SDL3/Vulkan
                        swapchain letterboxed, once per acquire; latency EMA.
                        No shader pipelines exist anywhere yet — the render
                        path is pure transfer commands (clear + blit).
  core/cadence.*   203L CadenceTracker (pure, tested): source fps from span
                        mean over whole pulldown periods, pattern from repeat
                        run-lengths ("3:2"/"2:2"/"1:1"/"irregular"), lock
                        flag from window-half agreement, gap reset at 0.5 s
  vk/context.*     402L instance/device/queue/swapchain, DRM modifier query,
                        shared graphics queue + mutex
  vk/frame_pool.*  202L triple-buffered VkImage pool, capture=writer /
                        render=reader, publish/acquireRead lease model;
                        published images live in TRANSFER_SRC_OPTIMAL
  vk/dmabuf_import.*150L VkImage import of PipeWire DMA-BUF planes
tests/test_cadence.cpp 184L  7 scenario tests driving CadenceTracker
```

### Build & test baseline (this environment, fresh container)

- Ubuntu 24.04. `libpipewire-0.3-dev` (1.0.5), `libportal-dev` (0.7.1),
  `libvulkan-dev` (1.3.275) from apt. SDL3 still not packaged on 24.04;
  rebuilt SDL 3.4.12 from the `sdl3-src` crates.io tarball (GitHub remains
  blocked by network policy; crates.io API needs a User-Agent header) into a
  scratch prefix — this time as a console-only build
  (`-DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_UNIX_CONSOLE_BUILD=ON`, since X11
  dev headers aren't installed and the container has no display anyway).
- Full app: `cmake -B build -G Ninja && cmake --build build` → **compiles
  and links clean, zero warnings**; `./build/lsfg-cap --help` runs.
- Core: `-DLSFG_BUILD_APP=OFF` builds; `ctest` → **1/1 passed** (cadence).
- Not runtime-testable here: anything needing a display/portal/GPU. No
  `glslc`/`glslangValidator` installed by default (relevant below).

## Phase 2 — Milestone 3a: frame-generation engine

### Why "3a"

Milestone 3 as scoped in the README is "LSFG shader integration, 2x". The
LSFG shader chain itself cannot land from this environment: it requires a
user-owned `Lossless.dll` (never bundled), lsfg-vk's sources as reference
(GitHub blocked here), and a real GPU to debug against. What *can* land now,
correctly and testably, is everything the shader chain will plug into — and
that is also the part where latent design mistakes would be expensive later:

- **frame pacing** (when to present which real/generated frame),
- **duplicate withholding** (interpolate unique frames, not repaints),
- **a two-frame history** the interpolator reads from,
- **a pluggable `Interpolator` interface** with a trivial built-in blend
  (crossfade) implementation to prove the whole path end-to-end.

Split the milestone: **3a — engine (this pass)**, **3b — LSFG shader chain**
(next pass, on hardware, per lsfg-vk's approach). The blend interpolator is
an explicitly-labelled placeholder: it will look like ghosting/crossfade,
which is correct behavior for a pacing testbed, not the shipped experience.

### Increments (each: build clean, tests green, commit)

1. **Add frame pacer core with unit tests**
   - `src/core/pacer.{hpp,cpp}`: `FramePacer`, pure std-only logic, same
     contract as CadenceTracker (caller serializes; no system deps).
   - Inputs: `setMultiplier(m)`, `onUniqueFrame(seq, t)` (from capture),
     `onCadence(fps, locked)` (periodic snapshot). Query from the render
     loop: `frameForPresent(t_now) -> {base_seq, phase, generated}` — which
     source frame pair to show and the interpolation phase in [0,1).
   - Policy: with a cadence lock and m > 1, presentation runs one source
     interval behind arrival (pair (A,B) is on screen while C arrives —
     the standard frame-interpolation delay); phases are k/m through the
     interval. Without a lock, or m == 1, degrade to "latest frame,
     phase 0" (today's behavior). A gap > 0.5 s (pause/seek, matching the
     tracker) snaps to latest and re-arms.
   - `tests/test_pacer.cpp` (same assert-macro style as test_cadence):
     24 fps source × m=2 on a 60 Hz tick, 30 × m=2, m=1 passthrough,
     no-lock fallback, pause/resume snap, late/jittery arrivals, cadence
     change mid-stream, 3x/4x phase sequences (engine supports any m even
     though only 2x ships as "supported" this pass).

2. **Attach the duplicate flag to published frames**
   - In `Capture::handleProcess`, read the probe *before* `publish()` (the
     copy+probe were already fence-waited by then — pure reorder, no new
     synchronization), and pass `duplicate` through
     `FramePool::publish()`/`Slot`/`ReadLease`, plus a monotonically
     increasing `unique_seq`. Capture also calls `pacer.onUniqueFrame`.
   - No behavior change for the renderer yet; app still passthrough.

3. **Two-frame history + `Interpolator` interface + blend placeholder**
   - `src/vk/frame_history.*`: renderer-side pair of images (prev/cur
     unique frame). On each render tick, if the pool has a new *unique*
     frame, copy it in (transfer, same proven barrier/fence style as the
     existing paths). Duplicates are consumed but not copied — this is the
     "withhold duplicates" step, done on the consumer side so the pool and
     capture thread stay simple.
   - `src/interpolate.hpp`: `class Interpolator { generate(A, B, phase,
     dst); }` — the seam where lsfg-vk's chain plugs in for 3b.
   - `src/interp_blend.*`: compute-shader crossfade
     (`out = mix(A, B, phase)`). First shader in the repo: GLSL source
     checked in under `shaders/`, SPIR-V embedded as a generated header
     also checked in (regenerated by CMake when `glslangValidator` or
     `glslc` is present, so builds never require a shader compiler).
     Output image is `R8G8B8A8_UNORM` with STORAGE usage (the one format
     with universally guaranteed storage-image support; the final blit
     handles any component-order conversion to the swapchain).
   - Unit-testable parts stay in core; this increment is compile-clean +
     existing tests green (no GPU here to run it — see risks).

4. **Wire the multiplier: paced presentation**
   - Renderer consults the pacer each tick: phase 0 → blit the real frame
     (exactly today's path); otherwise generate into the intermediate and
     blit that. `-m 2` becomes functional end-to-end (blend placeholder).
   - Stats line grows output info: `out 60.0 fps (2x of 23.98, interp
     blend)`; latency line now reflects the deliberate +1-source-interval
     buffering (≈42 ms at 24 fps) — the < 50 ms lipsync target needs
     re-evaluating on hardware once 3b's real shader cost is known.
   - `-m 1` (default) keeps the exact current passthrough path.

5. **Docs**
   - README: milestone table splits 3 into 3a (done) / 3b (LSFG chain,
     needs owned Lossless.dll + hardware); usage/`-m` notes; design notes
     for pacing and the interpolator seam. PLAN.md history updated.

CI is untouched: the new pacer test builds under the existing
`LSFG_BUILD_APP=OFF` core configuration automatically.

### Explicitly out of scope for this pass

- **3b:** loading/translating Lossless.dll shaders (lsfg-vk approach) —
  needs owned assets, lsfg-vk reference sources, and a GPU; none exist in
  this environment.
- Milestone 4 (3x/4x productization, polish) — the pacer and interface are
  built m-agnostic, but only 2x is claimed/documented as working.
- The Milestone 0 hardware question (is Crunchyroll-in-Firefox capture
  black?) still needs a manual `--drm-test` run on a real desktop.

### Risks

- **No GPU/display here**: increments 3–4 are compile-clean but not
  runtime-verified until a hardware run. Mitigation: all decision logic
  (pacing, withholding policy) lives in the unit-tested core; the Vulkan
  additions reuse the exact barrier/fence/queue-mutex patterns already
  proven in capture/renderer, and the first hardware run has a clear
  checklist (validation layers on, `-m 1` regression first, then `-m 2`).
- **Blend ghosting**: inherent to the placeholder; it demonstrates pacing
  correctness (motion cadence smooths) while looking soft. Clearly labelled
  in `--help`, stats line, and README so nobody mistakes it for LSFG.
- **Added latency**: +1 source interval is fundamental to interpolation;
  at 24 fps content that is ~42 ms on top of the current ~capture-to-present
  delay. If hardware runs show lipsync failing, 3b gains a config to trade
  multiplier for delay (e.g. interpolate from a shorter history) — noted,
  not solved, here.
- **Storage-image format support**: sidestepped by writing to
  `R8G8B8A8_UNORM` (mandatory storage support) and letting the existing
  blit convert; worst case is one extra transfer we already pay today.
