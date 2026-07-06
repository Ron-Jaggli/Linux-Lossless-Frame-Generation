# PLAN — Linux-Lossless-Frame-Generation (lsfg-cap)

## History

- **2026-07-05 (morning):** Phase 1 recon of the then-empty repository; the
  bootstrap plan was reviewed and merged as PR #1.
- **2026-07-05 (midday):** the project pivoted from the generic skeleton in
  that plan to a concrete tool (see README): capture an arbitrary window via
  the desktop portal and re-present it, with LSFG interpolation to follow.
  Milestones 0/1 (portal/PipeWire capture → Vulkan passthrough window) were
  implemented on `claude/wizardly-clarke-4fk1ip`.
- **2026-07-05 (evening):** Milestone 2 (duplicate detection + cadence
  recovery) plus unit tests and CI, merged as PR #2.
- **2026-07-06 (this pass):** fresh recon below, plus the plan for the
  frame-generation engine — the machinery half of Milestone 3.

## Phase 1 recon — current state (2026-07-06)

### Module map

```
CMakeLists.txt          C++20; lsfg_core static lib (pure logic, no deps) +
                        lsfg-cap app (Vulkan, libpipewire-0.3, libportal, SDL3);
                        LSFG_BUILD_APP=OFF builds core+tests only (used by CI)
.github/workflows/ci.yml  ubuntu-latest, core-only configure, build, ctest
src/
  main.cpp        273L  CLI parsing, portal pump, main loop, per-second stats
                        (capture/source/present fps, delay, luma), DRM verdict
  options.hpp           Options struct shared across modules
  log.hpp               leveled logging + monotonic nowSeconds()
  portal.{hpp,cpp}      PortalSession: ScreenCast handshake via libportal
                        (picker, restore token, close signal)
  capture.{hpp,cpp}     Capture: PipeWire consumer on pw_thread_loop. DMA-BUF
                        w/ modifier fixation, SHM fallback + mid-stream
                        renegotiation; copies every frame into FramePool;
                        64×64 probe/frame → duplicate detect (byte compare,
                        1-LSB tolerance) → CadenceTracker; probe luma → DRM test
  renderer.{hpp,cpp}    Renderer: acquireRead latest pool frame → letterboxed
                        blit to SDL3/Vulkan swapchain; latency EMA
  core/cadence.{hpp,cpp} CadenceTracker (pure, single-threaded by contract):
                        (t, is_dup) events → source fps, pattern ("3:2"…),
                        dup ratio, lock flag; gap >0.5 s resets window
  vk/context.*          instance/device/queue, swapchain, DRM modifier query
  vk/frame_pool.*       triple-buffered VkImage pool, publish/acquireRead
                        lease model (capture writer / render reader)
  vk/dmabuf_import.*    VkImage import of PipeWire DMA-BUF planes
tests/test_cadence.cpp  7 cadence cases (3:2, 2:2, 1:1, damage-driven, jitter,
                        cadence change, pause/resume), assert-macro based
```

### Build & test baseline (this environment)

- Ubuntu 24.04 container, no GPU/display. `libpipewire-0.3-dev`,
  `libportal-dev`, `libvulkan-dev`, `glslang-tools` from apt. SDL3 still not
  packaged on 24.04 → built SDL 3.4.12 from source (crates.io `sdl3-src`
  tarball; GitHub/libsdl.org blocked) headless
  (`SDL_UNIX_CONSOLE_BUILD=ON`, video/audio off) into a scratch prefix —
  link-valid, not runnable, which is all this container can use anyway.
- Full app build (`cmake -B build -G Ninja && cmake --build build`):
  **clean, zero warnings**. `./build/lsfg-cap --help` runs.
- Core-only build (`-DLSFG_BUILD_APP=OFF`, what CI runs): clean; `ctest`
  **1/1 passed** (cadence).
- Runtime capture/present paths remain validated only on real desktops
  (needs portal + GPU); unchanged situation from previous passes.

## Phase 2 — Milestone 3 (part 1): the frame-generation engine

README milestone 3 is "LSFG shader integration, 2x interpolation". It splits
cleanly in two:

- **The engine** (this pass): everything around the shader — frame history,
  present re-timing, cadence-locked pacing, a compute-dispatch interpolation
  stage, and a built-in **blend interpolator** so `-m 2` produces real
  intermediate frames end-to-end.
- **The LSFG shader chain** (next pass): lifting Lossless Scaling's shaders
  per lsfg-vk. Deliberately excluded here: it requires the user's
  `Lossless.dll` (Steam) and a GPU to validate — neither exists in this
  container, so that code would be unbuildable-in-anger and untestable. The
  engine is designed so the LSFG chain drops in behind the same
  `Interpolator` interface.

