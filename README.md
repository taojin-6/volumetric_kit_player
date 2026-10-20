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
> an orbit camera and a controls panel on any window system; the desktop
> app (`apps/volumetric_kit_player/`), a GLFW window over the view; and the
> iOS app (`apps/ios/`), a UIKit shell over the view.

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

## iOS

The iOS app plays the first `.vrst`, by name, in its Documents folder, for
iPhone and iPad (arm64, iOS 16 or newer). It needs Xcode and MoltenVK's iOS
release, named by `MoltenVK_ROOT` as the core's
[README](https://github.com/taojin-6/volumetric_kit_core#prerequisites) says.
Unsigned, as CI builds it:

```bash
cmake -S "$root" --preset ios
cmake --build "$root/build-ios" --config Debug --target volumetric_kit_player
```

For a device, sign it with your Apple team's ID, which
`security find-identity -v -p codesigning` shows in parentheses, and a bundle
ID of your own if the default, `io.taojin.volumetrickit.player`, is taken;
then install it:

```bash
cmake -S "$root" --preset ios -DVP_DEVELOPMENT_TEAM=<team id> [-DVP_BUNDLE_ID=<id>]
cmake --build "$root/build-ios" --config Debug --target volumetric_kit_player -- -allowProvisioningUpdates
xcrun devicectl device install app --device <device id> \
  "$root/build-ios/apps/ios/Debug-iphoneos/volumetric_kit_player.app"
```

The app's entitlement `com.apple.developer.kernel.increased-memory-limit`
needs that capability on its App ID, which automatic signing adds.

To give it a stream, copy a `.vrst` into the app's folder: in the Files app
under On My iPhone (or iPad) › Player, or in Finder under the device's Files
tab. Each time the app comes back to the foreground it looks again, and
until it finds one it says so. It plays with loop on; the panel at the
bottom plays, pauses, steps and loops. One finger orbits the camera, two pan
it, and a pinch zooms.

The owner checks the app on a device by hand (DECISIONS.md); nothing runs it
in CI. The GPUs it is known to run on are those that check confirms, listed
here once it has; no others are claimed.

## Development

```bash
pre-commit install   # clang-format + cmake-format + hygiene hooks
```

## License

Released under the [MIT License](LICENSE). Each source file carries an
`SPDX-License-Identifier: MIT` header.
