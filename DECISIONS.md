# Decisions

The choices made for `volumetric_kit_player`, newest first, and why.
[AGENTS.md](AGENTS.md) is the working guide; [README.md](README.md) says what
is implemented.

## 2026-10-30 — The Apple audio output: its clock and lifecycle

On macOS and iOS a file's audio plays through `open_apple_audio`
(`apple_audio_out.mm`): AVAudioEngine with an AVAudioSourceNode whose render
block pulls from the ring. Its logic is C++, `DeviceAudioOut`
(`device_audio_out.hpp`), over an `AudioDevice` seam that the `.mm`
implements and the tests fake. Video stays the master for files (the entry
below).

- **The render block neither locks, allocates, logs nor calls
  Objective-C:** it renders into a scratch allocated with the device and
  copies each channel out. The clock reaches readers through a sequence lock
  it never waits on. Why: a realtime callback that waits glitches.
- **What is heard when:** each render's host time says when its first frame
  passes the device's output; `position()` is the frames rendered, less those
  still queued and their run's latency, at the host clock's now. The latency
  is `presentationLatency`, plus `AVAudioSession.outputLatency` on iOS, read
  at each start; a start's renders before then play silence and take
  nothing. Why: `position()` is the clock that file pacing and live viewing
  can master on.
- **Each run keeps the latency of the start it began in.** Onto a route of
  longer latency the position holds at the old route's last frame until the
  new route's first is heard, and onto a shorter one it jumps ahead. Why: one
  latency for every run maps the old route's frames at the new route's, so
  speakers (10 ms) to AirPods (180 ms) stepped the position back by 170 ms.
- **Runs and the re-anchor rule** (`reanchors`): frames rendered back to back
  are one run, and each render moves the run's end to its own time, so the
  run keeps the device's rate rather than the nominal one. A flush, a device
  start, a short render, pts that do not follow on, or a time more than 1 ms
  off the run's end start a new run; the run before is kept while its frames
  may still be heard. Why: a single anchor drifts with the crystal, up to
  0.36 s an hour at 100 ppm.
- **The host clock is read at each call, never mapped through a stored
  offset to `steady_clock`.** Why: libc++'s `steady_clock` counts sleep and
  Core Audio's host time does not, so a stored offset goes stale at every
  wake; read at each call, nothing does, so no wake needs a re-measure.
- **Stopped, the position holds at what was heard as it stopped;** the
  frames the device had not played are skipped at the next start.
