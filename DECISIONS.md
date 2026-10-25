# Decisions

The choices made for `volumetric_kit_player`, newest first, and why.
[AGENTS.md](AGENTS.md) is the working guide; [README.md](README.md) says what
is implemented.

## 2026-10-25 — Another project can build on the player's library

- **A project can add the player (`add_subdirectory`) and link
  `volumetric_kit::player`,** as core, recon and gfx are added:
  `VP_BUILD_APPS`, `VP_BUILD_TESTS` and `VP_WARNINGS_AS_ERRORS` default ON
  only when the player is the top-level project. Why: the project that adds
  it then builds the library alone, with no GLFW and under its own
  compiler's warnings. The player stays an application that installs and
  exports nothing (2026-10-18).
- **`VP_DESKTOP_APP` stays, under `VP_BUILD_APPS`,** which turns it off. Why:
  the measurement runner passes it to every commit it builds, older ones
  too.
- **A consumer project builds, not only configures** (`tests/consumer/`): it
  adds the player, each dependency the build fetched passed on as a local
  source, and builds and runs a program that links the library; beside it,
  the player configured alone has all three options ON. Why: a configure
  would not show that the library links outside the player's own tree. They
  check CMake, not a device, so CI runs them (label `consumer`) on the
  ubuntu-26.04 leg alone.

## 2026-10-25 — One batch per frame

Wave 2 of the data-flow plan (player PR 7: PL3 and PL4), over core's
`HostRing` and recon's recorded decode and extract. The owner: "I want timing
to be comprehensive and correct. No need for unnecessary copy or waits";
"everything should be GPU with zero-copy"; "if the new design is beneficial
everywhere, we do not need the older pipelines".