Why an engine-first cut is genuinely useful: with cadence lock from
milestone 2, blend interpolation of *unique* frames already converts 3:2
judder into evenly paced output, and it exercises every hard part
(threading, layouts, pacing, latency) that LSFG will need.

### Design

Data flow with `-m N` (N > 1):

```
capture thread → FramePool (unchanged, public API preserved)
render thread:  FrameGen stage
                  - polls pool; each NEW seq is copied into a 2-deep history
                    ring [A = previous, B = latest] (compute-usable images)
                  - FramePacer (pure core logic) maps present-time → position
                    on the source timeline → (show A? show B? interpolate φ)
                  - interpolation: compute dispatch blend(A, B, φ) → out image
                Renderer blits the chosen image (unchanged blit/letterbox path)
```

Key decisions:

- **History ring lives in FrameGen, not FramePool.** The pool's lease API
  and triple-buffer invariants stay untouched (public interface preserved);
  FrameGen briefly holds a read lease only to copy new frames out.
- **Pacing is pure logic in `src/core/pacer.{hpp,cpp}`** so it gets real
  unit tests: given (t_now, tA, tB, source interval from CadenceStats,
  multiplier) it returns {frame choice, phase φ ∈ (0,1)}. Presenting between
  A and B necessarily adds ~one source-frame interval of delay; that is
  inherent to interpolation, shown in the existing delay stat, and absent at
  `-m 1`.
- **Fallback is passthrough.** No cadence lock yet, source paused
  (gap > 0.5 s), multiplier 1, or interpolation init failure → present
  latest frame exactly as today. `-m 1` never enters FrameGen.
- **Duplicates are withheld from interpolation** (only unique frames enter
  the history ring) — cadence data finally gets consumed, as milestone 2
  anticipated. Passthrough fallback still shows every frame.
- **Shader build:** `shaders/blend.comp` (GLSL) → SPIR-V at build time via
  `glslangValidator` (new app-build dependency; packaged in Fedora
  `glslang` / Ubuntu `glslang-tools`), embedded as a C array with CMake
  `file(READ … HEX)` — no runtime file loading, no FetchContent (network
  policy). CI is unaffected (`LSFG_BUILD_APP=OFF` builds no shaders).

### Increments (each: build clean, tests green, commit)

1. **Add FramePacer with unit tests** — `src/core/pacer.{hpp,cpp}`,
   `tests/test_pacer.cpp`, CTest wiring. Cases: 30→60 2x (φ=0.5 midpoints),
   23.976 3:2 → evenly re-timed 2x, 60→60 2x, unlock → passthrough decision,
   pause/seek gap → immediate cut to B, multiplier 3/4 phase series.
2. **Add SPIR-V shader build + blend compute pipeline** — CMake
   custom-command pipeline for `shaders/blend.comp`, `src/vk/interp.{hpp,cpp}`
   (descriptor set, pipeline, `dispatch(A, B, φ, out)`); compile/link
   validation here, no runtime path yet touches it.
3. **Add FrameGen stage and wire it behind -m** — `src/framegen.{hpp,cpp}`
   (history ring + pacer + interp), renderer consumes its output when active;
   `-m 1` bit-identical behavior to today. Update the milestone log line.
4. **Extend stats line and docs** — `fg 2x (blend) N fps` in stats; README:
   milestone table row for the engine, glslang build dep, `-m` semantics;
   PLAN.md history entry.

### Explicitly out of scope for this pass

- LSFG shader chain / `Lossless.dll` loading (next pass, needs user assets +
  GPU; the `Interpolator` interface is its landing pad).
- Motion-vector or optical-flow interpolation of our own — blend is the
  honest placeholder; anything fancier is LSFG's job.
- Config file, GUI (roadmap items, unchanged).
- The Milestone 0 hardware question (Crunchyroll `--drm-test` on a real
  desktop) — still user homework; no code here can answer it.

### Risks

- **No GPU anywhere in the loop for the Vulkan additions** — same standing
  risk as milestones 0–2: compile-only validation here, runtime validation
  on the user's desktop. Mitigated by keeping `-m 1` (the validated path)
  untouched and all new code behind the multiplier flag + graceful fallback.
- **Timestamp domains:** pacing mixes capture timestamps with present-time
  `nowSeconds()`; both already use the same monotonic clock (capture stamps
  arrival with `nowSeconds()`), so no conversion is needed — asserted in
  code comments rather than discovered in production.
- **Blend ghosting:** a 50/50 blend of fast motion looks soft/ghosty. That
  is expected for the placeholder and disappears with the LSFG chain;
  README will say so to preempt "it looks blurry" reports.
- **glslangValidator availability** on user machines: Fedora/Ubuntu both
  package it; CMake fails at configure time with a clear message if absent
  (better than a silent runtime miss).