- **It plays while neither parked nor interrupted.** A park stops it and
  gives the route back (on iOS it deactivates the session, telling others);
  the unpark starts it, ending an interruption too. An interruption stops it
  and asks the player to pause; its end starts it again, paused, as the
  player is. A configuration change (a route or device change) starts a new
  engine and re-reads the route and latency. A device gone (on macOS no
  longer alive, on iOS the old route's device unavailable) asks the player
  to pause. A start that fails, as during a call, leaves it stopped until
  the next. Why: Apple's guidance for players, which says an interruption
  may never end and to try again once the app is active; and an engine
  whose device changed stops itself.
- **`route()` never waits for a device call:** it reads what the last start
  or stop left, under a mutex of its own; the lifecycle's mutex is held
  across AVAudioEngine's start and stop and iOS's `setActive`. Why: the
  frame thread's status line reads it every frame, under the player's mutex.
- **The system's notices run on a serial queue of the device's,** closed
  before it goes, so none reaches a destroyed output. An engine's
  configuration change counts only from the engine playing. On iOS a route
  change restarts the engine too, so where the engine's own notice comes
  first it restarts twice: a few ms more silence, and one more run.
- **The player follows a pause request once a frame**
  (`StreamPlayer::follow_audio`, from `PlayerView::frame`), pausing the
  frames and the audio together. Why: the notice comes on a system thread;
  polled on the view's, the player's mutex stays out of it.
- **iOS: playback for movies, no background audio.** The category is set
  only when it differs, as setting it posts a route change.
- **The apps choose the Apple output; tests and vp_probe the null one.**
  `StreamPlayer::open`'s default is now the null output; the desktop and iOS
  apps pass `AudioOut::open_default`, which falls back to the null output
  where no device starts. Why: no test or agent opens an audio device, and a
  Mac with no output still plays the frames.
- **Every test runs with `VP_NO_AUDIO_DEVICE` set** (tests/CMakeLists.txt),
  and the Apple output then refuses before touching a device, so
  `open_default` gives the null output. Why: a test that reached
  `open_default` started AVAudioEngine on the Mac's output, paused but open;
  like `VP_NO_WINDOW`, the variable stops what the rule alone did not.
- **Mute is a box after the others, drawn only with audio, and M on the
  desktop;** the status line ends with `muted`, `audio <n> ms` (the route's
  latency) or `audio off`, and says nothing without an Audio track.
- **Underruns are counted as renders too** (`AudioOutStats::underruns`). Why:
  live viewing will raise its delay on them.
- **`vp_audio_check` is checked by hand,** a tool beside vp_probe that ctest
  never runs: a tone at gain 0 unless asked, with the route, latency, rate,
  drift, underruns, the fit's largest residual, the position's steps back
  and the clock's runs printed. Why: the rate averages steps and jitter out,
  so it alone can read 0 ppm over a wrong mapping.

## 2026-10-30 — A file's audio: AudioOut, the null output, video the master

A file's Audio track (grid 0.4.0's `TrackKind::Audio`, raw PCM s16) plays
beside its geometry through `AudioOut` (`audio_out.hpp`), fed by
`AudioFeeder` (`audio_feeder.hpp`). The owner's audio plan, step one: the
output and the feeder, with a silent output only.

- **Audio is host I/O:** PCM in host memory, never on the GPU or in the
  per-frame GPU path; a CPU audio codec needs the owner's approval. Why:
  the OS reads and writes PCM in host memory, 96 KB/s for 48 kHz mono, and
  its only arithmetic here is s16 to f32.
- **The output is the player's,** an interface with play, pause, flush, gain
  and `position()`, the pts being heard. Why: what plays the audio and its
  clock are the player's pacing; nothing else in the family plays sound.
- **One ring between one producer and the render thread, lock-free** (an
  SPSC `AudioRing` of f32 frames). The render side neither locks nor
  allocates, and takes the producer's flush through a sequence lock it never
  waits on. Why: a device's render callback must do neither.
