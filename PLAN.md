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
  recovery) plus CI landed and merged as PR #2.
- **2026-07-06 (this pass):** fresh recon recorded below, plus the plan for
  Milestone 3 groundwork: frame pacing, unique-frame history, and a
  placeholder GPU interpolator that makes `-m N` actually generate frames.

## Phase 1 recon — current state (2026-07-06)

### Module map

```
CMakeLists.txt          C++20; lsfg_core static lib (pure logic) + lsfg-cap
                        executable (deps: Vulkan, libpipewire-0.3, libportal,
                        SDL3 via pkg-config). LSFG_BUILD_APP=OFF builds only
                        core + tests; BUILD_TESTING adds the CTest binary.
.github/workflows/ci.yml  ubuntu-latest, core-only build, runs ctest.
src/
  main.cpp        272L  CLI parsing, portal pump, main loop, per-second stats
                        (capture/source-cadence/present/latency/luma), DRM
                        black-frame verdict, restore-token persistence
  options.hpp      20L  Options struct (multiplier parsed, not yet consumed)
  log.hpp          48L  leveled printf-style logging + monotonic nowSeconds()
  portal.{hpp,cpp} 183L PortalSession: ScreenCast handshake via libportal
  capture.{hpp,cpp}947L Capture: PipeWire consumer. DMA-BUF negotiation with
                        modifier fixation, SHM fallback incl. mid-stream
                        renegotiation; copies every frame into the FramePool;
                        64×64 probe every frame → duplicate detection (feeds
                        CadenceTracker) + luminance (DRM test)
  renderer.{hpp,cpp}225L Renderer: blits latest pool frame to the SDL3/Vulkan
                        swapchain, letterboxed; latency EMA
  core/cadence.*   203L CadenceTracker (pure, unit-tested): recovers source
                        fps + repeat pattern ("3:2" etc.) from
                        (timestamp, is_duplicate) events; handles pulldown
                        and damage-driven delivery; pause/seek resets
  vk/context.*     402L instance/device/queue, swapchain, DRM modifier query
  vk/frame_pool.*  202L triple-buffered VkImage pool, publish/acquireRead
                        lease model; single "latest" frame only
  vk/dmabuf_import.*150L VkImage import of PipeWire DMA-BUF planes
tests/
  test_cadence.cpp 184L 7 scenario tests (3:2, 2:2, passthrough, damage-
                        driven, jitter, cadence change, pause) via a small
                        assert macro; no external framework
```

### Build & test baseline (this environment, 2026-07-06)

- Ubuntu 24.04 container, g++ 13.3, cmake + ninja present.
- Core-only (`-DLSFG_BUILD_APP=OFF`): **builds clean, `ctest` 1/1 passing.**
- Full app: `libpipewire-0.3-dev` 1.0.5, `libportal-dev` 0.7.1,
  `libvulkan-dev` 1.3.275 from apt; SDL3 still isn't packaged on 24.04, so
  SDL 3.4.12 was built from the `sdl3-src` crates.io tarball (GitHub/libsdl
  release downloads are blocked by this container's network policy; X11 dev
  headers + `libxtst-dev` needed first) into a scratch prefix. With
  `PKG_CONFIG_PATH` pointing there: **compiles and links clean, zero
  warnings** (`-Wall -Wextra`); `--help` runs.
- Runtime is untestable here (no display/portal/GPU) — unchanged from prior
  passes. The Milestone 0 hardware question (is Crunchyroll-in-Firefox
  capture black?) still needs a manual `--drm-test` run on a real desktop.
- `glslang-tools` (GLSL→SPIR-V compiler) is packaged on Ubuntu 24.04 and
  Fedora — relevant to increment 3 below.

## Phase 2 — Milestone 3 groundwork: pacing + placeholder frame generation

Why this scope: full Milestone 3 means running Lossless Scaling's shipped
shaders, which requires a machine with a GPU and an owned copy of the game's
assets — neither exists here, and integrating a shader chain we cannot
execute once would be stacked, unverifiable work. What *can* be built and
verified now is everything around that shader: the frame pacer (pure logic,
unit-testable), the unique-frame history the interpolator reads, a working
placeholder interpolator (GPU crossfade) behind a backend interface, and the
wiring that makes `-m N` produce real generated output. When the LSFG shader
chain lands, it slots into an already-proven pipeline as a second backend.

### Increments (each: build clean, tests green, commit)

