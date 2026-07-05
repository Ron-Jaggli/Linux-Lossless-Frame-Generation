# PLAN — lsfg-cap edit pass

## Phase 1: Recon baseline (2026-07-05, second pass)

The previous PLAN.md described an empty repository. That is no longer true:
commit `cc53e53` landed the Milestone 0/1 implementation (PipeWire portal
capture → Vulkan passthrough window, ~2 400 lines of C++20). This plan
replaces the old one and covers the next edit pass.

### Module map (all under `src/`)

| Module | Purpose |
|---|---|
| `main.cpp` | CLI parsing (getopt), portal/restore-token handling, main loop: pump SDL events + portal, draw, 1 Hz stats line, DRM black-frame verdict logic (exit codes 0/2/3) |
| `options.hpp` | `Options` struct — plain aggregate of all CLI flags |
| `log.hpp` | header-only leveled logger (`logDebug…logError`) + `nowSeconds()` (CLOCK_MONOTONIC) |
| `portal.{hpp,cpp}` | `PortalSession`: libportal ScreenCast handshake (CreateSession → picker → Start → OpenPipeWireRemote), restore-token support, async via GLib main-context pumping |
| `capture.{hpp,cpp}` | `Capture`: PipeWire stream consumer on a `pw_thread_loop`. Negotiates DMA-BUF with explicit DRM modifiers (incl. fixation) with SHM fallback and mid-stream renegotiation; copies every frame into the `FramePool` (GPU blit or staging upload) and fence-waits before requeueing; 16×16 downscale luminance probe (every 30th frame, or every frame under `--drm-test`) |
| `renderer.{hpp,cpp}` | `Renderer`: blits latest pool frame to the swapchain, letterboxed; tracks presented-frame count and EMA end-to-end latency |
| `vk/context.{hpp,cpp}` | `vk::Context`: SDL3 window, instance/device/queue (single graphics queue + mutex), swapchain (re)creation, device selection, DRM modifier enumeration with import-capability filtering |
| `vk/frame_pool.{hpp,cpp}` | `vk::FramePool`: triple-buffered VkImage pool decoupling capture (writer) from render (reader); non-blocking writer, latest-wins reader, blocking `recreate()` on source resize |
| `vk/dmabuf_import.{hpp,cpp}` | `importDmaBuf()`: VkImage import of a PipeWire DMA-BUF with explicit modifier, dedicated allocation, fd-dup ownership handling |

Threading model: capture thread (PipeWire loop) writes to the pool and
fence-waits each copy; main thread renders and presents. All queue submits
hold `Context::queue_mutex`.

### Build & test baseline

- **Build:** `cmake -B build -G Ninja && cmake --build build` → clean,
  **zero warnings** with `-Wall -Wextra`. `./build/lsfg-cap --help` runs
  (exit 0).
- **Tests:** none exist (`ctest`: "No tests were found").
- **CI:** none (no `.github/workflows/`).
- **Environment notes (this container, Ubuntu 24.04):** `libpipewire-0.3-dev`
  1.0.5, `libportal-dev` 0.7.1, `libvulkan-dev` 1.3.275 from apt. SDL3 is not
  packaged in 24.04 — built and installed **SDL 3.2.4 from source** (git
  clone; the network policy blocks raw tarball downloads but allows git).
  No display/GPU/portal in the container, so runtime behavior cannot be
  exercised here — only compile, link, and `--help`. Graphical validation
  requires the owner's machine.

### Defects found during code reading

1. **`renderer.cpp:69` — `VK_SUBOPTIMAL_KHR` mishandled on acquire.**
   `vkAcquireNextImageKHR` returning `VK_SUBOPTIMAL_KHR` *does* acquire an
   image and *does* signal `sem_acquire_`. The code returns early without
   submitting/presenting, leaving the semaphore signaled and the image
   unreleased; the next acquire reuses a signaled binary semaphore
   (validation error, possible driver hang). Fix: on `SUBOPTIMAL`, proceed
   with the frame and set `needs_recreate_` afterwards; only the
   `OUT_OF_DATE` path may return early.
2. **`capture.cpp:470` (`processShm`) — no bounds check against the chunk.**
   Copies `stride * height` bytes from `data + offset` without checking
   `d->chunk->size`/`d->maxsize`; a compositor sending a short chunk causes
   a read overrun. Fix: clamp the copy to the chunk size and drop the frame
   if it is too small.
3. **`capture.cpp:232` (`handleFormatChanged`) — unchecked modifier choice
   pod.** `n_vals == 0` or a non-choice pod would deref `vals[0]`. Fix:
   validate before use.

None of these can regress the container baseline (they are runtime paths),
but 1 is a real correctness bug on any resize on some drivers.

## Phase 2 scope (proposed increments, in order)

Each increment: build clean → tests green → single imperative commit.
Public interfaces (class APIs above, CLI flags, exit codes) are preserved;
additions only.

1. **Add test scaffold and CI** — `LSFG_BUILD_TESTS` CMake option (default
   OFF) fetching Catch2 v3 via FetchContent(GIT_REPOSITORY); empty-but-green
   suite wired to `ctest`. GitHub Actions workflow (`ubuntu-24.04`): apt
   deps, cached SDL3-from-source build, configure, build, run tests.
   Commit: "Add Catch2 test scaffold and CI workflow".
2. **Fix the three recon defects** — one commit each ("Fix suboptimal
   swapchain acquire path", "Bound SHM copy to chunk size", "Validate
   modifier choice pod before fixation"). No interface changes.
3. **Add cadence detector (Milestone 2 core)** — new `src/core/cadence.{hpp,cpp}`:
   pure, GPU-free `CadenceDetector` consuming `(timestamp, is_duplicate)`
   events; estimates unique-frame rate, detects repeat patterns (e.g. 3:2
   pulldown of 23.976 fps in a 60 Hz repaint stream), exposes source fps +
   confidence. Unit-tested against synthetic 24/25/30-in-60 sequences with
   jitter. This is the roadmap's testable heart and lands before any GPU
   wiring. Commit: "Add cadence detector".
4. **Add duplicate detection in capture** — extend the existing 16×16 probe
   into a per-frame comparison (probe readback each frame; compare against
   previous probe to flag duplicates — cheap: 1 KiB/frame). Feed
   `CadenceDetector`; report `source fps` and duplicate ratio in the 1 Hz
   stats line. `Capture` gains new const accessors only. Commit: "Detect
   duplicate frames and report source cadence".
5. **Update README** — mark Milestone 2 progress, document test/CI usage.
   Commit: "Document cadence detection and test workflow".

Deferred (out of scope for this pass): milestone 3 shader integration,
config file, GPU-hash duplicate detection (probe compare is sufficient
until interpolation exists), frame pacing/re-timing of unique frames.

## Open questions for review

1. Is the Milestone 2 scope (increments 3–4) the right target for this
   pass, or should the pass stop at tests + CI + bug fixes (1–2)?
2. Probe-based duplicate detection samples a 16×16 downscale — adequate for
   video content, but subtitles-only changes may alias. Acceptable for now,
   or require the full-resolution GPU hash immediately?
3. CI: is `ubuntu-24.04` + SDL3-from-source acceptable (~2 min cached), or
   would you rather CI skip SDL/Vulkan entirely and build only the future
   `src/core/` + tests until packages catch up?
