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
- **2026-07-05 → 07-06 (overnight):** the scheduled edit-pass routine ran
  repeatedly; each run pushed a Phase-1 PLAN.md to its own
  `claude/wizardly-clarke-*` branch and stopped for review that never came.
  Eight sibling plan branches now sit unmerged; the later ones
  (`ry0fv0`, `zixbd5`, `l96kov`, `ay7ygk`) all converge on the same
  milestone-3 groundwork scope.
- **2026-07-06 (this pass):** fresh recon (recorded below) and a single
  **consolidated** milestone-3a plan superseding those sibling branches.
  Once one plan is approved, the other plan-only branches can be deleted.

## Phase 1 recon — current state (2026-07-06, post-milestone-2)

### Module map

```
CMakeLists.txt          C++20; lsfg_core static lib (pure logic, no system
                        deps) + lsfg-cap app (Vulkan, libpipewire-0.3,
                        libportal, SDL3) + CTest. LSFG_BUILD_APP=OFF builds
                        just core+tests (what CI runs).
.github/workflows/ci.yml  ubuntu-latest, core-only build, ctest.
src/
  main.cpp        272L  CLI parsing, portal pump, main loop, per-second
                        stats line (capture/source/present fps, latency,
                        luma), DRM black-frame verdict (--drm-test)
  options.hpp      20L  Options struct shared across modules
  log.hpp          48L  leveled logging + monotonic nowSeconds()
  portal.{hpp,cpp} 183L PortalSession: ScreenCast handshake via libportal
                        (picker dialog, restore token, close signal)
  capture.{hpp,cpp}947L Capture: PipeWire consumer on pw_thread_loop;
                        DMA-BUF with modifier fixation, SHM fallback incl.
                        mid-stream renegotiation; copies every frame into
                        the pool; 64×64 probe on every frame → duplicate
                        compare (1-LSB/byte tolerance) feeds CadenceTracker;
                        probe mean luminance drives the DRM test.
                        Note: the probe is read back *after* publish(), so
                        published frames do not carry a duplicate flag yet.
  renderer.{hpp,cpp}225L Renderer: letterboxed blit of the latest pool frame
                        to the SDL3/Vulkan swapchain; latency EMA. The
                        render path is transfer-only — no shader pipeline
                        exists anywhere in the repo yet.
  core/cadence.*   203L CadenceTracker (pure, unit-tested): source fps,
                        repeat pattern ("3:2"/"2:2"/"1:1"/"irregular"),
                        dup ratio, lock flag; 0.5 s gap resets the window.
  vk/context.*     402L instance/device/queue/swapchain, DRM modifier
                        query, shared graphics queue guarded by a mutex
  vk/frame_pool.*  202L triple-buffered VkImage pool, capture=writer /
                        render=reader, publish/acquireRead lease model;
                        published images live in TRANSFER_SRC_OPTIMAL
  vk/dmabuf_import.*150L VkImage import of PipeWire DMA-BUF planes
tests/test_cadence.cpp 184L  7 scenario tests driving CadenceTracker
```

### Build & test baseline (this environment, fresh container)

- Ubuntu 24.04. `libpipewire-0.3-dev` 1.0.5, `libportal-dev` 0.7.1,
  `libvulkan-dev` 1.3.275 from apt (two dead PPA entries had to be removed
  before `apt update` would run). SDL3 is still not packaged on 24.04:
  built SDL 3.4.12 from the `sdl3-src` crates.io tarball (GitHub/libsdl.org
  blocked by network policy; crates.io needs a User-Agent) as a
  console-only build (`-DSDL_X11=OFF -DSDL_WAYLAND=OFF
  -DSDL_UNIX_CONSOLE_BUILD=ON`) into a scratch prefix on PKG_CONFIG_PATH.
- Full app: `cmake -B build -G Ninja && cmake --build build` → **compiles
  and links clean, zero warnings** (`-Wall -Wextra`); `--help` runs.
- Core: `-DLSFG_BUILD_APP=OFF` → builds; `ctest` → **1/1 passed** (cadence).
- Not runtime-testable here: anything needing a display/portal/GPU. The
  Milestone 0 hardware question (is Crunchyroll-in-Firefox capture black?)
  still needs a manual `--drm-test` run on a real desktop.
- `glslang-tools` (GLSL→SPIR-V) is packaged on Ubuntu 24.04 (15.1.0) and
  Fedora — relevant to increment 3 below.

## Phase 2 — Milestone 3a: pacing + placeholder frame generation

This consolidates the plans on branches `ry0fv0`/`zixbd5`/`l96kov`/`ay7ygk`.

Why "3a": full Milestone 3 runs Lossless Scaling's shipped shaders, which
needs a machine with a GPU and a user-owned `Lossless.dll` (plus lsfg-vk's
sources as reference — GitHub is blocked here). None of that exists in this
environment, and integrating a shader chain that can never be executed here
would be stacked, unverifiable work. What *can* be built and verified now is
everything that chain plugs into — which is also where latent design
mistakes would be most expensive later:

- **frame pacing** — when to present which real/generated frame (pure
  logic, unit-testable),
- **duplicate withholding** — interpolate unique frames, not repaints,
- **a two-frame history** the interpolator reads from,
- **an `Interpolator` seam** with a trivial GPU crossfade backend proving
  the path end-to-end with zero proprietary assets.