1. **Add frame pacer core module with unit tests**
   - `src/core/pacer.{hpp,cpp}`: `FramePacer`, pure std-only, single-threaded
     by contract (like `CadenceTracker`).
   - Input: `onUniqueFrame(t_seconds, seq)` for each unique captured frame,
     plus multiplier N and the cadence lock flag. Query per display refresh:
     `Decision decide(t_now)` → either *passthrough* (show newest frame) or
     *interpolate* `{seq_prev, seq_next, phase ∈ (0,1)}`.
   - Interpolating toward a frame requires already having it, so the pacer
     schedules output one source interval behind capture; it reports that
     added latency so the stats line can show it honestly.
   - Falls back to passthrough whenever cadence is unlocked, the multiplier
     is 1, or source frames stop arriving (pause) — output must degrade to
     exactly today's behavior, never freeze.
   - `tests/test_pacer.cpp` (same assert-macro style): 24 fps → 60 Hz at
     ×2/×3/×4 (phases monotonic, every pair used, none skipped), unlock →
     passthrough fallback, pause/resume, source-rate change, jitter.

2. **Keep the last two unique frames in the FramePool**
   - Grow the pool from 3 to 4 slots so "previous", "latest", "being read"
     and "being written" can coexist without ever blocking the writer
     (same lease model, same invariant, one more slot).
   - Add `acquirePair()` alongside `acquireRead()` (which is preserved
     unchanged): returns the latest two published frames for interpolation.
   - Capture publishes only *unique* frames once interpolation is active;
     duplicates are still probed (cadence keeps updating) but not published.
     With `-m 1` every frame is published exactly as today.

3. **Add a Vulkan interpolator with a crossfade backend**
   - `src/vk/interpolate.{hpp,cpp}`: owns an intermediate VkImage and a
     compute pipeline; `generate(prev, next, phase)` writes the in-between
     frame; renderer blits it instead of the pool image when the pacer says
     to interpolate. Designed as *the* backend seam: the future LSFG shader
     chain implements the same generate() contract.
   - `shaders/blend.comp` (GLSL): `mix(prev, next, phase)` — a crossfade.
     Visibly soft on motion, but it exercises timing, layout transitions,
     and pacing end-to-end with zero proprietary assets.
   - Compiled at build time via `glslangValidator` (new app-build dependency;
     packaged on Fedora and Ubuntu incl. CI runners; core-only builds don't
     need it). No binary blobs committed.

4. **Wire the multiplier through the main loop**
   - `-m 2..4` activates the pacer + interpolator (default 1 = passthrough,
     bit-exact today's path); stats line gains
     `output 47.9 fps (x2 gen, +42 ms pacing)`.
   - README: milestone table row for "3a — pacing + placeholder
     interpolation"; document the crossfade placeholder and the new
     glslang build dependency.

5. **CI: add an app-build job**
   - Now that the full dep set is proven reproducible on Ubuntu 24.04:
     apt packages + SDL3 built from the official release tarball, cached
     with `actions/cache` keyed on the SDL version (runners have open
     network, unlike this container). Compile/link validation only — no
     GPU runtime on runners. The existing core-tests job stays untouched
     as the fast always-green signal.

### Explicitly out of scope for this pass

- The real LSFG shader chain (`Lossless.dll` extraction à la lsfg-vk) —
  needs owned Steam assets and a GPU to validate; lands as a second
  interpolator backend in a later pass.
- Config file (`~/.config/lsfg-cap/config.toml`) — roadmap, not blocking.
- Runtime/visual validation — impossible in this container; increments are
  verified by unit tests + clean builds, and the crossfade path needs a
  desktop run (same caveat as Milestones 0–2).

### Risks

- **Pool growth (3→4 slots) touches the concurrency invariant.** Mitigated:
  the lease model is unchanged, the non-blocking-writer argument is the same
  counting argument with one more slot, and `acquireRead()` behavior is
  preserved for the passthrough path.
- **Crossfade quality is poor on motion** — that's expected of a placeholder;
  it's off by default (`-m 1`) and clearly labeled in README. The point is
  proving pacing/latency plumbing, not visual quality.
- **Interpolation adds one source interval of latency** (~42 ms at 24 fps),
  on top of the existing pipeline delay; lipsync target (< 50 ms total) may
  not hold while interpolating. Surfaced honestly in the stats line;
  acceptable for a placeholder, revisit when the real backend lands.
- **glslangValidator as a new build dep** could be missing on some distro;
  it's checked at configure time with a clear error, and core builds/CI
  tests never need it.
