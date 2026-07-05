# PLAN — lsfg-cap edit pass (Phase 1 recon, 2026-07-05)

Supersedes the previous PLAN.md, which described bootstrapping an empty
repository. That bootstrap happened (PR #1 + commit `cc53e53`); this plan is a
recon of the milestone-0/1 codebase that now exists and the edit pass proposed
on top of it.

## Repository map

| Path | What it does |
|---|---|
| `src/main.cpp` | CLI parsing (getopt_long), portal restore-token persistence, main loop: SDL event pump, portal pump, per-second stats, DRM black-frame verdict + exit codes |
| `src/options.hpp` | `Options` struct shared by CLI and subsystems |
| `src/log.hpp` | Header-only leveled printf logging + `nowSeconds()` monotonic clock |
| `src/portal.{hpp,cpp}` | xdg-desktop-portal ScreenCast handshake via libportal: session → window picker → start → `OpenPipeWireRemote`; restore-token support; session-closed callback |
| `src/capture.{hpp,cpp}` | PipeWire stream consumer on a `pw_thread_loop`: format negotiation with DRM modifiers (offer → fixate), DMA-BUF import path + SHM staging path, mid-stream SHM fallback, copy into `FramePool`, 16×16 luminance probe (every 30th frame, every frame under `--drm-test`) |
| `src/renderer.{hpp,cpp}` | Blits latest pool frame to the swapchain, letterboxed; EMA end-to-end latency; swapchain-recreate handling |
| `src/vk/context.{hpp,cpp}` | SDL3 window, Vulkan instance/device/single shared graphics queue (+`queue_mutex`), swapchain, DRM-modifier enumeration with importability checks |
| `src/vk/frame_pool.{hpp,cpp}` | Triple-buffered `VkImage` pool decoupling capture (writer) from render (reader); recreate-on-resize blocks until the read lease is released |
| `src/vk/dmabuf_import.{hpp,cpp}` | DMA-BUF fd → `VkImage` import with explicit modifier plane layouts |

No tests, no `tests/` directory, no CI (`.github/workflows/` absent).
Build system: CMake ≥ 3.20 + pkg-config; deps: Vulkan, libpipewire-0.3,
libportal, SDL3.

## Build & test baseline (this container: Ubuntu 24.04)

- `libpipewire-0.3-dev` (1.0.5), `libportal-dev` (0.7.1), `libvulkan-dev`
  installed from the Ubuntu archive. SDL3 is not packaged on 24.04; built
  SDL 3.2.8 from source (tarball from the Ubuntu 25.x pool — github.com and
  libsdl.org are blocked by this session's network policy).
- **Build: clean.** `cmake -G Ninja` + `ninja` compile all 8 objects and link
  with zero warnings under `-Wall -Wextra`.
- **Smoke test:** `./build/lsfg-cap --help` works (exit 0). A real run exits
  immediately with `SDL_Init failed: No available video device` — expected:
  the tool needs a display, a GPU, and a desktop portal, none of which exist
  in this container. Functional testing of capture/present is therefore
  impossible here and stays manual (on the owner's machine).
- **Tests: none exist**, so nothing passes or fails. Nothing is broken.

## Findings from code reading (small defects worth fixing)

1. `main()` returns 0 when argument parsing fails or an unknown flag is
   passed (`parseArgs` → `printUsage` → `return false` → `main` returns 0).
   Scripts can't distinguish "ran and succeeded" from "bad invocation".
2. `-m/--multiplier` is `atoi`'d and never validated; `-m 0`, `-m -3`, or
   `-m banana` (= 0) are accepted silently. Should clamp/reject outside 2–4.
3. `Capture::processShm` copies `stride * height` bytes from the mapped
   chunk without checking `chunk->size`, trusting the producer; a short
   chunk would over-read. Cheap bounds check available.
4. Early-error returns in `run()` (portal failure, `--drm-test` verdict
   paths) skip `capture.stop()` / `ctx.shutdown()`. Harmless at process
   exit, but the DRM-test path returns while the PipeWire thread is live —
   tear down explicitly instead.

## Proposed edit pass

### A. Small correctness fixes (finding 1–4 above)
One commit, e.g. "Fix CLI exit codes and validate multiplier",
"Bound SHM copy by chunk size". Behavior-preserving otherwise.

### B. Milestone 2 — duplicate detection + cadence recovery (README roadmap)

The point: a 60 Hz browser repaints 23.976 fps video in a 3:2 pulldown;
interpolation (milestone 3) must operate on *unique source frames* and their
true cadence, not on the repaint rate.

1. **Per-frame fingerprint** (`src/capture.*`): run the existing 16×16
   downscale probe on *every* frame (it already exists and is tiny — 1 KiB
   readback, capture thread already fence-waits), and derive from it a
   64-bit hash + mean luma. Exact hash match against the previous frame
   marks a duplicate; a sum-of-abs-diff threshold marks near-duplicates.
   Expose duplicate/unique counters; duplicates are counted but still
   published (passthrough behavior unchanged).
2. **Cadence detector** (`src/cadence.hpp`, new, pure logic, no Vulkan/
   PipeWire): consumes `(timestamp, is_duplicate)` events, estimates the
   unique-frame interval (EMA + interval histogram), matches known cadences
   (23.976/24/25/29.97/30/50/59.94/60), reports stability and repaint
   pattern (e.g. 3:2). Header-only so it is unit-testable headless.
   Note PipeWire screencast is damage-driven, so some compositors already
   deliver only unique frames — the detector uses arrival times alone in
   that case and fingerprint-duplicates when repeats do arrive.
3. **Wire into the stats line** (`src/main.cpp`): log
   `source ~23.98 fps (3:2 of 60)` next to the existing capture/present
   stats. No behavioral change to rendering yet — cadence-locked frame
   pacing belongs to milestone 3/4.

### C. Tests + CI

1. `tests/` with a plain-assert test executable for the cadence detector and
   fingerprint comparison (synthetic 3:2 / 2:2 / irregular timestamp
   sequences). No test-framework dependency — FetchContent from GitHub is
   blocked in this environment, and plain `ctest` executables keep the build
   dependency-free. Guarded by `BUILD_TESTING` (default on, standard CTest).
2. `.github/workflows/ci.yml`: ubuntu-latest, apt deps + SDL3 built from
   source (cached by version key), `cmake --build`, `ctest`. CI proves
   compile + pure-logic tests; GPU/portal paths remain manual.

### Public interfaces
All changes are additive (new accessors on `Capture`, new header, new stats
line). No existing signature changes; CLI flags unchanged; `--drm-test` exit
codes unchanged.

### Increment order (each: build clean → tests green → commit)
1. Fix CLI exit codes and validate multiplier (A1, A2)
2. Bound SHM copy and tear down cleanly on early exits (A3, A4)
3. Add cadence detector header with unit tests + CTest scaffolding (B2, C1)
4. Add per-frame fingerprint and duplicate counters to capture (B1)
5. Wire cadence stats into the main loop (B3)
6. Add GitHub Actions CI workflow (C2)

### Explicitly out of scope for this pass
- Milestone 3 (LSFG shader integration, actual interpolation)
- Cadence-locked presentation / frame pacing (milestone 4)
- Config file, GUI
- Any restyling of existing code

## Open questions for review
1. Should duplicates be *dropped* from the pool already in this pass (one
   frame of added latency, per README's one-frame buffer design), or only
   *counted* until milestone 3 consumes the signal? Plan assumes count-only.
2. Is a plain-assert test executable acceptable, or do you want Catch2
   vendored into the repo despite the extra ~0.5 MB?