**3b** (the real LSFG shader chain, per lsfg-vk's approach) lands as a
second backend in a later pass, on hardware.

### Increments (each: build clean, tests green, commit)

1. **Add frame pacer core with unit tests**
   - `src/core/pacer.{hpp,cpp}`: `FramePacer`, pure std-only, same contract
     as `CadenceTracker` (caller serializes; no system deps).
   - Inputs: `setMultiplier(m)`, `onUniqueFrame(seq, t)`, periodic
     `onCadence(fps, locked)`. Query per render tick:
     `decide(t_now)` → *passthrough* (newest frame, today's behavior) or
     *interpolate* `{seq_prev, seq_next, phase ∈ (0,1)}`.
   - Interpolating toward a frame requires already having it, so paced
     output runs one source interval behind arrival; the pacer reports that
     added latency so the stats line can show it honestly.
   - Falls back to passthrough whenever cadence is unlocked, m == 1, or
     frames stop arriving (> 0.5 s gap, matching the tracker) — output
     degrades to exactly today's behavior, never freezes.
   - `tests/test_pacer.cpp` (same assert-macro style as `test_cadence`):
     24 fps → 60 Hz at ×2/×3/×4 (phases monotonic, every pair used, none
     skipped), unlock fallback, m=1 passthrough, pause/resume snap,
     arrival jitter, mid-stream cadence change.

2. **Attach the duplicate flag + unique sequence to published frames**
   - In `Capture::handleProcess`, read the probe *before* `publish()` (the
     copy and probe are both fence-waited by that point — a pure reorder,
     no new synchronization) and pass `duplicate` and a monotonically
     increasing `unique_seq` through `publish()`/`ReadLease`.
   - Capture feeds `pacer.onUniqueFrame` under the existing cadence mutex.
   - All frames are still published; no behavior change for the renderer.

3. **Two-frame history + `Interpolator` seam + crossfade backend**
   - `src/vk/frame_history.*`: renderer-side prev/cur pair of images. Each
     render tick copies a newly published *unique* frame in (same proven
     barrier/fence/queue-mutex style as existing paths); duplicates are
     consumed but not copied — withholding happens on the consumer side,
     so the pool and capture thread keep their current simple invariant
     (no slot-count change).
   - `src/vk/interpolate.hpp`: `class Interpolator { generate(prev, next,
     phase, dst); }` — the seam 3b's LSFG chain implements later.
   - `src/vk/interp_blend.*` + `shaders/blend.comp` (GLSL, checked in):
     compute `mix(prev, next, phase)` into an `R8G8B8A8_UNORM` STORAGE
     image (the one format with guaranteed storage support; the existing
     blit handles conversion to the swapchain). Compiled at build time via
     `glslangValidator` — new app-build dependency, checked at configure
     time with a clear error; packaged on Ubuntu and Fedora; core-only
     builds and CI never need it. No binary blobs committed.

4. **Wire the multiplier: paced presentation**
   - Renderer consults the pacer each tick: phase 0 → blit the real frame
     (exactly today's path); otherwise generate into the intermediate and
     blit that. `-m 2..4` becomes functional end-to-end (crossfade
     placeholder); `-m 1` (default) keeps the current path bit-exact;
     multiplier is clamped to 1..4 at parse time.
   - Stats line grows: `out 60.0 fps (x2 of 23.98, blend, +42 ms pacing)`.
   - README: milestone table splits 3 into 3a (done, placeholder) / 3b
     (LSFG chain, needs owned Lossless.dll + hardware); document the
     crossfade placeholder and the glslang build dependency.

5. **CI: add an app-build job (compile/link validation only)**
   - The full dep set is now proven reproducible on Ubuntu 24.04: apt
     packages + glslang-tools + SDL3 built from a release tarball, cached
     with `actions/cache` keyed on the SDL version (runners have open
     network, unlike this container). The existing core-tests job stays
     untouched as the fast always-green signal.

### Explicitly out of scope for this pass

- **3b:** loading/translating Lossless.dll shaders (lsfg-vk approach) —
  needs owned Steam assets, reference sources, and a GPU.
- Milestone 4 (3x/4x productization, polish) — the pacer and seam are
  m-agnostic, but only 2x is claimed as supported in docs.
- Config file (`~/.config/lsfg-cap/config.toml`) — roadmap, not blocking.
- The Milestone 0 hardware question — still needs a manual `--drm-test`
  run on a real desktop; no code change here can answer it.

### Risks

- **No GPU/display here:** increments 3–4 are compile-clean but not
  runtime-verified until a hardware run. Mitigation: all decision logic
  (pacing, withholding) lives in the unit-tested core; the Vulkan additions
  reuse the exact barrier/fence/queue-mutex patterns already proven in
  capture/renderer; first hardware run checklist: validation layers on,
  `-m 1` regression first, then `-m 2`.
- **Crossfade ghosting:** inherent to the placeholder — it demonstrates
  pacing correctness (motion cadence smooths) while looking soft. Labelled
  as "blend" in `--help`, the stats line, and README so nobody mistakes it
  for LSFG.
- **Added latency:** +1 source interval (~42 ms at 24 fps) is fundamental
  to interpolation and lands on top of the existing pipeline delay; the
  < 50 ms lipsync target may not hold while interpolating. Surfaced
  honestly in the stats line; revisit when the real backend lands.
- **glslangValidator as a new app-build dep:** checked at configure time
  with a clear error; core builds and the existing CI job never need it.
