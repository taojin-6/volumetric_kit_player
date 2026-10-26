// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file desktop.hpp
/// @brief The desktop app as a library call, in the target
///        `volumetric_kit::player_desktop`, which links gfx's GLFW tier and
///        the core's config tier: `apps/volumetric_kit_player/main.cpp` is
///        one call of @ref run_desktop.

#include "volumetric_kit/core/config/config.hpp"
#include "volumetric_kit/player/app_config.hpp"
#include "volumetric_kit/player/player_view.hpp"

namespace volumetric_kit::player {

/// @brief The desktop app: `--config <scenario.json>`, a GLFW window over
///        PlayerView.
///
/// The core loads the scenario with its Player profile (its CONFIG.md) and
/// refuses one the app cannot use; `--help` or `-h` prints the usage. A
/// stream the scenario names that StreamPlayer cannot open is refused before
/// GLFW starts, and with `VP_NO_WINDOW` set the app stops there, refusing
/// with the window's title. The window plays until it closes, or for the
/// scenario's `run.frames` presents of a stream frame, and then prints the
/// frame drawn. The mouse and the keys work the camera and the panel
/// (README.md). Refusals and failures are one line on stderr.
///
/// @code
/// int main(int argc, char** argv) {
///   return volumetric_kit::player::run_desktop(argc, argv);
/// }
/// @endcode
/// @param argc    The command line's count, as main has it.
/// @param argv    Its arguments, as main has them.
/// @param config  The view's settings before the scenario's
///                (@ref desktop_view_config).
/// @return The process's exit code: 0; 1 for a failure; 2 for its usage or
///         a scenario it refuses.
int run_desktop(int argc, char** argv, const AppConfig& config = {});

/// @brief The view's settings @ref run_desktop plays a scenario with, on the
///        host alone.
///
/// @code
/// VKC_ASSIGN(const core::config::Config scenario,
///            core::config::load(path, core::config::Application::Player));
/// const PlayerViewConfig view = desktop_view_config(config, scenario);
/// @endcode
/// @param config    The program's settings.
/// @param scenario  A scenario the core loaded for `Application::Player`.
/// @return `config.view`, make_hooks included, with the scenario's
///         `window.validation`, which a Debug build turns on.
PlayerViewConfig desktop_view_config(const AppConfig& config,
                                     const core::config::Config& scenario);

}  // namespace volumetric_kit::player
