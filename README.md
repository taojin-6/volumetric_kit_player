# volumetric_kit_player

Plays a recorded `volumetric_kit` stream (`.vrst`) like a video player: each
frame is decoded and meshed on the GPU by
[volumetric_kit_recon](https://github.com/taojin-6/volumetric_kit_recon) and
drawn by [volumetric_kit_gfx](https://github.com/taojin-6/volumetric_kit_gfx),
paced by the stream's timestamps.

> **Status:** the playback clock (`src/volumetric_kit/player/playback.hpp`),
> which paces frames by their timestamps with play, pause and step; the
> stream player (`src/volumetric_kit/player/stream_player.hpp`), which decodes
> and meshes each frame on the GPU on a worker thread; the view
> (`src/volumetric_kit/player/player_view.hpp`), which draws each mesh under
> an orbit camera and a controls panel on any window system; and the desktop
> app (`apps/volumetric_kit_player/`), a GLFW window over the view.

[AGENTS.md](AGENTS.md) is the working guide for agents, and `CLAUDE.md`
imports it. [DECISIONS.md](DECISIONS.md) records the choices made.

## Prerequisites

A C++17 compiler (GCC or Clang, on Linux or macOS), CMake ≥ 3.21, and what
volumetric_kit_recon and volumetric_kit_gfx build with: a Vulkan SDK
(MoltenVK on macOS), `glslc` or `glslangValidator`, GLM, and GLFW 3.3 or
newer (`brew install glfw`, or `libglfw3-dev`) for the app. The first
configure fetches core, recon, gfx and googletest, so it needs network
access.

## Build and test

```bash
root="$(git rev-parse --show-toplevel)"
cmake -S "$root" -B "$root/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$root/build" --parallel
ctest --test-dir "$root/build" --output-on-failure
```

`VP_BUILD_TESTS` and `VP_WARNINGS_AS_ERRORS` default ON.

## Run

```bash
"$root/build/apps/volumetric_kit_player/volumetric_kit_player" <stream.vrst>
```

A Debug build runs under the Khronos validation layer. On macOS, put
Homebrew's layer on the search path, as the tests do (AGENTS.md):

```bash
cmake -S "$root" -B "$root/build-debug" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$root/build-debug" --parallel
DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib \
  "$root/build-debug/apps/volumetric_kit_player/volumetric_kit_player" <stream.vrst>
```

The panel at the bottom plays, pauses, steps and loops, and shows the frame.
The keys are its shortcuts:

| Input | Does |
| --- | --- |
| Left drag | orbits the camera |
| Right drag | pans it |
| Wheel | zooms |
| Space | plays or pauses |
| Right arrow | steps one frame |
| L | turns looping on or off |
| Esc | closes the window |

`--frames N` exits after N presents of a stream frame and prints the frame
drawn, for CI's smoke run (DECISIONS.md).

## Development

```bash
pre-commit install   # clang-format + cmake-format + hygiene hooks
```

## License

Released under the [MIT License](LICENSE). Each source file carries an
`SPDX-License-Identifier: MIT` header.
