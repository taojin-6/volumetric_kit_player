// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file probe.hpp
/// @brief vp_probe as a library call, in the target
///        `volumetric_kit::player_probe`, which links the core's test policy:
///        `tests/vp_probe.cpp` is one call of @ref run_probe. Not for iOS.

#include "volumetric_kit/player/app_config.hpp"

namespace volumetric_kit::player {

/// @brief vp_probe: a file or a sender's stream played with no window,
///        through the view on gfx's headless surface or by the worker alone
///        (`--worker`), for a GPU with no headless surface.
///
/// `vp_probe (--file PATH | --connect HOST[:PORT]) [--frames N] [--passes P]
/// [--timing off|sampled|all] [--tile-depth 0|1] [--labels on|off]
/// [--share-vertices on|off] [--csv PATH] [--latency] [--save-meshes DIR]
/// [--worker]` plays a .vrst file or a sender's live stream through
/// PlayerView on gfx's headless surface, at 60 ticks a second, and prints the
/// status line, the stats lines and the stage tables once a second. With
/// `--worker` it runs the worker alone, with no view, on the GPU the core
/// picks (a discrete one first), its meshes taken as a renderer takes them
/// (FrameTaker): for a GPU with no headless surface, as NVIDIA's has none.
/// With `--csv` it logs each stream frame taken (measure_log.hpp), the file
/// appearing at PATH once the run ends. With `--latency`, hooks
/// (frame_hooks.hpp) time each frame from its mesh finished on the worker to
/// its commit and to each draw of it, printed once a second, and the run's
/// last line says how many of their records are left unreleased. With
/// `--save-meshes`, each mesh is read back in its after_mesh batch and
/// written as DIR/<frame>.ply once that batch has finished, a line each. Both
/// install their hooks as `config.view.make_hooks`. A file plays P passes of
/// its track, 1 unless given; the run ends once N stream frames are taken,
/// or once the last pass ends, and never on a network error, which the
/// status line shows. It validates in Debug and as the core's test policy
/// asks (`VKC_TEST_VALIDATION`). The measurement runner's (tools/measure/,
/// DECISIONS.md), and agents'.
///
/// The view, or the worker alone, makes `config.view.make_hooks`' hooks as
/// the view does (HookContext: with no view, the worker's device takes the
/// meshes and the pipeline is null). The command line's settings replace
/// `config.view`'s: validation, device timing, tile depth, labels and vertex
/// sharing.
///
/// @code
/// int main(int argc, char** argv) {
///   volumetric_kit::player::AppConfig config;
///   config.view.make_hooks = make_hooks;  // a MakeHooks
///   return volumetric_kit::player::run_probe(argc, argv, config);
/// }
/// @endcode
/// @param argc    The command line's count, as main has it.
/// @param argv    Its arguments, as main has them.
/// @param config  The view's settings before the command line's.
/// @return 0; 1 for a GPU, log or file failure, a latency record left, an
///         error the validation layer reports or a layer the policy asks for
///         that did not load; 2 for its usage, or for `--latency` or
///         `--save-meshes` with `config.view.make_hooks` set.
int run_probe(int argc, char** argv, const AppConfig& config = {});

}  // namespace volumetric_kit::player
