# Changelog

All notable changes to `volumetric_kit_player` are documented here. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- The repository's skeleton: CMake, the agents' guide, the formatting hooks,
  and CI that builds and tests on the family's self-hosted Mac and Linux GPU
  runners, with lint and the `required` gate hosted.
- `player::Playback`, the clock that paces a stream's frames by their
  timestamps, with play, pause and step.
- `player::StreamPlayer`: plays a `.vrst` stream's geometry on a worker
  thread, paced by its pts. The worker builds its decoder and extractor, so
  `start` returns at once and a failed build reaches `status()`.
- CI: a Mac leg that builds Debug under ThreadSanitizer and runs the tests.
- `player::PlayerView`: the window side of playback on any window system,
  over gfx: the shared device, a FIFO swapchain, and the newest mesh under an
  orbit camera and a controls panel, one `frame(extent)` per tick.
- The desktop app, `volumetric_kit_player`: a GLFW window over `PlayerView`,
  with mouse orbit, pan and zoom and keys for the panel's controls. CI plays
  a stream in it under Xvfb on the Linux GPU legs.
- An iOS build, `cmake --preset ios`, and the iOS app (`apps/ios/`): a UIKit
  shell over `PlayerView` that plays the first `.vrst` in its Documents
  folder, with touch for the panel and the camera, and a park while the
  scene is not active. CI builds it unsigned on the Debug Mac leg.
- `StreamPlayer::park` and `unpark`: hold the worker for a suspend, so it
  starts no GPU work until it resumes from the same frame.
- A stats panel at the top left (`StatsPanel`), which a tap or click on its
  header closes and opens: the track's mean bitrate, the frame drawn (I or P,
  its payload and triangles), the rate frames are drawn at, and the read,
  decode, mesh and draw times, decode's and draw's on the device too where it
  can time. `PlaybackStats` keeps the figures; `FrameStamp` carries the
  record's `bytes`, `key` and `stages`; `StreamPlayer::track_stats` and `rate`
  give the track's counts and nominal rate.
- Live mode: `StreamPlayer::connect` plays a sender's live stream (recon's
  `networking::StreamServer`) over TCP, and the desktop app plays a
  scenario's `source.host`. The worker decodes every record and meshes the
  newest; a connection that ends or is refused is retried about every
  second, its reason on the status line (`LiveStatus`, `live_reason`), and a
  park closes it. Live, the stats show the bitrate and records a second over
  the last second (`FrameStamp::records`, `record_bytes`, `published`).
  `tests/vp_probe --connect` plays one with no window.
- The iOS app chooses what to play on screen: a Source button at the top
  right (`PlayerViewConfig::source_button`, `draw_source`,
  `PlayerView::take_source_request`) opens iOS's dialog for a sender's host
  and port (`player::parse_live_source`) or the first `.vrst` in its
  Documents folder (`player::folder_source`), and the app remembers the
  choice for its next launch. It asks for local network access before its
  first connection to a sender on the local network
  (`NSLocalNetworkUsageDescription`).
- `player::open_source`: the player of a scenario's source, a file's or a
  sender's, which both apps call.
