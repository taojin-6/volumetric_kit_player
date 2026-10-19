# Decisions

The choices made for `volumetric_kit_player`, newest first, and why.
[AGENTS.md](AGENTS.md) is the working guide; [README.md](README.md) says what
is implemented.

## 2026-10-19 — One view for every window system, drawn by gfx

How `PlayerView` shows playback; its header says what each call does.

- **The seam is gfx's `windowing::SurfaceSource`.** A shell makes one for
  its window system (GLFW's, a `CAMetalLayer`'s; the tests use the headless
  one), hands it to `PlayerView::create`, binds gfx's ImGui platform half to
  `ui_context()`, and calls `frame(extent)` each tick. The view builds the
  device recon and gfx share, and gfx draws the mesh and the controls panel.
  The library links no window system (gfx's GLFW tier stays off). Why: one
  class under every shell, iOS links no GLFW, and the device test runs the
  real class with no window.
- **FIFO present.** Why: the stream's pts paces playback, not the display,
  so a faster mode only spends power; and FIFO is the one mode every surface
  offers.
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
- **It meshes a frame only once the consumer has taken the last mesh,**
  rather than publishing over it. Why: each untaken mesh would hold a ring
  slot the consumer's `frames_in_flight + 1` do not cover. A consumer that
  stops drawing, such as a minimized window, so stops playback, and the clock
  re-anchors on the late frame.
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
  there is no suppressions file.
- **Left out until a need shows:** Debug on Linux, as the GPU hosts are
  shared with gfx and recon; AddressSanitizer and UBSan; a dependency cache,
  which would save about 30 s a job on the Mac and could go stale after a
  brew upgrade; and the siblings' split into reusable workflows.
- **The window is build-only in CI:** the Mac's runners are daemons with no
  window server, and no test opens a window (AGENTS.md).

## 2026-10-18 — An application over pinned core, recon and gfx; the input is a path

- The player is an application, not a library: it installs and exports
  nothing. Its logic is a build-tree-only static library, so the tests link
  what the app runs.
- It builds on core, recon and gfx, each fetched pinned by commit. The core
  is declared first, so all three share one copy (the core's DECISIONS.md).
- Like a video player, it takes the path of the stream to play and nothing
  else, so it reads no core config.
