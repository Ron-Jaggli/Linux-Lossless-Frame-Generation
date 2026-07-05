# PLAN — Linux-Lossless-Frame-Generation (lsfg-cap)

## History

- **2026-07-05 (morning):** Phase 1 recon of the then-empty repository; the
  bootstrap plan was reviewed and merged as PR #1.
- **2026-07-05 (midday):** the project pivoted from the generic skeleton in
  that plan to a concrete tool (see README): capture an arbitrary window via
  the desktop portal and re-present it, with LSFG interpolation to follow.
  Milestones 0/1 (portal/PipeWire capture → Vulkan passthrough window) were
  implemented on `claude/wizardly-clarke-4fk1ip`.
- **2026-07-05 (this pass):** fresh recon of the actual codebase, recorded
  below, plus the plan for Milestone 2.

## Phase 1 recon — current state (2026-07-05)

### Module map

```
CMakeLists.txt          C++20, single executable; deps: Vulkan, libpipewire-0.3,
                        libportal, SDL3 (pkg-config)
src/
  main.cpp        265L  CLI parsing, portal handshake pump, main loop,
                        per-second stats line, DRM black-frame verdict
  options.hpp      20L  Options struct shared across modules
  log.hpp          48L  leveled printf-style logging + monotonic nowSeconds()
  portal.{hpp,cpp} 183L PortalSession: xdg-desktop-portal ScreenCast handshake
                        via libportal (picker dialog, restore token, close signal)
  capture.{hpp,cpp}900L Capture: PipeWire stream consumer. DMA-BUF negotiation
                        with DRM modifier fixation, SHM fallback (incl. mid-
                        stream renegotiation), per-frame copy into the pool,
                        16×16 luminance probe (every 30th frame; every frame
                        under --drm-test)
  renderer.{hpp,cpp}225L Renderer: blits latest pool frame to the SDL3/Vulkan
                        swapchain, letterboxed; latency EMA
  vk/context.*     402L instance/device/queue setup, swapchain, DRM modifier
                        query, validation layers
  vk/frame_pool.*  202L triple-buffered VkImage pool decoupling capture (writer)
                        from render (reader); publish/acquireRead lease model
  vk/dmabuf_import.*150L VkImage import of PipeWire DMA-BUF planes
```

No tests and no CI exist yet.

### Build & test baseline (this environment)

- Ubuntu 24.04 container. `libpipewire-0.3-dev`, `libportal-dev`,
  `libvulkan-dev` installed from apt. SDL3 is not packaged on 24.04; built
  minimal SDL 3.4.12 from source (from the `sdl3-src` crate tarball — GitHub
  and libsdl.org are blocked by the network policy here) into a scratch
  prefix and pointed `PKG_CONFIG_PATH` at it.
- `cmake -B build -G Ninja && ninja -C build`: **compiles and links clean,
  zero warnings** (`-Wall -Wextra`).
- `./build/lsfg-cap --help` runs. Nothing else is runtime-testable in this
  container (no display, portal, or GPU).
- Tests: none exist → nothing to run. This is the gap Milestone 2 starts
  closing: its core logic is pure and will land with unit tests.

## Phase 2 — Milestone 2: duplicate detection + cadence recovery

Why: a 60 Hz compositor repaints a 23.976 fps video on a 3:2 pulldown
(frames repeat 3,2,3,2,…). Interpolating between *repaints* would blend
identical frames; Milestone 3 needs the stream of *unique* frames and the
recovered source cadence. Some compositors also deliver buffers only on
damage — then duplicates never arrive and cadence must come from arrival
times alone. Both cases are handled by one tracker fed
`(timestamp, is_duplicate)` events.

### Increments (each: build clean, tests green, commit)

1. **Add cadence tracker with unit tests**
   - `src/core/cadence.{hpp,cpp}`: `CadenceTracker`, pure std-only logic, no
     Vulkan/PipeWire dependencies, single-threaded by contract (caller locks).
   - Input: `addFrame(t_seconds, is_duplicate)`. Output `CadenceStats`:
     estimated source fps, repeat-pattern label ("3:2", "2:2", "1:1",
     "irregular"), duplicate ratio, lock flag, frame counts.
   - Source fps from the mean unique-frame interval over a sliding window
     (mean, not median — pulldown alternates 50 ms/33 ms intervals whose
     *mean* is the true 41.7 ms); pattern from repeat run-lengths;
     discontinuities (> 250 ms gap, i.e. pause/seek) reset the window.
   - `tests/test_cadence.cpp` driven by CTest with a small assert macro —
     no external framework (network policy here blocks FetchContent from
     GitHub, and a header-only vendored framework is overkill at this size).
     Cases: 23.976→60 Hz 3:2 pulldown, 30→60 2:2, 60→60 passthrough,
     damage-driven sparse delivery (no duplicates), timestamp jitter,
     mid-stream cadence change, pause/resume.
   - CMake: `BUILD_TESTING` (standard CTest toggle) builds the test; a new
     `LSFG_BUILD_APP` option (default **ON**, so normal builds are unchanged)
     lets CI build/run the pure tests without SDL3/PipeWire/portal installed.

2. **Detect duplicate frames in the capture path**
   - Grow the existing probe from 16×16/every-30th to 64×64/every frame and
     keep the previous probe's pixels; a frame is a duplicate when the probe
     bytes match within a small per-pixel tolerance. Rationale: a repeated
     video frame yields a byte-identical compositor buffer, and the GPU
     downscale is deterministic — false "unique" is impossible, and 4096
     sample points make false "duplicate" unlikely for real motion.
     Readback cost: 16 KiB/frame, negligible next to the existing full-frame
     copy + fence wait.
   - Luma probe (DRM test) reads from the same 64×64 buffer — behavior
     preserved, just more samples.
   - `Capture` feeds `(t_cap, is_dup)` into a mutex-guarded `CadenceTracker`
     and exposes a `cadence()` snapshot.
   - All frames (duplicates included) are still published to the pool —
     passthrough behavior is unchanged. Withholding duplicates and the
     one-frame re-timing buffer belong to Milestone 3, where the consumer
     actually exists.

3. **Wire cadence stats into the main loop**
   - Extend the per-second stats line: `source ~23.98 fps (3:2, 60% dup)`.
   - README: mark Milestone 2 implemented, document the mechanism.

4. **(If push permissions allow) `.github/workflows/ci.yml`**
   - ubuntu-latest, `cmake -DLSFG_BUILD_APP=OFF -DBUILD_TESTING=ON`,
     build + `ctest` — runs the pure-logic tests on every push/PR without
     needing SDL3/GPU. App builds stay validated locally until a
     runner-compatible dep set exists.

### Explicitly out of scope for this pass

- Milestone 3 (LSFG shader integration, actual interpolation).
- Withholding duplicate frames from the pool / re-timing (Milestone 3).
- The Milestone 0 hardware question — whether Crunchyroll-in-Firefox capture
  is black — still needs a manual `--drm-test` run on a real desktop; no
  code change here can answer it.

### Risks

- Damage-driven compositors deliver no duplicates; the tracker treats every
  frame as unique and recovers cadence from arrival times — covered by a
  dedicated test.
- Overlays (cursor embedded in the stream, subtitles) make some "repeat"
  frames genuinely differ; they'll read as unique. That is correct for
  interpolation purposes (the screen content did change), it just loosens
  the pattern lock — the tracker reports "irregular" rather than lying.
