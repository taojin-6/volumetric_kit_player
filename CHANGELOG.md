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
- The decoder-side deblock (recon #264), on by default: Deblock in the panel
  and the desktop's D key turn it on and off (`StreamPlayer::set_deblock`
  and `deblock`), and while paused the frame shown is rebuilt with the
  change and drawn again (`FrameStamp::rebuild`). The stats say what it did
  to the frame drawn, off or its filter, the frame's T and, after a filter,
  how many of its blocks recon's per-block fallback (recon #274) kept at
  their exact decode, as "deblock LG · T 0.490 mm · 12 of 1,920 blocks
  exact" (`FrameStamp::deblock_on` and `deblock`); a stats line too wide for
  the screen breaks before its last part. The iOS app keeps the choice for
  the next source.
- Device timing on demand: `StreamPlayer::set_device_timing` (`DeviceTiming`
  off, sampled one frame in `kSampleEvery`, or all), set by the view every
  frame while the stats are open, or fixed by a measurement
  (`PlayerViewConfig::device_timing`); the view's draw is timed only while
  the stats are open and the timing is not off. A timed frame's "decode" and
  "mesh" carry recon's breakdown rows; a paused rebuild is never timed.
  `FrameStamp` gains `timed`, `pass` and `counters`, the worker's
  device counts over the frame; `StreamPlayer::track`,
  `PlayerView::frames_taken` and `compute_device`.
- The measure log (`MeasureLog`, `device_line`, `build_info`,
  `measure_file_name`): one CSV per run, its header naming the commits, the
  build, the device, the stream, how it ran and the timing, then each
  frame's stage rows and device counts; a paused rebuild is no frame, so it
  is not written. `tests/vp_probe` (once `vp_live_probe`) writes one of a
  file or a sender, through the view or with `--worker` the worker alone,
  validated as the core's test policy asks; the iOS app does with
  `--measure` (`parse_measure_args`, with `--passes`), or writes why it
  cannot into a `.failed` file.
- `tools/measure/measure`: from the Mac, a measurement on the lab's RTX 4090,
  the Mac itself or the owner's iPhone or iPad, its logs copied home, and
  `summarize.py`'s markdown of them (mean, p50, p95; counts per frame; a
  Mac run's load; a diff of two).
- `PlayerViewConfig::compact_stats`, which the iOS app sets on an iPhone: no
  breakdown rows, the rate over the renderer's table, and in landscape the
  two tables side by side.
- build: `VP_DESKTOP_APP` (ON on a desktop): OFF builds the library, the
  tests but `cli_test`, and `vp_probe` with no GLFW, as the runner does on
  the lab.
- One batch per frame: a file's frame is read into one of the decoder's
  input slots, and its decode and mesh are recorded into one command batch,
  one submit, finished once due; recorded ahead while playing, and a pause
  publishes the frame recorded. Live, each record's body is read straight
  into an input slot and decoded in a batch of its own, and the newest is
  meshed in another. `FrameStamp` gains `rounds`, `due`, `taken`,
  `presented`, `device_idle_ms` and `queue_ms`, and its stages "submit",
  "wait: GPU" and "finish"; `StreamPlayer::set_tile_depth` and
  `tile_depth`; `PlayerView::render_metrics` and `frame_waits`;
  `PlayerViewConfig::tile_depth` and `debug_labels`.
- The stats' GPU line (the frames' device time, the worker's queue idle and
  queue latency) and latency line (due to presented over the frames
  presented, and its legs: due to published, taken and presented), one "GPU ·
  latency" line compact (`PlaybackStats::frame_means`, `FrameFigures`,
  `frame_figures`).
- Measure format 2 (`MeasureLog::add` takes the render rows; `rings_line`;
  `MeasureContext`'s `rings`, `tile_depth`, `debug_labels`, `thermal` and
  `low_power`), which the iOS app writes with the phone's thermal state and
  Low Power Mode; `--tile-depth` and `--labels` for vp_probe, the iOS
  measure launch (`parse_tile_depth`, `parse_switch`) and the runner, whose
  summarizer reads formats 1 and 2 and shows the latency of frames with no
  present, the worker alone's, apart, from due to taken.