- **A file's frame is one command batch:** its record read into one of the
  decoder's input slots (recon's `StreamReader::read` into the slot), its
  decode (`codec::record_growing`) and its mesh (`record_extract`, gated on
  the decode) recorded together, submitted without a wait, and finished once
  due (`finish_growing`, `MarchingCubes::finish`). A steady frame of the
  tests' stream is 1 submit, 0 staging buffers, 39 dispatches and 47
  barriers in place or 48 copied, I and P alike, where M0's was 6 submits
  and 7 staging buffers; the tests pin the counts, as Apple GPUs step in time
  near 80 of either (recon's DECISIONS.md). The old per-call path is gone.
- **The rings take their path from the device:** in place where
  device-mapped memory is cached (Apple, lavapipe), copied elsewhere
  (NVIDIA), as core's `HostRing` decides; the player has no setting for it,
  and the log's `rings` key states it.
- **Recorded ahead only while playing.** Playing, the next frame is recorded
  as soon as the one before is published, so its mesh is ready when due.
  Paused or held, a frame is recorded only once due, and a pause publishes
  the frame already recorded, so whenever the clock is paused the grid holds
  the frame shown, which `Decoder::rebuild` redoes after a toggle. A pause
  lands at most one frame period late: it shows the frame already recorded,
  ahead of its due time. A toggle while playing reaches the second frame
  after it.
- **The ring is two slots past the frames in flight** (`mesh_wiring`): one
  mesh published and not yet taken, one meshed in the next frame's batch
  before that take. One more vertex arena: about 19 MiB for rafa2, 119 MiB
  for the live room.
- **The draw waits for the mesh's `ready` value on the GPU**
  (`gfx::windowing::FrameWait` at `VERTEX_INPUT | DRAW_INDIRECT`): reached
  before the mesh is published, so it stalls nothing; it carries the
  worker's writes across queues, where the fence alone did not.
- **A park waits out a batch in flight,** submitting nothing, and the frame
  is finished and published at unpark, a grow, retry or refit included: a
  submit from the background loses the device on iOS. A park finds the worker
  in its pacing sleep, which it wakes.
- **Live, each record is a decode batch of its own,** its body read
  straight into an input slot (`StreamClient::read`) and finished before the
  next is recorded, as the next parses against its reference; the newest
  frame decoded is meshed in a batch of its own once its decode is judged.
  The arrival stamp is taken when the complete record is received, before
  finishing the preceding decode, so latency includes that wait or recovery.
- **The timing:** a frame's rows are read, decode and mesh (recording),
  submit, `wait: GPU` (about 0 while the GPU keeps up) and finish, the batch's
  device rows folded in once after its wait; the queue's idle before the
  worker's batch, from raw ticks when the batch before was timed and nothing
  came between; its queue latency where the device calibrates; and the
  latency from due, the time the clock set rather than the worker's wake-up,
  to published, taken and the present call, the whole over the frames
  presented: a mean of a frame set with each leg's own would mix frames
  presented with the worker alone's. The renderer's pass row is "draw
  (mesh+UI)", as Apple GPUs time only a pass's bounds. Tile depth and debug
  labels are measure options for M4 (`--tile-depth`, `--labels`).
  The runner omits these options at their defaults so older probes can
  still measure a baseline; a nondefault requires support from that probe.
- **The compact stats keep their height:** one "GPU · latency" line over the
  renderer's table beside the rate, the worker's table read, decode and mesh,
  and the renderer's without its loop's submit, its draw named "draw". Why:
  on the view tests' shortest notched phone in landscape the panel already
  ended at the controls' top, and two more rows took it 35 pt past them; in
  portrait "draw (mesh+UI)" took the body 2 pt past the narrowest phone.
- **Measure format 2:** the renderer's rows as of the tick that took a
  frame, the dispatches and barriers, what the deblock did, the rounds, the
  idle, the queue latency and the latency legs per frame, under `rings`,
  `tile_depth` and `labels`, and a phone's thermal state and Low Power Mode.
  The summarizer reads formats 1 and 2, so M2 diffs against M0 row for row.
  The render rows are gfx's profiler's newest resolved frame, two display
  frames behind the one that drew the stream frame, as its snapshot lags the
  frames in flight and carries no frame number to pair by; their means are
  the renderer's all the same.
- **The phone runner** builds on 6 jobs (the Mac's load rule), names each
  timing's log as the lab's and the Mac's are, and lists the app's Documents
  before its measure folder, so it never asks devicectl for the folder the
  app has not made yet, and every other devicectl error shows.
- **CI reruns the worker's and the view's suites under synchronization
  validation** (`VKC_TEST_SYNC_VALIDATION`) on the Release legs: NVIDIA's,
  the rings copied, and the Mac's, in place and across MoltenVK's two
  families, where a regression in the one batch's barriers or the draw's
  wait would show.
- **The tests' grow stream** has a key whose blocks nearly fill the decode
  grid its first frame sized, so the table refuses some on the device and
  recon grows the grid at the frame's finish: a recovery the park tests can
  find due.

## 2026-10-24 — The stats count the blocks the deblock kept exact

The stats' deblock line over recon #274's per-block fallback
(`codec::DeblockInfo`'s `block_count` and `exact_blocks`).

- **The line names the filter, T and the blocks kept exact:** "deblock LG ·
  T 0.490 mm · 12 of 1,920 blocks exact". Why: recon no longer skips a frame
  for an unobserved voxel; it keeps each block holding one at its exact
  decode and filters the rest, so the count is what tells a fused room's
  frames, nearly every block exact, from a true SDF's, none. With no filter,
  for the deblock off or a T finer than every filter's, it says "off" and no
  count, as every block is exact; "skipped" is gone.
- **A stats line too wide for the room it has breaks before its last " · ",**
  the rest indented below it. Why: at the block ceiling the deblock line is
  57 characters, about 520 pt at the iOS shell's scale, past a 375-pt phone
  in portrait.
- **Compact, the rate sits over the renderer's table, and beside it the
  worker's lines take the width it leaves.** Why: in landscape, side by side
  with the sphere's 48-character deblock line on one row, the columns were
  768 pt wide on the view tests' shortest notched phone, whose stats hold
  706. Broken, the line adds a row to the worker's column, the taller, which
  sets how far above the controls the panel ends (2026-10-23); the rate's
  row moved to the renderer's, the shorter, leaves the panel as tall as
  before, and 624 pt wide (lavapipe, 2026-10-24).

## 2026-10-23 — Timing on demand, each unit stated, and one measure log

Wave 1 of the data-flow plan (its plan v2, row 4: PL0, PL1, PL2 and the
measurement automation), with D1 to D10 as recommended. The owner: "I want
timing to be comprehensive and correct. No need for unnecessary copy or
waits"; "you should automate the measurement process, how you get the data
log". Nothing in the data flow changes; today's costs become measurable.

- **The device is timed only while the stats are open, or as a measurement
  fixes it** (`StreamPlayer::set_device_timing`,
  `PlayerViewConfig::device_timing`): the worker's frames, and the draw,
  whose gfx profiler the view attaches only while the stats are open and the
  timing is not off. Untimed, recon gets null metrics; the host and wait
  halves stay. Why: a timestamp costs device time (recon's DECISIONS: a
  command buffer's first costs about 0.13 ms on an M5 Max), and nobody reads
  a closed panel; a measurement's "off" writes no timestamp anywhere.
  gfx's profiler has no switch of its own: detached, the renderer's table is
  empty rather than stale, and just after it reopens gfx's rate still
  averages over the gap. `kSampled` is for measuring timing's cost (M4).
  This replaces "decode's device time is always on" (2026-10-20).
- **recon's breakdowns are forwarded** under the player's "decode" and
  "mesh", which get recon's device time themselves, as core 0.20.0 counts a
  tile to every scope open around it: the old loop that added "codec
  decode"'s to "decode" would count it twice.
- **Each stamp's device counts are the worker's since the last publish**,
  counted on the worker, so a frame's counts are its own read, decode and
  mesh, not whatever the worker began after it.
- **The stage means are over the frames that have each row,** a row only
  timed frames have kept under its stage, and the device state the highest
  of the frames', as the core folds one. Why: with sampling, a breakdown's
  mean is the timed frames', and a mean reads measured only when every frame
  that timed the row measured it. The summarizer places rows by the same
  rule.
- **Two tables, each under its window** (`gfx::ui::StageWindow`). Why: one
  table mixed stream frames and display frames.
- **Compact stats on an iPhone** (`PlayerViewConfig::compact_stats`). Why:
  gfx's three loop rows alone took the panel past the controls on the
  shortest phone in landscape, and gfx's window line is wider than the
  narrowest in portrait; an iPad has room for the full panel.
- **One log format, written by the player** (`MeasureLog`). Why: every
  measurement, desktop or phone, before or after a change, compares row for
  row, and a log says what it measured and how it ran: through the view,
  whose draw shares the GPU, or by the worker alone.
- **A paused deblock toggle's rebuild is no frame** (`FrameStamp::rebuild`):
  the log skips it as `PlaybackStats` does, and it is never timed. Why: it is
  the frame before again, so a row for it would repeat that frame's place with
  another stage's time, and a timestamp nobody reads only costs device time.
- **`vp_probe --worker`**, the worker alone on the GPU the core picks, with
  nothing drawn. Why: NVIDIA's ICD has no headless surface, so on the lab box
  the view runs on lavapipe; the render rows wait for PL4, so the worker's
  rows lose nothing. It validates as the core's test policy asks, so its
  ctest runs are validated where the tests are.
- **A measure launch that cannot log writes why** into a `.failed` file
  beside the logs. Why: the runner cannot see the phone's screen.
- **The runner, `tools/measure/measure`, runs from the Mac, which
  orchestrates.** On the lab it keeps its own clone of the player in the
  lab's scratch, holds a lock for the whole run, and builds with
  `VP_DESKTOP_APP=OFF`. Why: it leaves the owner's checkout alone, two runs
  cannot mix their checkout, build and logs, and it needs no GLFW, which the
  lab lacks; GLFW stays the desktop app's prerequisite, never a download.
  Two passes, the first a warm-up, and timing off then all, as M0 reads host
  rows untimed and device rows timed. A phone run starts an app on the
  owner's device, so it runs only with the owner's go-ahead.
- **`mac` measures on the Mac itself, with the owner's go-ahead,** once the
  Mac is idle, its load written beside each log. Why: the owner allowed it
  with the iPhone away (2026-10-23: "you can measure on a mac"), and its GPU
  is Apple's under MoltenVK, as a phone's is; the Mac is otherwise shared
  and only orchestrates, so a run waits for it to be idle and says how busy
  it was. It builds on the siblings' checkouts clean at their pins, as
  `vp_build_info` reads clean, so it fetches only what no checkout has.
- **iOS builds Release,** in CI's iOS step and the README: a measurement and
  the owner's checks run what ships, not an `-O0` build.

## 2026-10-23 — The decoder-side deblock, on by default

`StreamPlayer::set_deblock` and `deblock`; `FrameStamp`'s `deblock_on`,
`deblock` and `rebuild`; the panel's Deblock, the stats' deblock line and the
desktop's D key, over recon #264's `Decoder::set_deblock`, `rebuild` and
`last_deblock`.

- **On by default,** the owner's call (2026-10-23); recon's `Decoder` keeps
  it off. It is out of loop and costs no bits (recon's `decoder.hpp`), so it
  may change between any two frames.
- **`StreamPlayer` holds the choice,** a control as Loop is, safe from any
  thread, and the worker gives it to the decoder before each decode, as the
  decoder is the worker's alone; live too, from the next record. The iOS
  shell hands it to the player it builds for a new source or after a
  failure.
- **A toggle while paused, or held at the end, redoes the frame shown:** the
  worker rebuilds it from the decoder's reference (`Decoder::rebuild`),
  meshes it and publishes it again, stamped `rebuild`, which `PlaybackStats`
  does not count. Why: otherwise the change shows only at the next step.
  Playing, the next frame shows it.
- **So whenever the clock is paused the grid holds the frame shown,** which
  `rebuild` redoes: the frame decoded last. The clock counts a frame shown as
  of when it was due, so a slow decode does not push the frames after it
  back, but only once it decodes, so a frame that fails takes no step.
  *Since 2026-10-25 frames are recorded ahead while playing, and a pause
  publishes the frame recorded; until then a frame was decoded once due.*
- **The stats name what the deblock did to the frame drawn** (2026-10-24
  for the line), T in mm to the micrometre, as the rule's lowest threshold
  is 0.075 mm.
- **Deblock is a box after Normals, or, where that row would not fit the work
  area, on a row of its own below it:** a phone's row in portrait was 365 pt
  of 375 already. In landscape there is no height for a row: with the
  deblock line and the stage tables the stats end 8 pt above the controls on
  the view tests' shortest notched phone (lavapipe, 2026-10-24), and the row
  with Deblock is 488 pt wide.
- **The tests' true-SDF stream** codes the sphere with no unobserved voxel,
  so the deblock filters every block of it (filter R); every block of the
  other streams' frames is masked, so it keeps them all exact.

## 2026-10-22 — A frame holds at most 131,072 blocks

`kMaxFrameBlocks`, the ceiling recon's `codec::make_decode_grid` and
`decode_growing` take.

- **2^17 blocks, so the decode grid's voxels stay within 1 GiB:** it has a
  slot for twice a frame's blocks, 4 KiB each (512 voxels of float `tsdf` and
  `weight`), and recon grows it no further. Why: a file's or a socket's bytes
  are untrusted, and a forged header could otherwise make a grid of
  gigabytes; the largest stream the family codes, room0 at 5 mm (about
  107,000 blocks, recon's DECISIONS.md), fits; and one ceiling serves every
  device, so it is sized for a phone, not the desktop's GPU.

## 2026-10-22 — A normal-coloured view, toggled from the panel

`PlayerView::normals`, `set_normals` and `mesh_flags`; the panel's Normals and
the desktop's N key.

- **gfx's `kHybridMeshNormals` in place of lighting,** so the mesh's own
  normals show, such as Marching Cubes' facets on a noisy live SDF.
- **The view holds the choice:** it is the drawing thread's, as the camera
  is, while `StreamPlayer`'s controls are playback's, safe from any thread;
  `draw_controls` takes it by reference.
- **A button shown pressed while on, `kButtonEms` wide,** not a box as Loop's:
  with its label a box took a phone's row to 402 pt, past 375; the button
  keeps it at 365.
- **Live too:** the shading is the view's, so a sender's stream takes it as a
  file's does.
- **No scenario field:** the core's Player profile has no render section; a
  default from the scenario waits for a need.

## 2026-10-22 — The iOS app chooses what to play on screen

`PlayerViewConfig::source_button`, `draw_source`,
`PlayerView::take_source_request`, `player::parse_live_source`,
`folder_source` and `open_source`; README.md says how to use it.

- **The Source button opens iOS's own dialog** (`UIAlertController`). Why:
  the owner asked for the sender's address on screen, not in a file; ImGui
  has no touch keyboard on iOS, and the system keyboard would cover an ImGui
  field in the panel at the bottom.
- **Source is opt-in, a button of its own at the work area's top right,**
  file or live, as tall as the stats' header beside it, so their body clears
  it. A tap only records a request, which the shell takes after `frame()`.
  Why: the desktop takes a scenario; no UIKit runs inside the ImGui frame;
  and the controls have no room for it. Their playback row is full with
  Normals (365 of 375 pt), the status line's row is full beside a 5-digit
  track's line, and a row of its own would lift the controls into the stats'
  body on a landscape phone.
- **One parser for the typed fields, `parse_live_source`,** over
  `parse_live_address`, into the source `open_source` plays, the desktop
  app's call too, so the library links the core's config tier on every
  platform. An empty port is port 0, the sender's default, as in a scenario;
  a host with a ':' points at the Port field. Why: the port's range and the
  default port each keep one home.
- **The choice is remembered** in `NSUserDefaults`: the sender's host and
  port as the parser took them, or the file. While nothing plays, a tap on
  the label asks. Why: the lab's sender is typed once, and with no view
  there is no Source button.
- **The listing stays in Objective-C,** sorted as Finder sorts it
  (`localizedStandardCompare:`), and `folder_source` takes the names in that
  order. Why: no C++ collation matches Finder's, and the choice is still
  tested on the host.
- **`NSLocalNetworkUsageDescription` is the one key live adds.** Why: iOS
  asks before the first connection to a host on the local network. ATS
  covers URL loading, not a BSD socket, and nothing is discovered, so there
  are no Bonjour keys.

## 2026-10-21 — The desktop app takes a scenario

`apps/volumetric_kit_player/main.cpp` and `config/examples/`; README.md says
how to run it.

- **`--config PATH` or `--help` alone,** through the core's `parse_args`,
  `usage` and `load(path, Application::Player)`, as recon's examples
  (through its `configured_main`), gfx's and calib's start. The core owns
  the format and the profile's fields (its CONFIG.md); a command line or a
  scenario it refuses exits 2, as recon's examples do. Why: one way to start
  the family's scenario-driven programs, and a scenario names a stream or a
  sender, the window and a count once, in a file.
- **A `source.port` of 0, or none, keeps `LiveAddress`'s port,** recon's
  `networking::kDefaultPort`. Why: the sender's default has one home.
- **`vp_live_probe` keeps its `host[:port]`:** a tool for agents, not a
  program a scenario starts. *`vp_probe` takes `--connect` since
  2026-10-23.*

## 2026-10-21 — Live mode: a sender's stream over TCP

`StreamPlayer::connect`, a scenario's `source.host` and
`tests/vp_live_probe`; the headers say what each call does. recon's
`networking::StreamClient` reads what a `StreamServer` sends: the `.vrst`
chunks, joined at the newest key.

- **Decode every record, mesh the newest.** The worker decodes every record,
  and meshes once the consumer has taken the last mesh: when
  nothing more has come, at once for a connection's first frame, or every
  100 ms while records keep coming, so a join shows a picture at once and
  its GOP as it drains. Not paced by pts: the sender paces. Why: the
  file loop's wait for the consumer would let latency grow without bound and
  stop draining the socket, which hides lag from the sender's drop rule
  (newest key wins). The consumer check stays: a superseded mesh keeps its
  ring slot, and a minimized window takes none. With a frame in hand, the
  worker drains immediately available input before waiting for the take
  (`taken()`), 10 ms at a time, so it meshes at once on the take without
  throttling a queued burst. `StreamClient::next` reports progress separately
  from a whole record: a partial receive must also keep draining. Only a
  call with no progress permits the wait.
- **Live steps never wait.** A step is GPU work alone (the build, a decode
  from its recording to its finish, a mesh), and live, one that would wait
  while parked does not begin: the worker waits out a decode in flight,
  drops it and the record in hand, and closes the connection within a 10 ms
  slice, and unpark connects again at the newest key. Why: a parked
  player must hold no connection, and a stalled sender must not make a park
  time out, which on iOS rebuilds the view.
- **Network and protocol errors are never fatal.** They set
  `LiveStatus::last_error`, and the worker connects again about every
  second, waiting on its condition variable, so stop and park stay prompt.
  Only a GPU failure reaches `status()`. Why: `PlayerView::frame` turns
  `status()` into an error, and on iOS that drops the view. Each connection
  decodes into a new grid, as a sender may change its voxel size; the
  decoder is kept, as a key resyncs it.
- **No jitter buffer in v1.** Network jitter shows as judder. Add a bounded
  delay only if the owner sees judder.
- **Two swapchain images live** (`view_swapchain`), FIFO still. Why: with
  gfx's default, the surface's least + 1, a mesh taken waits behind one more
  queued image, a vsync longer. A file keeps the default, as its pts paces
  it.
- **The panels live:** the controls are Normals over the status line, and
  Space, Right and L do nothing. The bitrate and records a second are over
  the last second, as a live stream has no track mean: the records each
  frame stands for (`FrameStamp::records`), by when the worker published it
  rather than when it was taken, as the frame taken after a pause in drawing
  (a minimized window) stands for the whole pause. They and the status line's
  seq count records as they arrive, decoded or not, so they show what the link
  carries. `decode` is the shown record's own, and there is no `read` row,
  as the network's wait is no stage.
- **`vp_live_probe`** plays a live stream on gfx's headless surface and
  prints the panels' lines once a second: agents check a sender with it on a
  Mac, with no window. Mac only, as NVIDIA's ICD has no headless surface.
  *Grown into `vp_probe` 2026-10-23.*

## 2026-10-20 — A stats panel that a tap on its header closes

`StatsPanel`, `PlaybackStats`, and `FrameStamp`'s `bytes`, `key` and
`stages`; their headers say what each figure is.

- **What it shows:** the track's mean bitrate; the newest frame drawn, I or P,
  with its payload and triangles; the rate frames are taken for drawing, over
  the nominal; and a table of the worker's `read`, `decode` and `mesh` (host
  ms, and decode's device ms where its queue can time) and `draw`, gfx's
  `Profiler` around the whole render pass, as Apple GPUs time only at a
  pass's bounds. Why: a stream's size, its pace and where a frame's time
  goes, from what recon and gfx already count (`TrackStats`, `StageMetrics`,
  the `Profiler`).
- **The windows.** The stage means cover the last N frames taken, N the
  nominal frames a second rounded (30 when unknown, at most 240), so a pause
  holds them and a step moves them by one; the rate covers the last second of
  wall time; `draw` is the profiler's newest resolved frame. The text is
  rebuilt 4 times a second. Why: means over wall time would sink to 0 on a
  pause, and digits that change every frame cannot be read.
- **The bitrate is the track's mean:** 8 × payload bytes a record over the
  mean pts step, from the footer or the open scan, with no payload read. It
  agrees with recon's examples' `mbit_per_s` (bytes a frame × fps, over an
  encoder's GOP model) when the pts step is the nominal period and the file's
  share of I-frames is the GOP's. Why: exact and steady, and the frame line
  shows the I/P swing; a rolling figure waits for a need.
- **Decode's device time is always on:** the worker passes a `StageMetrics` to
  `decode_growing`. Why: recon's A/B on the Mac shows no cost on a real
  workload, and without timestamps the timer does nothing. A switch waits for
  an iPhone decode that looks inflated. *Replaced 2026-10-23: timed while
  the panel is open.*
- **Its own panel at the work area's top left, open at launch,** with gfx's
  collapsible header (`begin_collapsible_panel`): a tap or click on it closes
  the panel and another opens it; only the header takes input, so a finger or
  the mouse on the body orbits. Closed, nothing is rebuilt, but the counts go
  on, so it reopens with current figures; the state is not kept across
  launches. Why: grown into the controls, it would cover the bottom of a
  landscape phone and move the controls; ImGui's own collapse has a
  font-sized arrow, takes a double-click, and its window takes input over its
  whole body.
- **`StatsPanel` holds the open state and the rebuild,** rather than
  `PlayerView`. Why: a host test can then show that nothing is rebuilt while
  the panel is closed and that its figures hold for a refresh.

## 2026-10-20 — Phone sizing: the text and the targets apart

`PlayerViewConfig::ui_min_frame_height` and `kButtonEms`; the iOS shell sets
the sizes.

- **The iOS shell sets `ui_scale` 1.3 and `ui_min_frame_height` 44,** which
  the view passes to gfx's `ImGuiOverlayConfig`; the desktop keeps 1 and 0.
  Why: one scale set both: at 2 the targets were still 38 pt tall, and the
  panel was wider than a phone in portrait.
- **Play/Pause and Step are `kButtonEms` (4.5) font sizes wide,** not frame
  heights. Why: the frame height now carries the touch padding, and three of
  them would take a phone's row past 375 pt, the narrowest phone's width.
- **The status line has no ms.** Why: with the frame's decode and mesh time
  on it, the panel outgrew a 375-pt phone while playing.

**Not SwiftUI,** for the panel on iOS:

- **It would break the one copy.** gfx draws the panel in shared C++ under
  every shell (below), and SwiftUI runs only on Apple platforms: a second copy
  now, and a third with Android.
- **Its cost is in the seam.** In `volumetric_kit_ios`'s scanner, the SwiftUI
  view took the camera's touches, needed hand-written size constraints and a
  2 Hz gate on its string rebuilds, and its rows drifted between C++ and Swift
  until one side owned them.
- **The problem was sizes,** which one gfx field fixes in the shared C++.
  Revisit when the app ships to people who are not developers, for
  accessibility and a native look, or when the UI grows past an overlay: a
  stream browser, settings, text entry.

## 2026-10-20 — The iOS app, a UIKit shell over the view

`apps/ios/`; README.md says how to build it and give it a stream.

- **A thin Objective-C++ shell, all on main,** making the desktop shell's
  calls: gfx's `metal_surface` over a `CAMetalLayer` view at the screen's
  native scale, `PlayerView::create`, gfx's `ImGuiTouch` on its context, and
  a display link that calls `frame` with the drawable in pixels, at 24 to 60
  Hz: streams are 25 to 30 fps, and a ProMotion iPad would otherwise redraw
  the same frame at up to 120 Hz. Every touch phase reaches `ImGuiTouch`, a
  cancelled one as `touch_cancelled` so it presses nothing. One-finger pan
  orbits, two-finger pan pans and pinch zooms, over the view's height; the
  pan and the pinch compose, and none begins before the view exists or while
  `ui_wants_pointer()`. The shell never writes the layer's `drawableSize`,
  which MoltenVK owns (gfx's `metal_surface`). Why: one view under every
  shell (below).
- **The lifecycle:** the view is built only while the scene is in the
  foreground and active, as bring-up submits GPU work. Resign-active stops
  the display link, then parks the worker for up to a second; become-active
  unparks it and restarts the link. A park that timed out, or a `frame()`
  error, drops the view and builds a new one, from frame 0: the first at the
  next activation, the second at once, once per activation; after that a
  label says why until the next activation or a choice. Why: a park that
  timed out may have let the step under way submit from the background
  (below), and a rebuild is the only recovery from a lost device.
- **A file is the first `.vrst`, by name, in the app's Documents folder**
  (a sender is chosen on screen, 2026-10-22), which `UIFileSharingEnabled`
  and `LSSupportsOpeningDocumentsInPlace` show in Finder and the Files app;
  each build of the view looks again, and with none it leaves a note file
  there, as the Files app may hide an empty app folder. Why: the smallest
  way onto a device with no picker; a document picker and open-in wait for a
  need.
- **The `increased-memory-limit` entitlement:** past the jetsam limit iOS
  kills the app with no `Status`, while a stream too big for the device's
  Vulkan budget is refused with one. **The device family is a build
  setting,** `TARGETED_DEVICE_FAMILY` 1,2, as Xcode rewrites the plist's key,
  and an iPad letterboxes an iPhone-only app's layer; CI checks the bundle's
  key.
- **No validation on the device,** so the shell never sets
  `PlayerViewConfig::validation`: iOS has no loader, so no layer. The macOS
  legs stand in: they run every device test under validation on Homebrew's
  MoltenVK, kept at the iOS release's version.
- **Signed only when given a team** (`VP_DEVELOPMENT_TEAM`), and unsigned
  without one. The owner runs it on a device by hand (H2), as MoltenVK's iOS
  release has no simulator slice.

## 2026-10-20 — Park the worker for a suspend

`StreamPlayer::park` and `unpark`; the header says what each call does.

- **`park` holds the worker before each step** (its kernel build, a record's
  read, a frame's recording, a rebuild, a mesh) and waits for the one under
  way, a frame's batch in flight included (2026-10-25); while parked no frame
  comes due, so a control applied meanwhile takes effect at `unpark`, and
  the worker carries on from the same frame. Why: a GPU submit from the
  background loses the device on iOS (MoltenVK marks it lost for good) or
  ends the app, so a suspended app must start none. `pause` cannot stand in,
  as the worker records a frame before it is due, nor can `stop`, as `start`
  refuses after it.
- **A park that times out keeps its hold,** so at most the one step under way
  submits after it, and the caller learns that it may have.

## 2026-10-19 — An iOS build, on the core's MoltenVK set-up

`CMakePresets.json`'s `ios` preset; README.md says how to build it.

- **CMake's own iOS support, and nothing iOS-specific in the player** but
  turning off the desktop app, gfx's GLFW tier and the tests. The core links
  MoltenVK's iOS release statically and answers glm (the core's
  DECISIONS.md). Why: what every iOS consumer needs has one home.
- **CI builds the iOS app** unsigned on the Release Mac leg (Debug until
  2026-10-23), and fails unless
  it is an iOS binary (`LC_BUILD_VERSION` platform 2) that loads only the
  system's libraries. Why: only an app's link shows what an iOS link misses,
  such as MoltenVK's frameworks or a macOS glm, and a library build does not;
  a dylib outside the system would not be on the device. Xcode's default dead
  stripping stays: the app's link resolves what the app can reach.

## 2026-10-19 — The desktop window, a GLFW shell over the view

`apps/volumetric_kit_player/main.cpp`; README.md says how to run it.

- **A thin shell.** It opens the stream first, so a stream it cannot play is
  refused before `glfwInit`; then makes gfx's GLFW window and surface source,
  the view, and gfx's `ImGuiGlfw` on the view's context. Each tick polls,
  feeds the camera, and calls `frame`; a tick that draws nothing waits on
  events for up to 50 ms rather than spinning. Why: every shell makes the
  same calls.
- **`VP_NO_WINDOW` stops the app just before `glfwInit`,** naming the
  window's title, and `cli_test` sets it. Why: a refusal that moved past
  that point then fails the test on every OS, rather than open a window
  (AGENTS.md); it alone stops a valid live scenario, which nothing refuses
  before the window; and the title shows the stream or the address the app
  would play.
- **The configure fails if GLFW enters the library's link closure,** checked
  once every directory has linked. Why: the library links no window system
  (below); only the app links gfx's GLFW tier.
- **Keys are shortcuts for the panel:** Space, Right, L and N call the
  controls its buttons call; Esc closes. Why: one set of controls, which the
  panel shows on every platform.
- **The wheel accumulator is a knowing copy** of calib's and gfx's examples'
  (a TODO in `main.cpp`). Why: it shares the window's user pointer with the
  keys, and moving it to gfx means gfx owns that pointer, part of the GLFW
  input glue that calib's `calib_rig.cpp` TODO plans to move into gfx.
- **`run.frames` counts presents of a stream frame,** then the app prints
  the frame drawn, for CI's smoke run. Why: the worker builds its kernels
  first, so the window presents before the first mesh arrives, and a count
  of every present could end before one is drawn.

## 2026-10-19 — One view for every window system, drawn by gfx

How `PlayerView` shows playback; its header says what each call does.

- **The seam is gfx's `windowing::SurfaceSource`.** A shell makes one for
  its window system (GLFW's, a `CAMetalLayer`'s; the tests use the headless
  one), hands it to `PlayerView::create`, binds gfx's ImGui platform half to
  `ui_context()`, and calls `frame(extent)` each tick. The view builds the
  device recon and gfx share, and gfx draws the mesh and the controls panel.
  The library links no window system (gfx's GLFW tier is the app's). Why: one
  class under every shell, iOS links no GLFW, and the device test runs the
  real class with no window.
- **FIFO present, gfx's default,** so `view_swapchain` names no mode. Why:
  the stream's pts paces playback, not the display, so a faster mode only
  spends power; and FIFO is the one mode every surface offers.
- **The camera takes gfx's gestures.** The shells drive `OrbitCamera`'s
  `drag_orbit`, `drag_pan` and `zoom`, in viewport heights, unless
  `ui_wants_pointer()`; the player keeps the viewport: the first view,
  `kFovY` and the clip planes. Why: mouse and touch need one mapping, which
  calib's camera writes out too; in gfx it has one home.

## 2026-10-18 — Decode and mesh on one worker thread, paced by pts

How `StreamPlayer` plays a stream; its header says what each call does.

- **One worker thread does a frame's GPU work** (read, decode, mesh,
  publish), and the drawing thread only takes meshes from recon's
  `MeshExchange`. Why: drawing never waits on a decode, and recon's decoder
  and grid, which are not thread-safe, stay on one thread.
- **The worker builds its own kernels** (the decoder and the extractor)
  before its first record, and a failed build reaches `status()`. Why: their
  shader builds took seconds on the iOS scanner's first launch, and blocking
  main that long risks the watchdog's kill; the window side stays on main.
- **It publishes a frame only once the consumer has taken the last mesh,**
  rather than publishing over it. Why: each untaken mesh would hold a ring
  slot the consumer's `frames_in_flight + 2` do not cover (+1 until
  2026-10-25, when frames came to be meshed before that take). A consumer
  that stops drawing, such as a minimized window, so stops playback, and the
  clock re-anchors on the late frame.
- **The consumer says it took a mesh** (`StreamPlayer::taken`), and until
  then the worker sleeps on its condition variable. Why: recon's
  `wait_collected` polls every 2 ms, so a minimized window kept the worker
  waking 500 times a second.
- **Resync rests on the decoder.** A damaged record is skipped, and the
  decoder refuses each P-frame whose reference was skipped (`NotFound`: it
  checks the reference's hash), so playback resumes at the next key record
  with no key tracking in the player. A record this version cannot read
  (`Unsupported`) is skipped alike, as a newer writer's may be.
- **A pass that shows no frame stops the worker with its first refusal,**
  which caused the rest. Why: a refusal that lasts, such as a newer codec's
  or a frame the device cannot hold, would otherwise look like a pause with
  nothing to report, and a loop would reread the file in a busy loop.
- **With loop off, the last frame holds and play restarts,** as in a video
  player.
- **CI runs its tests under ThreadSanitizer** (the CI entry), as the worker
  shares the controls and the exchange with the drawing thread.

## 2026-10-18 — CI: build and test on the family's GPU runners, lint and the gate hosted

`.github/workflows/ci.yml` is the only workflow.

- **Build and test on self-hosted runners with a GPU,** as gfx and recon do:
  the family's Mac (label `mac`; Debug and Release, on MoltenVK) and its Linux
  GPU hosts (label `vk-linux-gpu`; Release, in `ubuntu:24.04` and
  `ubuntu:26.04` containers with `--gpus all`, on the NVIDIA driver). Why:
  every tier past the clock needs a Vulkan device, and these are the family's
  GPUs; a hosted runner has only a software one. The core's
  [runner tooling](https://github.com/taojin-6/volumetric_kit_core/blob/main/tools/runners/README.md)
  registers the runners. A job on the Mac installs nothing and fails naming
  any formula missing.
- **Lint and the `required` gate run hosted:** they are free for a public
  repository, and once the legs finish or are cancelled a hosted gate always
  runs and reports, where a self-hosted one could queue as well. A leg queued
  for an offline runner still holds the gate pending until it is cancelled.
  Branch protection on `main` is set to require that one check (context
  `required`) once it has first reported.
- **ThreadSanitizer on a third Mac leg, in Debug:** `-fsanitize=thread` in
  `CMAKE_{C,CXX,OBJCXX}_FLAGS` and the linker flags, so googletest is
  instrumented too, which the core's `VKC_SANITIZE` (carried to every target
  that links the core) does not reach. A report fails its test. MoltenVK and
  the validation layer are not instrumented; no report points into them, so
  there is no suppressions file. The leg skips the view's rerun on
  MoltenVK's specialized queue families, which are no threading variable.
- **CI leaves out the `stress` label** (`-LE stress`): the live park test
  at 50 cycles, which a plain `ctest` runs too and `ctest -L stress` selects.
  Why: each cycle is a timing check on a shared host, and 8 spread over the
  same 0-20 ms reach the same states.
- **Left out until a need shows:** Debug on Linux, as the GPU hosts are
  shared with gfx and recon; AddressSanitizer and UBSan; a dependency cache,
  which would save about 30 s a job on the Mac and could go stale after a
  brew upgrade; and the siblings' split into reusable workflows.
- **The window runs in CI only on the Linux legs, under Xvfb, in a workflow
  step:** the app plays the tests' stream for 60 presents under the
  validation layer, and a missing "presented" line, a layer the loader did
  not insert, or a `VUID-` fails the leg. Why: it is the only automated check
  of the real window, surface and present path, and NVIDIA's ICD has no
  headless surface; without the insert, a clean log would prove nothing. A
  step, never a ctest, so no agent opens a window (AGENTS.md); the Mac legs
  never open it, as their runners are daemons with no window server.

## 2026-10-18 — An application over pinned core, recon and gfx

- The player is an application, not a library: it installs and exports
  nothing. Its logic is a build-tree-only static library, so the tests link
  what the app runs, as does a project that adds the player (2026-10-25).
- It builds on core, recon and gfx, each fetched pinned by commit. The core
  is declared first, so all three share one copy (the core's DECISIONS.md).
