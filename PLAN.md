# PLAN — lsfg-cap edit pass

## Phase 1: Recon baseline (2026-07-05, post-Milestone 0/1)

The previous PLAN.md described an empty repository. That is no longer true:
`main` now contains the Milestone 0/1 implementation (commit `cc53e53`,
"PipeWire portal capture -> Vulkan passthrough window"), so this plan replaces
it with a recon of the actual code and a concrete edit-pass proposal.

### Repository map

```
CMakeLists.txt        single executable target `lsfg-cap`; C++20, -Wall -Wextra
src/
  main.cpp            CLI parsing (getopt_long), restore-token persistence,
                      main loop: portal pump -> renderer -> stats -> DRM verdict
  options.hpp         Options struct shared by all modules
  log.hpp             header-only leveled logging + monotonic clock helper
  portal.{hpp,cpp}    libportal ScreenCast handshake (async, GLib main context)
  capture.{hpp,cpp}   PipeWire stream consumer; DMA-BUF/SHM negotiation with
                      modifier fixation, SHM fallback, triple-buffer pool fill,
                      16x16 luminance probe (DRM black-frame test)
  renderer.{hpp,cpp}  presents latest pool frame to swapchain, letterboxed
  vk/
    context.{hpp,cpp}       instance/device/swapchain, DRM modifier query
    frame_pool.{hpp,cpp}    triple-buffered VkImage pool (capture -> render)
    dmabuf_import.{hpp,cpp} DMA-BUF -> VkImage import with explicit modifiers
```

No tests, no test framework, no CI workflow, no `.github/`.

### Build & test baseline

Environment: Ubuntu 24.04 container, gcc 13.3, CMake 3.28, Ninja.
Dependencies: `libpipewire-0.3-dev`, `libportal-dev`, `libvulkan-dev` from apt;
**SDL3 is not packaged for Ubuntu 24.04** and was built from the Ubuntu 25.x
source tarball (`libsdl3 3.2.20`) — this matters for CI (below).

- `cmake -B build -G Ninja && cmake --build build`: **clean, 0 warnings**.
- `./build/lsfg-cap --help`: works, exit 0.
- Headless run: fails gracefully (`SDL_Init failed: No available video
  device`) — expected without a display; the real pipeline needs a Wayland
  session with xdg-desktop-portal and a GPU, which no CI box has.
- `ctest`: "No tests were found" — zero test coverage.

### Code-review findings (bugs found during recon, ranked)

1. **Acquired-but-unused semaphore on `VK_SUBOPTIMAL_KHR`**
   (`src/renderer.cpp:69-72`). On `SUBOPTIMAL` an image *was* acquired and
   `sem_acquire_` *will* be signaled, but `drawFrame()` returns early without
   waiting on it. The next `vkAcquireNextImageKHR` reuses a still-signaled
   semaphore — validation error and UB on real drivers. Correct handling:
   render/present the acquired image anyway and set `needs_recreate_`.
2. **Early exits leak the PipeWire thread** (`src/main.cpp` `run()`).
   The `--drm-test` verdict paths (`return 0/2/3`) and every error return
   skip `capture.stop()` / `renderer.destroy()` / `ctx.shutdown()`. The
   PipeWire loop thread then races process teardown (callbacks into
   half-destroyed state at exit). Fix: idempotent `stop()` called from a
   `Capture` destructor (same for renderer/pool/context), keeping exit codes.
3. **Unchecked `findMemoryType()` failures** (`src/capture.cpp:686,700,733`,
   `src/vk/frame_pool.cpp:50`). It returns `UINT32_MAX` on failure, which is
   passed straight into `vkAllocateMemory` — invalid API usage on exotic
   memory topologies. Fix: check and fail with a log message.
4. **SHM copy can overread the source buffer** (`src/capture.cpp:478-481`).
   `memcpy` of `stride * height_` bytes ignores `chunk->size`; if the last
   row is unpadded this reads past the mapped buffer. Fix: clamp to
   `chunk->size` when it is set.
5. **Portal PipeWire fd never closed** (`src/portal.cpp`, `src/capture.cpp:78`).
   `Capture::start` dups the fd (PipeWire takes ownership of the dup), but the
   original from `xdp_session_open_pipewire_remote` leaks. Fix: close it in
   `~PortalSession()`.

None of these break the demo on the happy path, which is why Milestone 0/1
appears to work; all are real defects on the paths they cover.

## Phase 2: Proposed increments

Each increment builds clean and keeps `ctest` green before it is committed.
Public interfaces (class APIs, CLI flags, exit codes) are preserved throughout.

1. **Add GitHub Actions CI** (`.github/workflows/ci.yml`): ubuntu-latest,
   apt deps + SDL3 built from source with `actions/cache` (Ubuntu 24.04 has no
   SDL3 package), then configure, build with `-Werror` off (keep -Wall -Wextra
   as-is), run `ctest`. This locks in the "builds clean" baseline before any
   code is touched.
2. **Fix suboptimal-swapchain semaphore reuse** (finding 1): present the
   acquired image, then recreate.
3. **Make teardown safe on early exits** (finding 2): idempotent
   `Capture::stop()` + destructor; same pattern for `Renderer`/`FramePool`/
   `vk::Context`. `main.cpp` exit codes unchanged.
4. **Harden memory and SHM edge cases** (findings 3 + 4): check
   `findMemoryType` results; clamp SHM copy to `chunk->size`.
5. **Close the portal fd** (finding 5).
6. **Milestone 2 groundwork — cadence detector (needs approval, see open
   question 2)**: a pure, headless-testable module (`src/cadence.{hpp,cpp}`)
   that consumes per-frame `(seq, pts)` events already produced by the capture
   thread, detects duplicate-repaint patterns (e.g. 3:2 pulldown of 23.976 fps
   in a 60 Hz browser), and reports recovered source cadence. Unit tests run
   under `ctest` (Catch2 v3 from the system package where available, minimal
   assert harness otherwise), which also gives increment 1's CI something real
   to run. Wiring it into the live pipeline is a later, separate increment.

## Open questions for review

1. Approve fixes 2-5 as scoped above? They are small, behavior-preserving on
   the happy path, and independently committable.
2. Should this edit pass include the Milestone 2 groundwork (increment 6), or
   stop at CI + bug fixes? The detector is self-contained and gives CI real
   tests, but it is new feature surface, not editing existing code.
3. CI budget: building SDL3 from source adds ~2-3 min to a cold CI run
   (cached afterwards). Acceptable, or should CI build only a display-less
   subset until SDL3 lands in the runner image?
