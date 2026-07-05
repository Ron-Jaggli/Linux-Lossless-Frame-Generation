# PLAN — Linux-Lossless-Frame-Generation (lsfg-cap)

## History

- **2026-07-05 (morning):** Phase 1 recon of the then-empty repository; the
  bootstrap plan was reviewed and merged as PR #1.
- **2026-07-05 (midday):** pivot to the concrete tool (see README);
  Milestones 0/1 (portal/PipeWire capture → Vulkan passthrough window)
  implemented.
- **2026-07-05 (afternoon):** Milestone 2 (duplicate detection + cadence
  recovery) with unit tests and a core-tests CI workflow; merged as PR #2.
- **2026-07-05 (evening, this pass):** fresh recon recorded below; plan for
  Milestone 3 groundwork — frame pacing + an interpolation pipeline with a
  blend fallback, so the LSFG shader chain later drops into a proven slot.

## Phase 1 recon — current state (2026-07-05, evening)

### Module map

```
CMakeLists.txt            C++20; lsfg_core static lib (pure logic) + lsfg-cap
                          executable (LSFG_BUILD_APP, default ON) + CTest
.github/workflows/ci.yml  ubuntu-latest, LSFG_BUILD_APP=OFF, builds core, runs ctest
src/
  main.cpp          272L  CLI parsing, portal pump, main loop, per-second stats
                          (capture/source-cadence/present/latency/luma), DRM verdict
  options.hpp        20L  Options struct shared across modules
  log.hpp            48L  leveled printf-style logging + monotonic nowSeconds()
  portal.{hpp,cpp}  183L  PortalSession: ScreenCast handshake via libportal
                          (picker, restore token, close signal)
  capture.{hpp,cpp} 947L  Capture: PipeWire consumer; DMA-BUF w/ modifier
                          fixation, SHM fallback + mid-stream renegotiation;
                          copies frames into FramePool; 64×64 probe every frame
                          → duplicate compare → CadenceTracker; luma for DRM test
  renderer.{hpp,cpp}225L  Renderer: blits latest pool frame to the SDL3/Vulkan
                          swapchain, letterboxed; latency EMA
  core/cadence.*    203L  CadenceTracker (pure, unit-tested): (t, is_dup) events
                          → source fps, pattern (3:2/2:2/1:1/irregular), lock
  vk/context.*      402L  instance/device/queue, swapchain, DRM modifier query
  vk/frame_pool.*   202L  triple-buffered VkImage pool, publish/acquireRead
                          lease model decoupling capture (writer) from render
                          (reader)
  vk/dmabuf_import.*150L  VkImage import of PipeWire DMA-BUF planes
tests/
  test_cadence.cpp  184L  CTest-driven, framework-free; pulldown/2:2/passthrough/
                          damage-driven/jitter/cadence-change/pause cases
```

### Build & test baseline (this environment)

- Ubuntu 24.04 container. From apt: `libpipewire-0.3-dev` 1.0.5,
  `libportal-dev` 0.7.1, `libvulkan-dev` 1.3.275. SDL3 is still not packaged
  on 24.04: built SDL 3.4.12 from source (crates.io `sdl3-src` tarball —
  GitHub is blocked here) into a scratch prefix, `PKG_CONFIG_PATH` pointed
  at it. X11/Wayland dev headers from apt for the SDL build.
- Full app (`cmake -B build -G Ninja && cmake --build build`): **compiles
  and links clean, zero warnings** (`-Wall -Wextra`). `./build/lsfg-cap
  --help` runs.
- Core-only (`-DLSFG_BUILD_APP=OFF`, what CI runs): builds clean;
  `ctest` → **1/1 passed** (cadence suite).
- Nothing is broken. Runtime capture/present paths remain untestable in this
  container (no display, portal, or GPU) — unchanged since milestone 1.

## Phase 2 — Milestone 3 groundwork: frame pacing + interpolation pipeline

Why this slice and not the LSFG shaders directly: the shader chain needs
`Lossless.dll` from the user's Steam library, so it can never be verified in
this container or in CI. Everything *around* it can be: a pure, unit-tested
pacer that decides what to present when; a unique-frame-pair history in the
pool; an `Interpolator` interface with a trivial GPU linear-blend
implementation that exercises the full generate-and-present path end to end.
Once that pipeline demonstrably works on real hardware, the LSFG chain
(milestone 3b) replaces the blend implementation behind the same interface.

