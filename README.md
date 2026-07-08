# Linux-Lossless-Frame-Generation (lsfg-cap) THIS WILL BE VIBECODED!!!!
# YOU WILL NEED A COPY OF LOSSLESSCALING(working on one without it right now but not opensource ;)
Lossless Scaling-style **capture-based frame generation for any window on
Linux** — aimed at video playback (e.g. Crunchyroll in Firefox), not games.
Unlike [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk), which injects into a
game's Vulkan swapchain, this tool captures an arbitrary window through the
desktop portal and presents an interpolated stream in its own window.

## Pipeline

```
PipeWire screencast (xdg-desktop-portal, DMA-BUF zero-copy w/ SHM fallback)
  → duplicate detection + cadence recovery        [milestone 2 ✓]
  → frame interpolation 2x/3x/4x                  [milestone 3a ✓ blend baseline;
                                                   3b: LSFG shaders]
  → Vulkan presentation window (SDL3)             [milestone 1 ✓]
```

## Status

| Milestone | State |
|---|---|
| 0 — DRM black-frame test (`--drm-test`) | implemented, needs a run against Crunchyroll |
| 1 — capture → display passthrough | implemented |
| 2 — duplicate detection + source cadence recovery | implemented |
| 3a — frame pacing + pair leases + blend-baseline interpolation (2x/3x/4x) | implemented, needs a run on real hardware |
| 3b — LSFG shader integration | not started |
| 4 — cadence-locked interpolation polish | not started |

Milestone 3b will consume Lossless Scaling's shipped shaders the same way
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

Unit tests (pure logic, no GPU/display needed): `ctest --test-dir build`.
On a machine without the app's dependencies, configure with
`-DLSFG_BUILD_APP=OFF` to build and test just the core.

## Usage

```sh
lsfg-cap                     # pick a window, 2x frame generation (default)
lsfg-cap -m 1                # plain passthrough at display refresh
lsfg-cap --drm-test          # milestone 0: is the capture black? (exit 2 = black)
lsfg-cap -m 3 --fullscreen   # 3x generation, fullscreen
lsfg-cap --present-mode mailbox --no-dmabuf --verbose
```

Keys: `F` fullscreen, `G` toggle frame generation, `Esc`/`Q` quit.

Frame generation engages only once the cadence tracker locks onto the
source rate; until then (and whenever it unlocks — pause, seek, scene
of irregular repaints) the tool shows real frames unmodified. The stats
line reports generated output as e.g. `output 60.0 fps (36.0 gen, 2x)`.

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
I am using waterfox so like if you do run into problems then use chrome or waterfox but most browsers might work

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
- **Duplicate detection + cadence (milestone 2):** a 64×64 GPU downscale
  probe runs on every frame; consecutive probes matching within 1 LSB per
  color byte mark a duplicate repaint. A pure, unit-tested `CadenceTracker`
  turns the (timestamp, duplicate) stream into the recovered source rate and
  repeat pattern (e.g. `23.98 fps (3:2)` for film in a 60 Hz browser), also
  handling damage-driven compositors that never deliver duplicates. Shown in
  the stats line; milestone 3 consumes it to interpolate only unique frames.
- **Frame generation (milestone 3a):** a pure, unit-tested `FramePacer`
  decides at every display refresh what to show — the latest real frame,
  or an in-between of the last two unique frames at a phase quantized to
  the multiplier. The A→B interval plays over the source period after B
  arrives, so interpolation inherently adds ~one source period of delay
  (~42 ms at 24 fps): honest, measured, shown in the stats line — it can
  exceed the 50 ms lipsync target on slow sources. The baseline
  `Interpolator` is a single-pass compute blend `mix(A, B, phase)` —
  expect ghosting on motion; it validates pairing, pacing, and present
  timing so the LSFG shader chain (3b) can drop in behind the same
  interface.
- **Edge cases:** source resize renegotiates and rebuilds the pool; stream
  errors/session-close are detected and reported; corrupted chunks skipped;
  black frames detected by the same probe's mean luminance.

## Roadmap details

- **Milestone 3b:** LSFG shader chain lifted per lsfg-vk's approach, reading
  `Lossless.dll` from your Steam library (path autodetected from lsfg-vk's
  config when present).
- **Config file** (`~/.config/lsfg-cap/config.toml`) for defaults; GUI later.
