# Decisions

The choices made for `volumetric_kit_player`, newest first, and why.
[AGENTS.md](AGENTS.md) is the working guide; [README.md](README.md) says what
is implemented.

## 2026-10-18 — CI: build and test on the family's GPU runners, lint and the gate hosted

`.github/workflows/ci.yml` is the only workflow.

- **Build and test on self-hosted runners with a GPU,** as gfx and recon do:
  the family's Mac (label `mac`; Debug and Release, on MoltenVK) and its Linux
  GPU hosts (label `vk-linux-gpu`; Release, in an `ubuntu:24.04` container
  with `--gpus all`, on the NVIDIA driver). Why: every tier past the clock
  needs a Vulkan device, and these are the family's GPUs; a hosted runner has
  only a software one. The core's
  [runner tooling](https://github.com/taojin-6/volumetric_kit_core/blob/main/tools/runners/README.md)
  registers the runners. A job on the Mac installs nothing and fails naming
  any formula missing.
- **Lint and the `required` gate run hosted:** they are free for a public
  repository, and a hosted gate reports even while the self-hosted runners
  are down. Branch protection on `main` requires that one check, whose
  context is `required`.
- **Left out until a need shows:** Debug on Linux, as the GPU hosts are
  shared with gfx and recon; a second Ubuntu; sanitizers, the first to add,
  for the decode worker's lifetimes; a dependency cache, which would save
  about 30 s a job on the Mac and could go stale after a brew upgrade; and
  the siblings' split into reusable workflows.
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
