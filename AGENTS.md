# Shared repository instructions

This is the working guide for Codex, Claude Code and other agents.
`CLAUDE.md` imports it with `@AGENTS.md`; keep shared rules here only.

## Project and boundaries

`volumetric_kit_player` is an application: it plays a recorded `.vrst`
stream like a video player, decoding and meshing each frame on the GPU and
drawing it in a window. [README.md](README.md) says what is implemented;
[DECISIONS.md](DECISIONS.md) records why.

- It is assembled from its siblings, each fetched pinned by commit:
  [`volumetric_kit_core`](https://github.com/taojin-6/volumetric_kit_core)
  (the device, `Status`/`Result`, the test fixtures),
  [`volumetric_kit_recon`](https://github.com/taojin-6/volumetric_kit_recon)
  (the stream reader, the codec, Marching Cubes) and
  [`volumetric_kit_gfx`](https://github.com/taojin-6/volumetric_kit_gfx)
  (the window, the mesh pipeline, the camera). From the primary checkout
  they are `../volumetric_kit_<name>/`.
- Code another application could use belongs in its owner, as a task in that
  repository. The player keeps only its assembly and its playback.
- Processing runs on the GPU, on one `VkDevice` that recon and gfx share. The
  host reads the file's bytes and handles input, with no host fallbacks.

## Layout and naming

- `src/volumetric_kit/player/`: the logic, as the static, build-tree-only
  library `volumetric_kit::player`. Headers sit beside their sources, as the
  app exports nothing.
- `apps/volumetric_kit_player/`: the desktop app, the one target that links
  GLFW.
- `tests/<name>_test.cpp`, each added with `vp_add_test` in
  `tests/CMakeLists.txt`; `tests/ios_link_check/`, the iOS build's app,
  linked and never run.
- Namespace `volumetric_kit::player`; `VP_*` for the player's macros and
  CMake options. The siblings' names stay theirs.

## Conventions

- C++17 with no compiler extensions, built with GCC or Clang on Linux and
  macOS, and for iOS. Add no `_WIN32`/`_MSC_VER` paths or MSVC flags.
- Doxygen on every class and function in a header: `@file`/`@brief` on the
  header, a `@code` example per class, `@param`/`@return`/`@pre` on methods.
- Fallible calls return the core's `Status` or `Result<T>`, propagated with
  `VKC_TRY`/`VKC_ASSIGN`; no exception crosses a function.
- Include Vulkan through the core's `volumetric_kit/core/vulkan/vulkan.hpp`,
  never `<vulkan/...>` (a pre-commit hook checks).
- Mark deferred work with a greppable `// TODO:`.

## Tests

- Every change carries unit tests for what it changes.
- **Tests and agents never open a window.** Nothing they run may reach
  `glfwInit`: a test may run the app only on input it refuses first, with
  `VP_NO_WINDOW` set, and agents skip gfx's `GlfwNativeWindowTest`. Only
  CI's Xvfb step on the Linux legs runs the window; the owner checks it by
  hand.
- Logic is tested headless: host-only where it can be, otherwise on a device
  through the core's fixtures, which skip without one locally and fail in CI.
- Run GPU tests under the validation layer: `VKC_TEST_VALIDATION=1`, and on
  macOS `DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib` so the layer loads.

## Working with Git

- Implement each task in its own branch and git worktree under `.worktrees/`.
  Preserve other worktrees and local changes; remove your worktree with
  `git worktree remove` after its PR merges.
- Use absolute paths for `git -C`, `cmake -S/-B` and file operations.
- Use Conventional Commits, e.g. `feat: …`, `fix: …`, `ci: …`.
- Open PRs with `gh pr create --assignee @me`. On an open PR, add commits and
  push normally; never force-push, rebase or amend.

## Build and validation

Build and test from the task's worktree as
[README.md](README.md#build-and-test) shows, then run
`pre-commit run --all-files --show-diff-on-failure` and `git diff --check`.
CI (`.github/workflows/ci.yml`) builds and tests on the family's self-hosted
Mac and Linux GPU runners, runs the hooks on a hosted runner, and its
`required` job is the merge gate.

## Keeping guidance current

Keep this guide short. Put rationale in DECISIONS.md, and update a decision in
the same commit as the code it changes.
