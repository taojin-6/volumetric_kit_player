# Decisions

The choices made for `volumetric_kit_player`, newest first, and why.
[AGENTS.md](AGENTS.md) is the working guide; [README.md](README.md) says what
is implemented.

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
  a display link that calls `frame` with the drawable in pixels. Every touch
  phase reaches `ImGuiTouch`, a cancelled one as `touch_cancelled` so it
  presses nothing. One-finger pan orbits, two-finger pan pans and pinch
  zooms, over the view's height; the pan and the pinch compose, and none
  begins before the view exists or while `ui_wants_pointer()`. The shell
  never writes the layer's `drawableSize`, which MoltenVK owns (gfx's
  `metal_surface`). Why: one view under every shell (below).
- **The lifecycle:** the view is built only while the scene is in the
  foreground and active, as bring-up submits GPU work. Resign-active stops
  the display link, then parks the worker for up to a second; become-active
  unparks it and restarts the link. A park that timed out, or a `frame()`
  error, drops the view and builds a new one, from frame 0: the first at the
  next activation, the second at once, once per activation; after that a
  label says why until the next activation. Why: a park that timed out may
  have let the step under way submit from the background (below), and a
  rebuild is the only recovery from a lost device.
- **The stream is the first `.vrst`, by name, in the app's Documents
  folder,** which `UIFileSharingEnabled` and
  `LSSupportsOpeningDocumentsInPlace` show in Finder and the Files app; each
  activation looks again, and with none it leaves a note file there, as the
  Files app may hide an empty app folder. Why: the smallest way onto a
  device with no picker; a document picker and open-in wait for a need.
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
  read and decode, an extract) and waits for the one under way; while
  parked no frame comes due, so a control applied meanwhile takes effect at
  `unpark`, and the worker carries on from the same frame. Why: a GPU submit
  from the background loses the device on iOS (MoltenVK marks it lost for
  good) or ends the app, so a suspended app must start none. `pause` cannot
  stand in, as the worker decodes a frame before it waits for its due time,
  nor can `stop`, as `start` refuses after it.
- **A park that times out keeps its hold,** so at most the one step under way
  submits after it, and the caller learns that it may have.

## 2026-10-19 — An iOS build, on the core's MoltenVK set-up

`CMakePresets.json`'s `ios` preset; README.md says how to build it.

- **CMake's own iOS support, and nothing iOS-specific in the player** but
  turning off the desktop app, gfx's GLFW tier and the tests. The core links
  MoltenVK's iOS release statically and answers glm (the core's
  DECISIONS.md). Why: what every iOS consumer needs has one home.
- **CI builds the iOS app** unsigned on the Debug Mac leg, and fails unless
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
- **`VP_NO_WINDOW` stops the app just before `glfwInit`,** and `cli_test`
  sets it. Why: a refusal that moved past that point then fails the test on
  every OS, rather than open a window (AGENTS.md).
- **The configure fails if GLFW enters the library's link closure,** checked
  once every directory has linked. Why: the library links no window system
  (below); only the app links gfx's GLFW tier.
- **Keys are shortcuts for the panel:** Space, Right and L call the controls
  its buttons call; Esc closes. Why: one set of controls, which the panel
  shows on every platform.
- **The wheel accumulator is a knowing copy** of calib's and gfx's examples'
  (a TODO in `main.cpp`). Why: it shares the window's user pointer with the
  keys, and moving it to gfx means gfx owns that pointer, part of the GLFW
  input glue that calib's `calib_rig.cpp` TODO plans to move into gfx.
- **`--frames N` counts presents of a stream frame,** then prints the frame
  drawn, for CI's smoke run. Why: the worker builds its kernels first, so the
  window presents before the first mesh arrives, and a count of every present
  could end before one is drawn.

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
- **The window runs in CI only on the Linux legs, under Xvfb, in a workflow
  step:** the app plays the tests' stream for 60 presents under the
  validation layer, and a missing "presented" line, a layer the loader did
  not insert, or a `VUID-` fails the leg. Why: it is the only automated check
  of the real window, surface and present path, and NVIDIA's ICD has no
  headless surface; without the insert, a clean log would prove nothing. A
  step, never a ctest, so no agent opens a window (AGENTS.md); the Mac legs
  never open it, as their runners are daemons with no window server.

## 2026-10-18 — An application over pinned core, recon and gfx; the input is a path

- The player is an application, not a library: it installs and exports
  nothing. Its logic is a build-tree-only static library, so the tests link
  what the app runs.
- It builds on core, recon and gfx, each fetched pinned by commit. The core
  is declared first, so all three share one copy (the core's DECISIONS.md).
- Like a video player, it takes the path of the stream to play, so it reads
  no core config.