- CI reruns the stream and view suites under synchronization validation on
  its Release legs.
- build: another project can add the player (`add_subdirectory`) and link
  `volumetric_kit::player`: `VP_BUILD_APPS`, which is new, `VP_BUILD_TESTS`
  and `VP_WARNINGS_AS_ERRORS` default ON only when the player is the
  top-level project. The `consumer` tests build such a project, and CI runs
  them on its ubuntu-26.04 leg.
- The frame's capture set: `FrameStamp::set` and `LiveStatus::set`, its
  record's (recon's `RecordInfo::set`), `kNoSet` when it names none; the
  status line shows "set N", from a file and live.
- `PlayerViewConfig::share_vertices` and `mesh_wiring`'s `share_vertices`,
  on by default: off, each triangle has three vertices of its own. vp_probe
  (`--share-vertices on|off`), the iOS measure launch
  (`MeasureArgs::share_vertices`) and the runner take it.
- Measure format 3: the header's `share_vertices`
  (`MeasureContext::share_vertices`) and the frame row's `vertices` and
  `set`; the summarizer reads formats 1 to 3 and shows the setting and the
  vertices per frame.
- Per-frame hooks (`frame_hooks.hpp`): `FrameHooks`' `after_mesh` on the
  worker before each publish, in a batch of its own whose ready point the
  published mesh carries, `commit` at the take (`FrameCommit`: draw, hold
  or skip), `before_draw` before the render pass (`DrawBinding`: a set-0
  descriptor set and mesh flags cleared), and `on_park` and `on_resume`;
  `HookContext` and `MakeHooks`. `StreamPlayer::start` takes the hooks;
  `FrameStamp::extra` carries the caller's from after_mesh to the draw.
  `FrameTaker` is the take the view and `vp_probe --worker` share;
  `PlayerViewConfig::make_hooks`; `PlayerView::frames_skipped`,
  `frames_held`, `record_mesh` and `target_layout`; `mesh_wiring`'s `holds`,
  one mesh more in the ring.
- Unlit: a box in the panel and the desktop's U key show the mesh's vertex
  colours unshaded, through the view's own `DrawBinding`
  (`PlayerView::unlit`, `set_unlit`, `PlayerViewConfig::unlit`).
- `vp_probe --latency`, which times each frame from its mesh to its commit
  and its draws, and `--save-meshes DIR`, which writes each mesh as
  `DIR/<frame>.ply`, both through the hooks.
- The desktop app and vp_probe as library calls, for a program of its own:
  `run_desktop` (`desktop.hpp`, target `volumetric_kit::player_desktop`)
  and `run_probe` (`probe.hpp`, target `volumetric_kit::player_probe`),
  each taking an `AppConfig` (`app_config.hpp`) whose `view`, `make_hooks`
  included, the scenario or the command line sets over
  (`desktop_view_config` and `probe_view_config` give the result). vp_probe's
  `--latency` and `--save-meshes` exit 2 when a program set `make_hooks`,
  and its log's `run` names a program's hooks. CI's window step also plays
  the stream through `run_desktop` with a program's hooks
  (`tests/vp_desktop_hooks.cpp`).

### Changed

- build: the iOS app needs iOS 26 or newer: `cmake --preset ios` targets
  iOS 26.0, the family's iOS floor.
- build: the player pins core #61's merge (84dc584, 0.24.0), gfx main at
  #158's merge (0106609) and recon main at #295's merge (b9bb7d2), and
  requires core 0.24.0, the floor recon's library asks for. A commit hook's
  skip is recon's `CommitDecision::kDrop` (recon #293). The view presents
  FIFO as gfx's default (gfx #150), naming no mode. A set a program's
  `before_draw` binds is made with `HookContext::commit_allocator` too, as
  gfx's `HybridMeshTextureSet::create` takes an allocator (gfx #158).
