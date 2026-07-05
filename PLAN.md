# PLAN — lsfg-cap edit pass (branch `claude/wizardly-clarke-dkjjbo`)

## Phase 1: Recon baseline (2026-07-05, this branch)

### Repository state

`main` is at `cc53e53` — "Milestone 0/1: PipeWire portal capture -> Vulkan
passthrough window". ~2 400 lines of C++20, built with CMake. No tests, no CI
on `main`.

### Module map

| Module | Purpose |
|---|---|
| `src/main.cpp` | CLI parsing (getopt), portal restore-token persistence, main loop: pump SDL events + portal, draw, 1 s stats line, DRM black-frame verdict logic |
| `src/portal.cpp/.hpp` | `PortalSession` — libportal ScreenCast handshake (CreateSession → picker → Start → OpenPipeWireRemote), async, pumped via GLib main context |
| `src/capture.cpp/.hpp` | `Capture` — PipeWire stream consumer on a `pw_thread_loop`. Negotiates DMA-BUF (explicit DRM modifiers, fixation) with SHM fallback incl. mid-stream renegotiation; copies each frame into the `FramePool`; 16×16 downscale luminance probe (every 30th frame, or every frame under `--drm-test`) |
| `src/renderer.cpp/.hpp` | `Renderer` — blits latest pool frame to the swapchain, letterboxed; EMA end-to-end latency; swapchain recreate on resize/out-of-date |
| `src/vk/context.cpp/.hpp` | `vk::Context` — SDL3 window, instance/device/queue selection, swapchain creation, DRM-modifier enumeration with import-capability verification, shared-queue mutex |
| `src/vk/frame_pool.cpp/.hpp` | `vk::FramePool` — triple-buffered `VkImage` pool decoupling capture (writer) from render (reader); blocking `recreate` on source resize |
| `src/vk/dmabuf_import.cpp/.hpp` | `importDmaBuf` — imports a compositor DMA-BUF as a `VkImage` (explicit modifier, dedicated allocation, fd-dup ownership handling) |
| `src/options.hpp`, `src/log.hpp` | `Options` struct; leveled stderr logger + `nowSeconds()` |

### Build & test baseline (this container: Ubuntu 24.04, GCC 13)

- Deps: `libpipewire-0.3-dev`, `libportal-dev`, `libvulkan-dev` from apt.
  **SDL3 is not packaged in Ubuntu 24.04** — built from source
  (release-3.2.16) and installed to `/usr/local`. The README's Fedora 43
  instructions are unaffected.
- `cmake -G Ninja` + build: **compiles clean, zero warnings** with
  `-Wall -Wextra`.
- `./build/lsfg-cap --help` runs (exit 0). The capture/present paths need a
  Wayland session, xdg-desktop-portal, and a GPU — not exercisable in this
  container; runtime verification stays on the owner's machine.
- Tests: **none exist on `main`**; nothing to run. Baseline = "builds clean,
  zero tests, nothing known broken".

### Branch coordination — read before approving

This scheduled routine has produced several parallel branches; none are
merged or reviewed:

- `claude/wizardly-clarke-4fk1ip` — **furthest along**: implements
  milestone 2 (cadence tracker `src/core/cadence.*` + 184-line unit test,
  duplicate detection via probe compare, cadence in the stats line) plus a
  CI workflow and test scaffolding (+665/−124 over `main`).
- `claude/wizardly-clarke-htdg7b`, `-i8yo5l`, `-ugzdx1` — PLAN.md rewrites
  only (recon passes like this one; no code).

**Recommendation:** review and merge `4fk1ip` for milestone 2 rather than
re-implementing it. To stay complementary, this branch proposes a
*correctness edit pass over the milestone 0/1 core* — fixes that are needed
regardless of which milestone-2 branch lands, touching files `4fk1ip`
mostly doesn't (renderer, dmabuf negotiation details), keeping merge
conflicts minimal.

## Phase 2 proposal: correctness fixes found during recon

Each item is one small, independently buildable commit. Public interfaces
unchanged throughout.

1. **Clamp the SHM copy to the buffer's actual size** —
   `Capture::processShm` (`src/capture.cpp:470`) copies
   `stride × height` bytes trusting the negotiated dimensions; a short or
   mismatched chunk from the compositor causes an out-of-bounds read of the
   mapped SHM buffer. Validate against `spa_data::maxsize`/`chunk->size`
   and drop the frame (with a log) on mismatch.
   *Commit: "Validate SHM chunk size before copying"*

2. **Present acquired images on `VK_SUBOPTIMAL_KHR`** —
   `Renderer::drawFrame` (`src/renderer.cpp:69`) treats `SUBOPTIMAL` like
   `OUT_OF_DATE`: it schedules a recreate and returns without using the
   image. On `SUBOPTIMAL` the image *was* acquired and `sem_acquire_` *was*
   signaled, so the next acquire re-waits a signaled semaphore
   (validation error, potential deadlock on some drivers). Render and
   present the suboptimal frame, then recreate.
   *Commit: "Present suboptimal swapchain images instead of dropping them"*

3. **Fixate only modifiers we verified importable** —
   `Capture::handleFormatChanged` (`src/capture.cpp:239`) defaults to the
   compositor's first offered modifier when none of ours match, guaranteeing
   a failed import + SHM fallback round-trip. When there's no intersection,
   renegotiate SHM-only directly.
   *Commit: "Fall back to SHM when no offered modifier is importable"*

4. **Validate CLI numeric arguments** — `-m`/`--multiplier` accepts 0,
   negatives, or garbage via `atoi`; `--drm-test-seconds` likewise via
   `atof`. Reject values outside sane ranges (multiplier 2–4, seconds > 0)
   with a usage error, before milestone 3 starts consuming the multiplier.
   *Commit: "Reject out-of-range CLI values"*

5. **Handle `findMemoryType` failure** — callers in `capture.cpp`
   (probe/staging allocations) pass its `UINT32_MAX` sentinel straight into
   `VkMemoryAllocateInfo::memoryTypeIndex`. Check and fail the setup path
   cleanly instead.
   *Commit: "Fail allocation setup when no memory type matches"*

Verification per increment: full rebuild with `-Wall -Wextra` (must stay
warning-free); `--help` smoke run. GPU-path behavior re-verified by the
owner on real hardware, as before.

### Explicitly out of scope for this branch

- Milestone 2 (cadence/dup detection) — lives on `4fk1ip`; duplicating it
  here would force the owner to pick a winner file-by-file.
- CI workflow — also on `4fk1ip`.
- Any reformatting/restyling of untouched code.

## Open questions for review

1. Merge `4fk1ip` for milestone 2, or should a fresh implementation land
   here instead?
2. This routine spawns a new branch per run — consider pointing it at a
   single long-lived branch (or pruning `htdg7b`/`i8yo5l`/`ugzdx1`, whose
   PLAN.md-only content is superseded).
3. Approve the five-fix edit pass above for Phase 2 on this branch?
