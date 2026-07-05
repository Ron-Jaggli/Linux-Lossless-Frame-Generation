# Linux-Lossless-Frame-Generation (lsfg-cap)

Lossless Scaling-style **capture-based frame generation for any window on
Linux** — aimed at video playback (e.g. Crunchyroll in Firefox), not games.
Unlike [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk), which injects into a
game's Vulkan swapchain, this tool captures an arbitrary window through the
desktop portal and presents an interpolated stream in its own window.

## Pipeline

```
PipeWire screencast (xdg-desktop-portal, DMA-BUF zero-copy w/ SHM fallback)
  → duplicate detection + cadence recovery        [milestone 2]
  → LSFG frame interpolation 2x/3x/4x             [milestone 3]
  → Vulkan presentation window (SDL3)             [milestone 1 ✓]
```

## Status

| Milestone | State |
|---|---|
| 0 — DRM black-frame test (`--drm-test`) | implemented, needs a run against Crunchyroll |
| 1 — capture → display passthrough | implemented |
| 2 — duplicate detection + source cadence recovery | not started |
| 3 — LSFG shader integration, 2x interpolation | not started |
| 4 — 3x/4x, cadence-locked interpolation, polish | not started |

Milestone 3 will consume Lossless Scaling's shipped shaders the same way
lsfg-vk does — **you must own Lossless Scaling on Steam**; no assets are
bundled here.

## Building

Dependencies (Fedora names): `cmake ninja-build gcc-c++ pipewire-devel
vulkan-loader-devel vulkan-headers SDL3-devel libportal-devel glib2-devel`.

On an immutable distro (Bazzite/Silverblue), build inside a distrobox:

```sh
distrobox create --name lsfg-dev --image registry.fedoraproject.org/fedora-toolbox:43 --nvidia --yes
distrobox enter lsfg-dev -- sudo dnf install -y gcc-c++ cmake ninja-build \
    pipewire-devel vulkan-loader-devel vulkan-headers SDL3-devel \
    libportal-devel glib2-devel
distrobox enter lsfg-dev -- cmake -B build -G Ninja
distrobox enter lsfg-dev -- cmake --build build
```

The binary talks to the host's portal/PipeWire/GPU, so it runs fine from
inside the box: `distrobox enter lsfg-dev -- ./build/lsfg-cap`.

## Usage

```sh
lsfg-cap                     # pick a window, passthrough at display refresh
lsfg-cap --drm-test          # milestone 0: is the capture black? (exit 2 = black)
lsfg-cap -m 3 --fullscreen   # multiplier is parsed now, applied in milestone 3
lsfg-cap --present-mode mailbox --no-dmabuf --verbose
```

Keys: `F` fullscreen, `Esc`/`Q` quit.

The portal window picker appears on first run; the grant is remembered via a
portal restore token (`~/.config/lsfg-cap/restore_token`), so later runs
reattach without a dialog. `--no-restore` forces the picker.

### The DRM test (do this first)

The whole project hinges on protected video not being blacked out by the
compositor. Play a Crunchyroll video in Firefox, then:

```sh
lsfg-cap --drm-test
```

Pick the Firefox window. The tool samples mean luminance of every captured
frame for ~12 s and prints a verdict:

- `VERDICT: capture is NOT black` — proceed, frame generation is viable.
- `VERDICT: captured frames are BLACK` — the browser/compositor protects the
  surface. Plan B: different browser (Chromium flatpak), or a different
  source. Exit code 2.

On Linux, Firefox's Widevine (L3) decodes in software without a protected
video path, so window capture is *expected* to work — but verify before
building on it.

## Design notes

- **Language: C++20.** Milestone 3 integrates lsfg-vk's Vulkan pipeline
  (C++); PipeWire's C API and spa pod macros translate directly; Rust would
  put an FFI boundary exactly where the hardest integration lives.
- **Capture:** libportal drives the ScreenCast handshake; a `pw_thread_loop`
  consumes the stream. DMA-BUF is negotiated with explicit DRM format
  modifiers (queried from Vulkan, offered to the compositor, fixated on its
  choice); the SHM path is kept as an automatic fallback, including mid-stream
  renegotiation if a DMA-BUF import fails.
- **Threading:** capture thread copies each frame into a triple-buffered pool
  of `VkImage`s and fence-waits before requeueing the PipeWire buffer; the
  render thread blits the latest pool image to the swapchain. One shared
  graphics queue guarded by a mutex.
- **Latency:** every stats line logs end-to-end video delay (PipeWire pts →
  present), EMA-smoothed. Target < 50 ms so lipsync with browser audio holds.
- **Edge cases:** source resize renegotiates and rebuilds the pool; stream
  errors/session-close are detected and reported; corrupted chunks skipped;
  black frames detected by a 16×16 GPU downscale probe (every 30th frame in
  normal operation, every frame under `--drm-test`).

## Roadmap details

- **Milestone 2:** damage/new-buffer signaling where available, GPU hash
  compare otherwise; measure repaint pattern (e.g. 3:2 pulldown of 23.976 fps
  in a 60 Hz browser) and feed only unique frames onward, with a one-frame
  buffer so interpolation timing is exact.
- **Milestone 3:** LSFG shader chain lifted per lsfg-vk's approach, reading
  `Lossless.dll` from your Steam library (path autodetected from lsfg-vk's
  config when present).
- **Config file** (`~/.config/lsfg-cap/config.toml`) for defaults; GUI later.