- An `after_mesh` hook may record work that reads the grid it is handed,
  such as a compaction of its map (recon #295's const
  `VoxelHashMap::record_compaction`). The worker finishes the hook's batch
  before anything grows the grid: at the next frame's finish, where it has
  already run, and before recording a frame that outgrows the grid, rather
  than only at the next `after_mesh`, after a grow had refused the batch and
  stopped playback. The tests' streams add `outgrow.vrst`.
- The extractor's ring is two slots past the frames in flight
  (`mesh_wiring`); the draw waits on the GPU for the mesh's `ready` value;
  a park waits out a batch in flight and finishes its frame at unpark.
- The renderer's pass row is "draw (mesh+UI)". Compact, the worker's table
  holds read, decode and mesh, and the renderer's leaves out its loop's
  submit and names the draw "draw".
- `device_line` states the core's memory facts
  (`PhysicalDeviceInfo::device_mapped_memory`, `device_mapped_cached`).
- The phone runner builds on 6 jobs, names each timing's log
  `<utc>-<commit>-<mode>-<timing>.csv`, and lists the app's Documents before
  its measure folder, so it never asks devicectl for a folder the app has
  not made yet, and shows every other devicectl error.
- The stats show two tables, each under the window it covers: the worker's
  stages per stream frame and the renderer's per display frame, under its
  rate, each as host (busy), wait and GPU ms (gfx #153). `StatsPanel::draw`
  takes the renderer's `gfx::FrameMetrics`; `render_rows`, `window` and
  `render_window` read them back.
- A stage mean is over the frames that have its row, its device state the
  highest of theirs (`PlaybackStats::stage_means`); `window_frames` and
  `timed_frames` count them.
- The iOS app builds Release, in CI and in the README.
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
- A file's frame is recorded once due while paused, so the grid holds the
  frame shown while paused.
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
- A status or stats line too wide for the work area breaks between its
  parts, each row as many as fit, rather than once, before its last.
- The live worker connects, and connects again after an end, through
  `player::LiveConnection` (`live_connection.hpp`), its wait after an error
  `kLiveRetry`. `LiveAddress`, `parse_live_address`, `to_string` and
  `live_reason` move into that header, which `stream_player.hpp` includes.
  What a live player shows, and when it retries, are unchanged.
- The view takes its meshes through `FrameTaker`, which commits a mesh only
  once its ready value is reached and holds it until then.
- build: `VP_DESKTOP_APP` defaults to `VP_BUILD_APPS` and builds
  `player_desktop` with gfx's GLFW tier, so a project that adds the player
  may turn it on alone, with no app. The desktop app's and vp_probe's mains
  are one call each; `player_probe`, on every desktop, builds only when
  linked, and the player asks the core for its test policy there.

### Fixed

- The measurement runner stops when fetching a revision or launching the
  phone app fails, and retrieves lab logs when `LAB_ROOT` contains spaces.
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
- `mesh_wiring` counts one frame in flight as two, as recon's ring rule
  does, so a renderer of one frame in flight gets a ring of 4, not 3.

### Migration

- `vp_live_probe host[:port]` is `vp_probe --connect host[:port]`.
- `StatsPanel::draw` takes `profiler.metrics()`, not its `sections`; its
  `rows()` are the worker's alone, the renderer's are `render_rows()`.
- Read a row's `device` (`core::DeviceTime`), not `has_gpu`, and its
  `wait_ms`: host time less the wait is busy time.
- Write a scenario in place of the command line (the core's CONFIG.md):
  `source.path` for the stream's path, `source.host` and `port` for
  `--connect host[:port]`, and `run.frames` for `--frames N`.
- A consumer of `StreamPlayer` gives the extractor a `slot_count` two more
  than its frames in flight (`mesh_wiring` does), and a measurement's log
  passes `PlayerView::render_metrics().sections` to `MeasureLog::add`.
- `MeasureLog::add` takes the frame's mesh, for its triangles and vertices,
  in place of its triangle count: `add(stamp, view->mesh(), render)`.
- `draw_controls` takes the view's `unlit` after its `normals`.
