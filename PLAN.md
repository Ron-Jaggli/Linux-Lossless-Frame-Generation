# PLAN — Linux-Lossless-Frame-Generation (lsfg-cap)

## Phase 1: Recon baseline (2026-07-05)

**Repository contents:** exactly one file, `README.md`, containing the single
line `# Linux-Lossless-Frame-Generation`. One commit (`a24407f "Initial
commit"`) on both `main` and the working branch.

**Build files:** none. No `CMakeLists.txt`, no `Cargo.toml`, no Makefile.

**CI config:** none. No `.github/workflows/`.

**Modules:** none. There is no source code.

**Build & test baseline:** nothing to build, nothing to run. Baseline is
"empty repo compiles trivially, zero tests exist, nothing is broken."

**Conclusion:** an "edit pass" over existing code is impossible — the project
must first be bootstrapped. This plan therefore proposes the initial project
skeleton and a set of small, independently buildable increments. The Phase 2
rules (buildable increments, imperative commits, no drive-by reformatting,
preserve public interfaces) still govern the work; they are simply trivially
satisfied at the start.

## Proposed scope

A standalone Linux frame-generation pipeline ("lsfg-cap"):
**capture → analyze → interpolate → present/emit**. It captures frames from a
source (screen via PipeWire, or a file/synthetic source for testing), detects
frame cadence, synthesizes intermediate frames, and emits the result at a
higher effective frame rate.

## Proposed technical decisions

| Decision | Choice | Why |
|---|---|---|
| Language | C++20 | Vulkan + PipeWire ecosystem is C/C++-native; matches prior art (lsfg-vk) |
| Build | CMake ≥ 3.22 | Standard for this stack; easy CI integration |
| Tests | Catch2 v3 via FetchContent | Header-friendly, no system dependency |
| CI | GitHub Actions, `ubuntu-latest` | Build + `ctest` on every push/PR |
| GPU path | Vulkan compute, runtime-detected | CI machines have no GPU, so CI runs the CPU reference path only |
| License | MIT (pending owner approval) | Permissive default; see open questions |

## Planned module map

```
src/
  core/      Frame, FrameClock, timing math, cadence detector (pure, unit-testable)
  capture/   FrameSource interface; FileSource (headless/CI), PipeWireSource (runtime)
  interp/    Interpolator interface; CpuBlendInterpolator (reference), VulkanInterpolator (later)
  present/   FrameSink interface; FileSink (CI-testable), window/swapchain sink (later)
  cli/       argument parsing, pipeline assembly, main()
tests/       unit tests per core module + end-to-end smoke test on synthetic frames
.github/workflows/ci.yml
```

## Increments (each one: build clean, tests green, then commit)

1. **Scaffolding** — `CMakeLists.txt`, `.gitignore`, expanded `README.md`,
   CI workflow; a stub executable and an empty-but-running test suite.
2. **Core types & timing** — `Frame`, `FrameClock`, cadence detector, with
   unit tests (this is where most of the pure logic lives).
3. **Capture layer** — `FrameSource` interface plus `FileSource` /
   `SyntheticSource` so everything downstream is testable headless;
   PipeWire backend added behind the same interface afterwards.
4. **CPU reference interpolator** — deterministic blend-based synthesis,
   verified against synthetic frame pairs in tests.
5. **CLI + end-to-end smoke test** — wire source → interpolator → sink;
   CI runs the full pipeline on synthetic input.
6. **Vulkan path (stretch)** — compute-shader interpolation behind runtime
   detection; never required by CI.

Commits stay small and imperative ("Add cadence detector", "Wire CLI
pipeline"). No increment lands with a red build or failing tests.

## Open questions for review

1. **Language:** C++/CMake proposed above — or would you prefer Rust/Cargo?
   (The task mentioned both `CMakeLists.txt` and `Cargo.toml`.)
2. **Product shape:** standalone capture-and-interpolate tool (assumed from
   "cap"), or a Vulkan layer injected into games like lsfg-vk?
3. **License:** MIT acceptable, or GPL?
4. **Baseline targets:** minimum Vulkan version / GPU generation, and is
   X11 support required or is Wayland/PipeWire-only fine?