- **The null output, silent, renders by a clock:** steady_clock on a thread
  of its own, or a test's clock and ticks. Linux, the tests and vp_probe
  play through it (the Apple output, above, is the apps'). Why: Linux and
  CI need no sound, and tests then run the ring on two threads, under
  ThreadSanitizer too, with no device: no test opens an audio device.
- **Video stays the master.** The frames are paced as before; the audio
  follows the controls: a pause, a step and the end of the track with loop
  off pause it, so its position holds; a play plays it; each pass, a loop's
  or a play's after the end, flushes it and restarts its cursor; a mute is
  gain 0 with its clock running. Until the file's pacing is decided, the
  audio is not held to the frames: it starts at `start`, before the first
  frame, and steps, late frames and parks shift the frames alone. Why:
  which clock paces a file is decided by measuring how often frames are
  late, and one design ships then, not a switch.
- **The feeder reads 200-500 ms ahead,** on a thread of its own with a second
  cursor on the file's reader, which is safe for several. Why: the frames'
  reads stay as they were, and the lead rides out a slow read.
- **Placed by pts, not by count:** a record within one chunk (its own
  length) of the running position follows on; past it, a forward jump is
  silence and an overlap is dropped. The first record after a flush is
  placed exactly, so the audio starts at the geometry's first pts. Why: a
  mic that overflows loses samples with no gap in its count, and within a
  chunk the pts' jitter should move nothing.
- **The first Audio track on the geometry's clock that the player can
  play:** PCM s16, 8 to 384 kHz, with records; the others are skipped. The
  first two channels play; channels past the second are dropped. Why: pts
  on another clock cannot be placed against the frames'.
- **No Audio track, nothing new:** no output, no ring, no feeder thread.
- **An output or a feeder that cannot start fails `start`,** and a failed
  `start` leaves neither, so a `start` again makes its own. Why: the null
  output fails only where a thread cannot start, as the worker would.

## 2026-10-29 — A program's docked panel

`FrameHooks::panel`: a program's own panel beside the scene, on the desktop
and on iOS alike. The player holds the panel; what it shows is the
program's.

- **Docked, never floating.** The panel and the scene split the drawable,
  and the mesh, the controls, the stats and Source move into the scene's
  part, so nothing covers the mesh.
- **gfx lays it out** (`ui::DockedPanel`, as its `ui::ImagePanel` does):
  the side, the share, the handle and the header. The side is where the
  program's content (`PanelHooks::content_size`) shows larger, else the end
  of the drawable's long axis. Why: the owner's choice of an automatic side
  over fixed sides per device, and one home for a docked panel's rules.
- **40 % of the safe area, 45 % on an iPhone, held to 25-60 % and 240 pt
  across; an iPhone's handle snaps to 45 % and 60 %, and a release below
  25 % closes.** Why: the scene keeps at least 40 %; on an iPhone the owner
  chose two sizes over a free drag.
- **Pointer input goes where a press began, kept until it ends**
  (`PointerTarget`). Why: a drag that crosses the panel's edge keeps its
  target.
- **Closed, nothing of it runs**: the mesh is drawn across the whole target
  and no hook of the panel is called. Why: a program pays only while its
  panel is open.
- **The state lasts the run,** carried across the iOS shell's view rebuilds
  as Deblock is; nothing is saved across launches.

## 2026-10-29 — The light follows the viewer

The view lights the mesh with a headlight (`headlight`): each frame,
`record_mesh` sets gfx's `HybridMeshFrame::light_dir` from the camera's view.
The desktop app, the iOS app and vp_probe all draw through `frame`, which
calls it, so all three light the mesh alike.

- **Not gfx's default.** Why: that light, (0.5, 0.8, 0.6), is fixed in the
  world and takes +y as up. A rig's world is its first camera's own frame (+y
  down, +z forward), so the light sat below and behind the room: what faced
  the cameras had gfx's ambient alone, flat grey, and only what faced away
  showed shading (the owner, 2026-10-29: "the shading seems flipped, only
  back surface got shaded visibly"). The mesh was right: on the fused room
  mesh, the vertex normals agree with the winding on 98.5% of faces. A light
  tied to the view lights what faces the viewer in any world, from any side
  the orbit takes.
- **From just above and left of the eye,** (-0.3, 0.4, 1) in view space,
  about 27° off the line of sight, not on it. Why: a light on the line of
  sight lights every surface square to it alike; off it, a surface facing
  the viewer is lit to 0.89 of full, one tilted toward the light to full and
  one tilted away less, so relief shows. Above and left is where a viewer
  expects light from.
- **The player's own for now; gfx's next.** A view-relative light belongs in
  gfx's mesh pipeline, where recon's rig_viewer, which makes its own, would
  share it. Why not yet: moving it takes a gfx PR and a re-pin, and this
  fixes the player first; the gfx task follows (the TODO in
  player_view.cpp).

## 2026-10-29 — A program's stats text of several lines

`FrameHooks::stats_line`'s text may hold several lines, separated by `\n`,
each shown as its own. It amends "One line" in "A program's line in the
stats" below.

- **The program breaks its text, not the panel.** Why: a program whose
  figures do not fit one line groups them as they read best; the panel still
  breaks a line too wide for its room at its ` · ` (2026-10-26).
- **Compact, every line over the renderer's table,** as the one was. On the
  view tests' narrowest phone in portrait three lines fit; on the shortest
  notched one in landscape three take the stats 36 pt past the controls
  (MoltenVK on an M5 Max), so a program for that phone keeps to one.

## 2026-10-29 — A program's device requirements

For the device the worker decodes and meshes on, in the view and in vp_probe
alike. It amends "Built on grid, not recon" below.

- **A program states what its hooks need of the device beyond grid's,** in
  the config that carries its hooks (`PlayerViewConfig::device_requirements`).
  Why: the player makes the device, and a device gains no extension once
  made.
- **The core's `merge` joins them to grid's** (`worker_requirements`). Why:
  it is a union, so a program's can only add.
- **They go on the shared device's compute side,** with grid's. Both sides
  adopt one `VkDevice`, so the worker's device and the draw's enable the
  same.

## 2026-10-29 — CI: no outsider's trigger, no token kept, images by digest

The core's CI does the same since its #68 (its DECISIONS.md, 2026-10-28,
"Setup checks who can trigger CI"), which says why.

- **The lint job runs the core's `check-workflow-triggers` action,** which
  fails on `pull_request_target`, `workflow_run`, the comment and discussion
  events, and `schedule`. It is taken at the core the build pins, as the
  core asks of a sibling.
- **No checkout keeps a token** (`persist-credentials: false`): no job
  pushes.
- **The Linux legs' images are pinned by digest,** the tag in a comment, so
  a new image comes only with a commit that moves the pin.

## 2026-10-29 — Built on grid, not recon

recon's `grid/` is now
[volumetric_kit_grid](https://github.com/taojin-6/volumetric_kit_grid)
(recon #309), a repository of its own on the core alone: the voxel grid,
Marching Cubes and the mesh types, mesh files, the codec, the `.vrst`
container and the live stream's server and client. Everything the player
took from recon is there, and none of what stayed in recon (its sensors,
fusion and texturing), so the player fetches core, grid and gfx, and no
recon.

- **The same code under grid's names, with no aliases.** grid's tiers are
  recon's at the player's last pin (#299's merge, d8b8bda), moved and
  renamed: headers `volumetric_kit/grid/<tier>/`, namespaces
  `volumetric_kit::grid::<tier>`, targets `volumetric_kit::grid_<tier>`, the
  PLY writer in `mesh_io` and the mesh integrator in `voxelize`. The `.vrst`
  format, the decode and the meshes are unchanged: on the home RTX 5090 the
  tests' streams that main and this change write are the same bytes, and the
  meshes each decodes from them the same triangles, vertex for vertex (their
  order varies from run to run on either). Why: a re-pin is then a rename,
  and a forwarding name would be one more to retire.
- **grid is declared after the core and before gfx, and checked with
  `vkc_require_sibling(volumetric_kit::grid <version> TARGETS …)`** at the
  version the player needs, its tests and examples off. Why: the core comes
  first so every sibling shares it (2026-10-18), and the check (core
  0.27.0's) refuses, at configure time, an older grid or an override without
  a tier the player links.
- **The device asks for grid's requirements:** `scalarBlockLayout`, without
  the external-memory and Metal-object extensions recon asked for, where
  offered, for its camera decoders. Why: the player never used them.
- **The measure log names grid's commit, as format 4.** The build stamp
  (`vp_build_info`) and the runner's sibling lists name core, grid and gfx;
  the summarizer still reads formats 1 to 3, which name recon's. Why: a log
  names the code that ran.
- Earlier entries say recon for what is now grid's; their decisions stand.

## 2026-10-27 — A program's line in the stats

`FrameHooks::stats_line`, a function that returns a short text the stats
panel shows as its last line. Why: a program built on the player has figures
of its own, a rate it counts for one, that belong beside the player's, on the
desktop and on iOS alike.

- **On `FrameHooks`, not `PlayerViewConfig`.** Why: `make_hooks` makes the
  program's state, so a line made there reads it; a config field is set
  before that state exists. A program that sets `make_hooks` for the line
  alone pays one more mesh in the ring (`mesh_wiring`'s `holds`).
- **A line of its own, the last; compact, over the renderer's table.** Why:
  on the view tests' shortest notched phone in landscape, a file's stats end
  8 pt above the controls, the worker's column (194 pt) the taller, so a text
  added to its bitrate line took them over the controls once it broke the
  line, from 20 characters on; the renderer's column (172 pt) has room for
  one more row, and a live view's stats end 29 pt above the controls.
- **Up to 44 characters, in parts of up to 35 between its ` · `.** Why: on
  that phone in landscape, file or live, such a line keeps the stats 7 or
  8 pt above the controls and within the panel's widest; a longer one widens
  the renderer's column past it, so its end is cut off, and breaking it would
  take a row there is no room for. On the narrowest phone in portrait the
  line breaks at its ` · `, and a part past 35 characters on an indented row
  is cut off. (Sweeps on the home RTX 5090 at the iOS shell's sizes; the view
  tests check a line at both lengths.)
- **Read at a rebuild once a second has passed since its last read**
  (`StatsPanel::kProgramLineRefresh`), only while the panel is open. Why: a
  rate the program counts since its last read is then over about a second
  while the panel stays open, as the live bitrate is, with no history kept;
  the first read after the panel opens, or after frames stop, covers all the
  time since the read before. Closed, nothing is rebuilt (2026-10-20).
- **One line.** A second waits for a program that needs it. *Several
  since 2026-10-29.*

## 2026-10-27 — Texture: the mesh with or without its binding's texture

`PlayerView::textured`, the panel's Texture box and the desktop's T key:

- Off at start, so the mesh starts in its own shading (the owner: "I think
  default is mesh shading").
- Off leaves out the whole of a binding that names a set, as its clears go
  with its texture: the frame is then the one a program with no binding
  draws.
- before_draw still runs while off, so its program keeps its state and
  Texture on shows the texture at once.
- The box shows only once a set has been bound, so a program that binds none
  has no box that does nothing.

## 2026-10-26 — after_mesh's batch finishes before the grid grows

recon #295 makes `VoxelHashMap::record_compaction` const, so an `after_mesh`
hook, handed the grid read-only, may record work that reads it, such as a
compaction of its map. That batch binds the map's own kernel sets, which a
grow (`VoxelBlockGrid::resize`) rewrites, and the core then refuses the batch
at its wait: recon's rule is no grow until the batch has resolved. The worker
finished the batch only at the next `after_mesh`, after the next frame's
record and finish, either of which may grow the grid (`codec::record_growing`,
`finish_growing`), so the first grow stopped playback with that refusal.

- **The worker finishes the batch before anything may grow the grid:**
  - At the next frame's finish, once that frame's batch has resolved and
    before `finish_growing`, which grows the grid when the table cannot place
    the frame; a live record's decode alike. The hook's batch went to the
    same queue before that frame's, so it has run: the wait finds it
    complete, and the core counts no host wait. It is timed in the frame's
    "wait: GPU" row, so a wait that did block would show there. On a steady
    frame this is the finish the next `after_mesh` did, a step earlier:
    still one submit more a frame and no blocking wait.
  - Before recording a frame of more blocks than the grid's heap
    (`codec::read_frame_info`'s `block_count` against the grid's
    `num_blocks`), the frame `record_growing` grows the grid for before it
    records. This wait can block, on that frame alone, inside its "decode"
    row: playing, the frame is recorded as soon as the one before is
    published, whose batch may still run. The grow then waits for the
    queue anyway, so the frame loses only what of that batch's run had
    overlapped the host's work before the grow's wait (measured below).
  - As before, the next `after_mesh`, a park, a stop, a frame's stamp
    unpublished at a stop, and a live connection's next grid finish it too.
- **Not before every record:** playing, the next frame is recorded right
  after the publish, while the hook's batch may still run, so that would be
  a blocking wait each frame, which the hooks' entry rules out.
- **The block count is recon's contract:** `record_growing` grows only for a
  frame of more blocks than the grid's heap, and `FrameInfo::block_count` is
  what a grid decoding the frame needs; reading it costs a header's parse,
  where a hook into recon's grow would be API for one caller.
- **Measured** with `vp_probe --worker` and no validation, main against
  this change, run by run in turn, on `outgrow.vrst`'s grown frame. The
  hooks are `--latency`'s, whose batch records nothing, and
  `--save-meshes`', which reads the mesh back.
  - Apple M5 Max, 30 timed runs each: with the readback, the new wait
    blocked in 28 runs, 0.40 ms at the median, and the grow's own wait fell
    by about 0.15 ms, so the frame blocked 4.01 ms on average, against
    main's 3.78. With the empty batch it never blocked.
  - RTX 5090, 40 untimed runs each: the frame blocked 1.04 ms on average,
    as on main, with the readback, and 1.08 against main's 1.03 with the
    empty batch. Timed, the new wait blocked in 3 of 60 runs, at most
    0.74 ms.
  - Steady frames (`sphere.vrst`, passes 2 to 6, `--latency`): no wait
    blocked in either build on either device.
- **Tests:** a hook that records a compaction through the grid it is handed
  and reads the count back, over the tests' grow stream, which grows the
  grid at a frame's finish, and a new outgrow stream (`outgrow.vrst`), whose
  frame `kGrownFrame` has more blocks than the first grid's slots, so it
  grows the grid at its record: stepped, played, after a rebuild, and live,
  every frame comes and each count is its frame's blocks, under the
  validation layer and its synchronization validation. Without the finish at
  a frame's finish, or at the record, its tests fail with the core's refusal.

## 2026-10-26 — The desktop app and vp_probe are library calls

`desktop.hpp` (`run_desktop`, target `player_desktop`) and `probe.hpp`
(`run_probe`, target `player_probe`), each taking an `AppConfig`
(`app_config.hpp`); the app's main and vp_probe's are one call each. It
amends 2026-10-25's library entry.

- **A program of its own runs either with the view's settings it chooses,
  `make_hooks` included,** with no copy of the window loop or the probe.
  Why: per-frame hooks (below) otherwise reach only a program that copies
  one of them.
- **The command line and the scenario set over `AppConfig::view`:** the app
  the scenario's `window.validation` (on in Debug, as before), vp_probe its
  options' validation, timing, tile depth, labels and vertex sharing. Why:
  what a run asks for wins over what the program built in.
  `desktop_view_config` and `probe_view_config` give the result on the host,
  so a test checks it.
- **vp_probe's `--latency` and `--save-meshes` install their hooks as
  `make_hooks`,** as a program's own would, and exit 2 when the program set
  it; the log's `run` names a program's hooks, a batch more a frame as
  theirs are. Why: the view takes one `make_hooks`, and no caller needs two
  sets of hooks composed.
- **`VP_DESKTOP_APP` defaults to `VP_BUILD_APPS`, and a project that adds
  the player may turn it on alone:** it builds `player_desktop` and gfx's
  GLFW tier, and the app only with `VP_BUILD_APPS`. Why: that project's
  program is then the window. The library still links no GLFW, nor does
  `player_probe`; the configure checks both.
- **`player_probe` is defined on every desktop, built only for a target
  that links it,** with the core's test policy, which the player now asks
  the core for on every desktop (built likewise). Why: vp_probe validates as
  the tests do, and a project that never links it pays nothing. Neither
  target is built for iOS, whose shell stays in `apps/ios/`.
- **The subproject consumer test links both:** with its defaults it builds
  the library and `player_probe` and no GLFW; configured again with
  `VP_DESKTOP_APP` on, `player_desktop` too; neither the app nor `vp_probe`.
- **CI's window step also runs `vp_desktop_hooks`,** a program of the tests'
  that calls `run_desktop` with hooks counting their calls, and fails the
  leg unless they were made once and ran for a mesh and a draw. Why: a test
  never opens a window, so nothing else shows a program's `make_hooks`
  reaching the window's view.

## 2026-10-26 — Per-frame hooks for the worker and the draw

`frame_hooks.hpp`: `FrameHooks`, made once by `PlayerViewConfig::make_hooks`
from a `HookContext`. Why: a caller adds GPU work to a frame and chooses what
each display frame draws, a readback or a measurement for one, with no copy
of the worker or the view; the panel's Unlit, `vp_probe --latency` and
`--save-meshes` are such callers.

- **Where each runs:** `after_mesh` on the worker before every publish, a
  frame's, one meshed again after a redo, a paused toggle's rebuild and a
  live frame's, with the grid the mesh was made from (read-only); `commit`
  inside the take (recon's `begin_frame` commit), on the drawing thread;
  `before_draw` on that thread before the render pass, each display frame
  that draws a mesh, with the shown frame's stamp; `on_park` in a park that
  returns true, once until the unpark, which runs `on_resume`. A failure
  stops playback, or the view, with its status.
- **No hooks, one batch a frame, as 2026-10-25's.** `after_mesh` records
  into a batch of its own, submitted behind the mesh's ready point, whose
  point the published mesh then carries; the worker finishes it at the next
  `after_mesh`, when the GPU has run it, so it adds a submit a frame and no
  blocking host wait. *Since 2026-10-26 it finishes it earlier, before the
  grid may grow ("after_mesh's batch finishes before the grid grows").* A
  park and a stop finish it too, so its readbacks are written before they
  return and nothing runs while parked; so do a stop before a frame's stamp,
  and its extra, goes unpublished, and a live connection before the next
  one's grid replaces the grid the batch was given. A park waits for it
  within its timeout, as for the step under way, and returns false, the hold
  kept and no `on_park`, if it still runs, so a suspend's park on the main
  thread stays bounded.
- **A mesh is committed only once its ready value is reached.** gfx's frame
  loop waits only for values reached (a present behind an unreached one is
  invalid), and `after_mesh`'s batch may still run at the take, so the take
  holds the mesh, as recon's park does, without asking the hook, rather than
  a host wait on the drawing thread.
- **Hold and skip are recon's park and drop** (`CommitDecision`, recon #293).
  The take tells the worker once a frame, held or not (`taken`). At most one
  frame is skipped between two drawn, as a dropped mesh keeps its slot until
  the next commit: a second skip holds the frame instead, and with hooks the
  ring is one mesh deeper (`mesh_wiring`'s `holds`: a vertex arena more). A
  hold needs none, as the worker waits behind a held mesh.
- **`FrameStamp::extra` lives until the GPU has drawn its frame for the last
  time:** each slot keeps the extra of the frame it drew until its next take,
  which waits for that slot's last frame. `FrameTaker` does this for the view
  and for `vp_probe --worker` alike, so the commit's handling is tested where
  no view runs, NVIDIA included.
- **Unlit is the view's own `DrawBinding`,** clearing `kHybridMeshLit`;
  Normals takes precedence. A box after Deblock, live too, and the U key.
- **`PlayerView::record_mesh` records the mesh as the last frame drew it,**
  and `frame` draws through it: the swapchain's images cannot be read back,
  so a test draws it into an offscreen target of `target_layout` and reads
  its pixels.
- **`mesh_wiring` counts one frame in flight as two,** as recon's ring rule
  does (the mesh a commit replaces keeps its slot until the committing frame
  retires): unchanged for the view's two.

## 2026-10-26 — The frame's set on show and in the log; vertex sharing a switch

- **Each frame carries its record's capture set** (`FrameStamp::set`, from
  recon's `RecordInfo::set`), and live the newest record's too
  (`LiveStatus::set`); `kNoSet` when the record names none, and a rebuild
  keeps its frame's. The status line shows "set N" after the frame, only for
  a frame that has one. Why: the set names the capture a frame came from,
  as its sender numbered it, so a frame on screen or in a log can be traced
  to that capture.
- **A status or stats line too wide for the work area breaks between its
  parts,** each row as many as fit and the rows after the first indented.
  Where all but the last part fit, that is 2026-10-24's break before the
  last. Why: with its set, a file's status line passes a phone's portrait
  width.
- **Measure format 3:** the header's `share_vertices` and the frame row's
  `vertices` (its mesh's) and `set` (empty for none). The summarizer reads
  formats 1 to 3, shows the setting, and the vertices per frame. Why: an A/B
  of the switch below needs the setting and the vertices in the log, and the
  set ties a logged frame to its capture.
- **Vertex sharing is a switch, on by default**
  (`PlayerViewConfig::share_vertices`, through `mesh_wiring`; vp_probe's
  `--share-vertices`, the iOS measure launch and the runner): off, recon's
  extractor gives each triangle three vertices of its own. Measured on
  rafa2.vrst (vp_probe, timing all, two passes after a warm-up): off, a
  frame holds 3.48 times the vertices (589,101 against 169,276 on average),
  and on the M4 the view's draw takes 0.75 ms of device time against 0.40;
  the extraction itself is cheaper, the mesh row 0.13 ms against 0.21 on the
  lab's RTX 4090 and 1.84 against 2.55 on the M4, its decode row the same
  either way. On stays the default: the draw is indexed, so it takes either,
  and on keeps the ring's vertex arenas and each draw's fetches to under a
  third. Off pays where the extraction, not the draw, bounds a frame; the
  switch lets a measurement on each device say which.
- **Phone runs require the log to confirm unshared vertices** when
  `--share-vertices off` is requested. Why: older apps silently ignore
  unknown launch options and still share vertices; a successful launch or
  a format-2 log cannot establish that sharing was disabled. A missing or
  different setting fails before summarizing, retaining the raw log for
  diagnosis; default sharing still permits older refs.

## 2026-10-26 — An interrupted view must be recreated

`PlayerView::create` and `frame` contain host exceptions inside their Result
boundaries. Allocation failures return `OutOfMemory`; other exceptions return
`InvalidArgument`. An exception during a frame can leave ImGui or command
recording incomplete, so the view refuses subsequent ticks before another
acquire. The caller destroys and recreates the view to resume. Destruction
stops the worker and drains submitted work before releasing its resources.
This makes the failure explicit without submitting incomplete commands.

## 2026-10-25 — Another project can build on the player's library

- **A project can add the player (`add_subdirectory`) and link
  `volumetric_kit::player`,** as core, recon and gfx are added:
  `VP_BUILD_APPS`, `VP_BUILD_TESTS` and `VP_WARNINGS_AS_ERRORS` default ON
  only when the player is the top-level project. Why: the project that adds
  it then builds the library alone, with no GLFW and under its own
  compiler's warnings.
- **`VP_DESKTOP_APP` stays, under `VP_BUILD_APPS`,** which turns it off. Why:
  the measurement runner passes it to every commit it builds, older ones
  too. *Since 2026-10-26 it defaults to `VP_BUILD_APPS` and may be on
  without the apps.*
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
  before the mesh is published (with an `after_mesh` hook, before it is
  committed: 2026-10-26), so it stalls nothing; it carries the worker's
  writes across queues, where the fence alone did not.
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
  (below); only the app links gfx's GLFW tier. *Through `player_desktop`
  since 2026-10-26.*
- **Keys are shortcuts for the panel:** Space, Right, L and N call the
  controls its buttons call; Esc closes. Why: one set of controls, which the
  panel shows on every platform.
- **The wheel accumulator is a knowing copy** of calib's and gfx's examples'
  (a TODO in `desktop.cpp`). Why: it shares the window's user pointer with the
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
  never open it, as their runners are daemons with no window server. *Since
  2026-10-26 a program with hooks runs there too.*

## 2026-10-18 — An application over pinned core, recon and gfx

- The player is an application, not a library: it installs and exports
  nothing. Its logic is a build-tree-only static library, so the tests link
  what the app runs, as does a project that adds the player (2026-10-25).
- It builds on core, recon and gfx, each fetched pinned by commit. The core
  is declared first, so all three share one copy (the core's DECISIONS.md).