- A normal-coloured view: Normals in the panel, beside Loop and live over the
  status line, and the desktop's N key colour the mesh by its world-space
  normals (gfx's `kHybridMeshNormals`) in place of lighting
  (`PlayerView::normals`, `set_normals` and `mesh_flags`; `draw_controls`
  takes the view's `normals`).
- Device timing on demand: `StreamPlayer::set_device_timing` (`DeviceTiming`
  off, sampled one frame in `kSampleEvery`, or all), set by the view every
  frame while the stats are open, or fixed by a measurement
  (`PlayerViewConfig::device_timing`). A timed frame's "decode" and "mesh"
  carry recon's breakdown rows. `FrameStamp` gains `timed`, `pass` and
  `counters`, the worker's device counts over the frame;
  `StreamPlayer::track`, `PlayerView::frames_taken` and `compute_device`.
- The measure log (`MeasureLog`, `device_line`, `build_info`,
  `measure_file_name`): one CSV per run, its header naming the commits, the
  build, the device, the stream and the timing, then each frame's stage rows
  and device counts. `tests/vp_probe` (once `vp_live_probe`) writes one of a
  file or a sender, through the view or with `--worker` the worker alone;
  the iOS app does with `--measure` (`parse_measure_args`).
- `tools/measure/measure`: from the Mac, a measurement on the lab's RTX 4090
  or the owner's iPhone or iPad, its logs copied home, and `summarize.py`'s
  markdown of them (mean, p50, p95; counts per frame; a diff of two).
- `PlayerViewConfig::compact_stats`, which the iOS app sets on an iPhone: no
  breakdown rows, and in landscape the two tables side by side.

### Changed

- build: the player pins core #50's head (e4d00f1, 0.20.0), gfx #153's
  (e5da2bb) and recon ca8d7ea, #272's head merged with #269's (on recon main
  at ab8d01c), and requires core 0.20.0. TODO(timing): re-pin to their merge
  commits. The view presents FIFO as gfx's default (gfx #150), naming no
  mode.
- The stats show two tables, each under the window it covers: the worker's
  stages per stream frame and the renderer's per display frame, under its
  rate, each as host (busy), wait and GPU ms (gfx #153). `StatsPanel::draw`
  takes the renderer's `gfx::FrameMetrics`; `render_rows`, `window` and
  `render_window` read them back.
- A stage mean is over the frames that have its row, its device state the
  highest of theirs (`PlaybackStats::stage_means`); `window_frames` and
  `timed_frames` count them.
- The iOS app builds Release, in CI and in the README.
- Decode's device time is no longer always on: with the stats closed, recon
  writes no timestamp.
- The iOS app's controls fit a phone: 17-pt text (`ui_scale` 1.3) and every
  control at least 44 pt tall (`PlayerViewConfig::ui_min_frame_height`, over
  gfx's `min_frame_height`). Play/Pause and Step are `kButtonEms` (4.5) font
  sizes wide, rather than three frame heights.
- The status line no longer shows the frame's decode and mesh time;
  `FrameStamp::decode_mesh_ms` is gone.
- The desktop app takes a scenario, `volumetric_kit_player --config PATH`
  (or `--help`), which the core's `Application::Player` loads (core
  0.17.0); `config/examples/` has a stream file's and a live sender's. A
  command line or a scenario the core refuses exits 2.
- The library links the core's config tier on every platform, iOS
  included, rather than the desktop app alone.
- Meshes share their vertices (`mesh_wiring`), so the mesh ring holds,
  and each draw fetches, about a third of the bytes: Rafa2's from 191 to
  59 MB.
- Live, the swapchain has two images (`view_swapchain`), so a frame shows
  a vsync sooner; a file keeps gfx's default.
- The iOS app draws at most 60 frames a second, so a ProMotion iPad no
  longer redraws the same frame at 120 Hz.
- A consumer of `StreamPlayer` calls `taken()` from the commit of each
  mesh it takes; the worker sleeps until then rather than waking every
  2 ms, as it did while a window was minimized. Live, a frame waiting on
  that take is meshed at once, rather than at the next 10 ms socket wait.
- The tests' streams are written once a ctest run, by `vp_test_streams`
  (`vp_write_stream <dir>`), rather than encoded again by every device test.
- The live park test parks 8 times; its 50-cycle soak is the `stress` ctest
  `vp_live_park_soak`, which CI leaves out.
- CI's NVIDIA legs keep the driver's shader cache, in the runner's tool
  cache, rather than compile every pipeline in every test process.

### Fixed

- A file's or a sender's frame can no longer make the decode grid gigabytes
  large: one of more than `kMaxFrameBlocks` (131,072) blocks, or a grow past
  the grid for it, is skipped as a damaged one is (recon #266's ceiling).
- Live input drains before the worker waits for a consumer to take a mesh,
  including records spanning multiple receives, so an untaken mesh does not
  add a 10 ms delay to every queued record or partial receive.
- `PlayerView::shown()` and the status line move onto a frame that meshes
  nothing, rather than staying on the frame before (recon #256).
- The `cmake-format` hook runs on Python 3.14, locally and in CI: it cannot
  start on 3.15, which Homebrew's pre-commit now uses.
- The stats' `draw` row times the whole render pass on the device, so on
  Apple GPUs it reads the pass rather than about 0.

### Migration

- `vp_live_probe host[:port]` is `vp_probe --connect host[:port]`.
- `StatsPanel::draw` takes `profiler.metrics()`, not its `sections`; its
  `rows()` are the worker's alone, the renderer's are `render_rows()`.
- Read a row's `device` (`core::DeviceTime`), not `has_gpu`, and its
  `wait_ms`: host time less the wait is busy time.
- Write a scenario in place of the command line (the core's CONFIG.md):
  `source.path` for the stream's path, `source.host` and `port` for
  `--connect host[:port]`, and `run.frames` for `--frames N`.