### Increments (each: build clean, tests green, commit)

1. **Add frame pacer core with unit tests**
   - `src/core/pacer.{hpp,cpp}`: `FramePacer`, pure std-only, single-threaded
     by contract (same conventions as `CadenceTracker`).
   - Input: multiplier, unique-frame events `(t, seq)`, current
     `CadenceStats`. Output: a present schedule — for each output slot,
     `(base seq, blend alpha, target time)`. 2x on a locked 24-in-60 3:2
     source means: present frame N, then gen(N→N+1, α=0.5), each for half the
     source interval.
   - Interpolation requires holding frame N until N+1 arrives: one source
     frame of added latency, only when multiplier > 1. Passthrough schedule
     (α=0, no delay) when multiplier == 1 or cadence is not locked; gap
     > 250 ms (pause/seek) flushes to passthrough until re-locked.
   - `tests/test_pacer.cpp` (same framework-free CTest style): 2x/3x/4x over
     3:2 pulldown, 2x over clean 30 fps, unlock → passthrough fallback,
     pause/resume, mid-stream cadence change, jittered timestamps.

2. **Expose unique-frame pairs from the FramePool**
   - Capture already knows duplicates; pass `is_duplicate` through
     `publish()`. Pool tracks the latest two *unique* slots and gains
     `acquirePairRead()` (prev + curr leases); grow 3 → 5 slots so the
     writer stays never-blocking with two slots pinned by the reader.
   - Existing single-frame API (`acquireRead`) unchanged — passthrough path
     untouched. Compile + existing tests; pool logic that is pure (slot
     selection) stays small enough to verify by inspection.

3. **Add Interpolator interface + linear-blend implementation**
   - `src/vk/interpolate.{hpp,cpp}`: `record(cmd, prev, curr, alpha, dst)`
     appends commands producing the generated frame. First implementation: a
     small compute shader `mix(prev, curr, alpha)`.
   - SPIR-V: compiled offline with glslangValidator and committed as a
     uint32 array header next to its GLSL source (regeneration one-liner in
     a comment). No build-time shader-compiler dependency, matching the
     no-new-deps stance of CI.

4. **Wire pacer + interpolator into the renderer**
   - When multiplier > 1 and the pacer emits a generate slot, the renderer
     blends into an intermediate image and blits that to the swapchain;
     otherwise the existing blit path runs unchanged.
   - Stats line gains output rate and mode: `output 59.9 fps (2x gen)` vs
     `(passthrough)`.
   - README: milestone 3 groundwork status, one paragraph on the blend
     fallback and the added source-frame latency.

5. **CI: add a full-app build job**
   - Now that the dep recipe is proven on Ubuntu 24.04: apt packages + SDL
     3.4.12 from source, cached via `actions/cache` keyed on the SDL
     version. Compile-only (no display on runners); the core-tests job stays
     as is. Keeps app-breaking changes from landing silently.

### Explicitly out of scope for this pass

- The actual LSFG shader chain / `Lossless.dll` loading (milestone 3b) —
  needs the DLL and a GPU to verify; scaffolding-only work on it would be
  untestable guesswork.
- 3x/4x cadence-locked refinement and present-timing polish (milestone 4).
- Config file (`config.toml`), GUI.
- The Milestone 0 hardware question (is Crunchyroll-in-Firefox capture
  black?) still needs a manual `--drm-test` run on a real desktop.

### Risks

- **No GPU here:** the blend interpolator and renderer wiring can only be
  compile-checked in this container; a manual run on real hardware is the
  acceptance test, exactly as it was for milestones 1–2. The pacer — the
  logic most likely to be subtly wrong — is pure and fully unit-tested.
- **Present-mode quantization:** FIFO vsync quantizes presents to the
  refresh; the pacer schedules against target times and the renderer
  presents the slot whose target has passed, so small drift self-corrects.
  If that proves too coarse on hardware, `VK_KHR_present_wait`/mailbox is
  the milestone 4 follow-up.
- **Slot pinning:** holding two read leases while the writer keeps
  publishing risks writer starvation if sized wrong; 5 slots leaves 3 free
  (write target + latest + spare), preserving the never-blocking writer
  invariant.
