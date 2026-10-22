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
  `tests/vp_live_probe` plays one with no window, on a Mac.

### Changed

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

### Fixed

- `PlayerView::shown()` and the status line move onto a frame that meshes
  nothing, rather than staying on the frame before (recon #256).
- The `cmake-format` hook runs on Python 3.14, locally and in CI: it cannot
  start on 3.15, which Homebrew's pre-commit now uses.
- The stats' `draw` row times the whole render pass on the device, so on
  Apple GPUs it reads the pass rather than about 0.

### Migration

- Write a scenario in place of the command line (the core's CONFIG.md):
  `source.path` for the stream's path, `source.host` and `port` for
  `--connect host[:port]`, and `run.frames` for `--frames N`.
