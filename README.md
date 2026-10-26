# volumetric_kit_player

Plays a recorded `volumetric_kit` stream (`.vrst`) like a video player, or a
sender's live stream over TCP: each frame is decoded and meshed on the GPU by
[volumetric_kit_recon](https://github.com/taojin-6/volumetric_kit_recon) and
drawn by [volumetric_kit_gfx](https://github.com/taojin-6/volumetric_kit_gfx),
paced by the stream's timestamps or by the sender.

> **Status:** the playback clock (`src/volumetric_kit/player/playback.hpp`),
> which paces frames by their timestamps with play, pause and step; the
> stream player (`src/volumetric_kit/player/stream_player.hpp`), which decodes
> and meshes each frame on the GPU on a worker thread, from a file or live
> from a sender; the view
> (`src/volumetric_kit/player/player_view.hpp`), which draws each mesh under
> an orbit camera, a controls panel and a stats panel on any window system;
> the per-frame hooks (`src/volumetric_kit/player/frame_hooks.hpp`), a
> caller's work on the worker after each mesh, at the take and before the
> draw; the desktop app (`apps/volumetric_kit_player/`), a GLFW window over
> the view; and the iOS app (`apps/ios/`), a UIKit shell over the view.

[AGENTS.md](AGENTS.md) is the working guide for agents, and `CLAUDE.md`
imports it. [DECISIONS.md](DECISIONS.md) records the choices made.

## Prerequisites

A C++17 compiler (GCC or Clang, on Linux or macOS), CMake ≥ 3.21, and what
volumetric_kit_recon and volumetric_kit_gfx build with: a Vulkan SDK
(MoltenVK on macOS), `glslc` or `glslangValidator`, GLM, and GLFW 3.3 or
newer (`brew install glfw`, or `libglfw3-dev`) for the app; and Python 3 for
the tests of the measure tools. The first configure fetches core, recon, gfx
and googletest, so it needs network access.

## Build and test

```bash
root="$(git rev-parse --show-toplevel)"
cmake -S "$root" -B "$root/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$root/build" --parallel
ctest --test-dir "$root/build" --output-on-failure
```

`VP_BUILD_APPS`, `VP_BUILD_TESTS` (never on iOS) and `VP_WARNINGS_AS_ERRORS`
default ON when the player is the top-level project, and OFF when another
project adds it with `add_subdirectory` to link `volumetric_kit::player`;
`-DVP_DESKTOP_APP=OFF` builds everything but the desktop app, with no GLFW.

## Run

The app plays a scenario, `--config <scenario.json>` (`--help` prints the
usage), in the format the core's
[CONFIG.md](https://github.com/taojin-6/volumetric_kit_core/blob/main/CONFIG.md)
gives for its `Player` profile. [config/examples](config/examples) has a
stream file's, [play_file.json](config/examples/play_file.json), and a live
sender's, [play_live.json](config/examples/play_live.json). Edit
play_file.json's `source.path` to point at a `.vrst`, or put one at
`config/examples/data/rafa2.vrst`:

```bash
"$root/build/apps/volumetric_kit_player/volumetric_kit_player" \
  --config "$root/config/examples/play_file.json"
```

A Debug build runs under the Khronos validation layer, and a Release build
when the scenario's `window.validation` is true. On macOS, put Homebrew's
layer on the search path, as the tests do (AGENTS.md):

```bash
cmake -S "$root" -B "$root/build-debug" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$root/build-debug" --parallel
DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib \
  "$root/build-debug/apps/volumetric_kit_player/volumetric_kit_player" \
  --config <scenario.json>
```

The panel at the bottom plays, pauses, steps and loops, and shows the frame
and, where its record names one, its capture set ("set N");
Normals, shown pressed while on, colours the mesh by its world-space normals
in place of lighting; and Deblock, on at first, has recon's decoder smooth
each frame across its block faces, and while paused redoes the frame shown.
Unlit shows the mesh's vertex colours unshaded; Normals takes precedence.
The stats at the top left show the bitrate, the frame drawn, what the
deblock did to it (its filter, T and the blocks it kept at their exact
decode), the frames' GPU time, the worker's queue idle and queue latency, the
latency from a frame's due time to its present, the rate and where each
frame's time goes: the worker's stages per stream frame and the renderer's
per display frame, each as host (busy), wait and GPU ms. The GPU is timed
while they are open; a click on their header closes and opens them. The keys
are the panel's shortcuts:

| Input | Does |
| --- | --- |
| Left drag | orbits the camera |
| Right drag | pans it |
| Wheel | zooms |
| Space | plays or pauses |
| Right arrow | steps one frame |
| L | turns looping on or off |
| N | turns Normals on or off |
| D | turns Deblock on or off |
| U | turns Unlit on or off |
| Esc | closes the window |

With `run.frames`, the app prints the frame drawn as it exits, for CI's
smoke run (DECISIONS.md).

### Live

A scenario with `source.host` in place of `source.path` plays what a sender
serves (recon's `serve_stream`, or its `fuse_orbbec` with `output.serve` in
a recon whose `fuse_orbbec` serves).
[play_live.json](config/examples/play_live.json) plays a sender on this
machine; for the lab's, over the VPN, copy it and set `source.host` to
`taojin-desktop.arena.andrew.cmu.edu` or `192.168.21.217`.

The picture joins at the sender's newest I-frame. The panel at the bottom is
then Normals, Deblock and Unlit over a status line: connecting, live with the
newest record's seq and set and how long nothing has come, or reconnecting
with why, about every second until Esc. The keys that play, pause, step and
loop do nothing. The stats show the bitrate and the records a second over
the last second.

`"$root/build/tests/vp_probe" --connect host[:port]` plays a live stream
with no window, and prints the status and stats lines once a second (below).

## iOS

The iOS app, for iPhone and iPad (arm64, iOS 26 or newer), plays a sender's
live stream or a `.vrst` from its Documents folder. It needs Xcode and
MoltenVK's iOS release, named by `MoltenVK_ROOT` as the core's
[README](https://github.com/taojin-6/volumetric_kit_core#prerequisites) says.
It builds Release, as it ships. Unsigned, as CI builds it:

```bash
cmake -S "$root" --preset ios
cmake --build "$root/build-ios" --config Release --target volumetric_kit_player
```

For a device, sign it with your Apple team's ID and a bundle ID of your own
if the default, `io.taojin.volumetrickit.player`, is taken; then install it.
The team ID is in Xcode › Settings › Accounts, and is the `OU` the first line
prints; the code in parentheses in a certificate's name is not it.

```bash
security find-certificate -c "Apple Development" -p | openssl x509 -noout -subject
cmake -S "$root" --preset ios -DVP_DEVELOPMENT_TEAM=<team id> [-DVP_BUNDLE_ID=<id>]
cmake --build "$root/build-ios" --config Release --target volumetric_kit_player -- -allowProvisioningUpdates
xcrun devicectl device install app --device <device id> \
  "$root/build-ios/apps/ios/Release-iphoneos/volumetric_kit_player.app"
```

The app's entitlement `com.apple.developer.kernel.increased-memory-limit`
needs that capability on its App ID; `codesign -d --entitlements - <app>`
shows whether the signed app has it.

Source, at the top right, opens iOS's dialog: Connect plays the sender at
Host and Port, on the sender's default port when Port is empty, and Play
file, offered while the app's folder holds a `.vrst`, plays the first one,
by name. What the dialog refuses it shows with why, keeping what was typed.
The app remembers the choice: at launch it connects to the last sender, if
that was the choice, or else plays the first `.vrst`. For the lab's sender
(above), put the phone on the VPN or the sender's network. On the sender's
network, not over the VPN, the first connection asks for local network
access; once refused, Settings › Privacy & Security › Local Network allows
it. A sender the app cannot reach is the status line's reason, retried
about every second until Source chooses another. While nothing plays, a
label says why, and a tap on it opens the dialog.

To give it a file, launch it once, then copy a `.vrst` into its folder: in
the Files app under On My iPhone (or iPad) › Player, in Finder under the
device's Files tab, or with devicectl:

```bash
xcrun devicectl device copy to --device <device id> \
  --domain-type appDataContainer --domain-identifier <bundle id> \
  --source <file> --destination Documents/<name>
```

Unless a sender is the choice, the app looks each time it builds its view,
and with no `.vrst` asks: at launch, on a choice, and on coming back to the
foreground with nothing playing. A file plays with loop on. A tap works the
panel, Source and the stats' header, one finger orbits the camera, two pan
it, and a pinch zooms.

It logs the view's bring-up time, each park's wait and any failure, which
Console.app shows. The owner checks the app on a device by hand
(DECISIONS.md); nothing runs it in CI. Devices it has run on: iPhone 18 Pro
Max (2026-10-20). Each one that check passes on is added here, and no others
are claimed.

## Measure

A measurement is one CSV log per run, which the player writes itself
([measure_log.hpp](src/volumetric_kit/player/measure_log.hpp) gives the
format, version 3): a header naming the commits, the build, the device, how
recon's rings run on it, the stream, how it ran, the timing, the tile depth,
the labels, whether vertices were shared and the time (and on a phone its
thermal state and Low Power Mode), then each stream frame's stage rows
(host, wait and GPU ms), the renderer's newest rows as it was taken (gfx's
profiler's, two display frames behind), and its frame row: the worker's
device counts (submits, waits, blocked time, staging buffers, dispatches,
barriers), what the deblock did, the rounds its decode ran, the queue's idle
and latency, its latency from due to published, taken and presented, its
mesh's vertices, and its record's set.

`vp_probe` writes one on a desktop, with no window:

```bash
"$root/build/tests/vp_probe" --file rafa2.vrst --passes 2 --timing all \
  --csv rafa2-all.csv [--worker]
```

It plays through the view on gfx's headless surface, or with `--worker` the
worker alone on the GPU the core picks, for a GPU with no headless surface
(NVIDIA's). `--timing` is `off`, `sampled` (one frame in `kSampleEvery`) or
`all`; `--tile-depth 1` tiles the worker's timed batches by "decode" and
"mesh" alone (0, every scope, by default); `--labels off` asks for no debug
labels; `--share-vertices off` gives each triangle three vertices of its
own (shared by default); `--frames N` stops after N frames;
`--connect host[:port]` plays a sender.

Two options run the player's per-frame hooks, as a caller would, so the
cost of that path shows too (a batch more a frame, which the log's `run`
names). `--latency` times each frame from its mesh finished on the worker
to its commit and, through the view, to its first draw, and the shown
frame's age at each draw, and prints their p50, p95 and max once a second
with the frames held or skipped (its commit draws every frame, so a frame is
held only by a take that found its mesh not yet ready); with `--worker`,
mesh to commit alone. A park leaves the frames in flight out, and the run's
last line says how many of its records are left unreleased, which fails it.
`--save-meshes DIR`
reads each mesh back in its hook's batch and writes it as `DIR/<frame>.ply`
once that batch has finished, a line each with its vertices, triangles and
the grid's blocks.

The iOS app writes one when launched with `-- --measure <name in Documents>
[--passes P] [--frames N] [--timing T] [--tile-depth D] [--labels on|off]
[--share-vertices on|off]`:
it plays that stream, logs into `Documents/measure/<utc>-<commit>.csv`, and
shows "measure done"; a launch that cannot log writes why into a `.failed`
file there instead.

[tools/measure/measure](tools/measure/measure), run from the Mac, does it end
to end, copies the logs into `~/ws/volumetric_kit/measurements/<date>/` and
prints their summary; its header lists the options and settings:

```bash
tools/measure/measure lab <ref>      # the lab's RTX 4090 (a branch, a commit, pr/<n>)
tools/measure/measure mac <ref>      # this Mac's GPU, with the owner's go-ahead
tools/measure/measure iphone <ref>   # or ipad: VP_DEVELOPMENT_TEAM set, unlocked
tools/measure/measure summarize before.csv after.csv
tools/measure/measure --dry-run lab <ref>   # prints the commands, runs none
```

Each runs two passes of rafa2.vrst, the first a warm-up, with timing off and
then all; `--tile-depth`, `--labels` and `--share-vertices` select the
probe's settings. For `mac` and `lab`, a setting at its default (0, on and
on) is not passed, so older probes also run, and a nondefault requires the
target probe to support the option. `lab` builds `vp_probe` in its own clone
of the player on the lab and runs it under `uvrun --time`. `mac` builds it in
the Mac's worktree for runs, on the siblings' checkouts that are clean at
their pins, and runs it once the Mac is idle, its load written beside each
log. `iphone` and `ipad` build the signed Release app in that worktree on 6
jobs, install it, copy rafa2.vrst into its Documents if missing, then launch
it once per timing; they start an app on the device, so they run only with
the owner's go-ahead. Each log is named `<utc>-<commit>-<mode>-<timing>.csv`.
`summarize` reads formats 1 to 3, and gives the settings, per stage, the
worker's and the renderer's, the mean, p50 and p95 of host, wait and GPU, the
counts and vertices per frame, the frames' figures (device time, queue idle
and latency, the latency to the present, or to taken for frames with none,
its legs, rounds), a run's load, and with two logs their diff.

## Development

```bash
pre-commit install   # clang-format + cmake-format + hygiene hooks
```

## License

Released under the [MIT License](LICENSE). Each source file carries an
`SPDX-License-Identifier: MIT` header.
