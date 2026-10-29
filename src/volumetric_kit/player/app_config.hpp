// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file app_config.hpp
/// @brief What a program built on the player sets beyond its command line,
///        for the entry points run_desktop (desktop.hpp) and run_probe
///        (probe.hpp).

#include "volumetric_kit/player/player_view.hpp"

namespace volumetric_kit::player {

/// @brief What a program built on the player sets beyond its command line.
///
/// @code
/// int main(int argc, char** argv) {
///   AppConfig config;
///   config.view.make_hooks = make_hooks;  // a MakeHooks
///   return run_desktop(argc, argv, config);
/// }
/// @endcode
struct AppConfig {
  /// The view's settings before the command line's or the scenario's:
  /// make_hooks, the device_requirements its hooks need beyond grid's, and
  /// the rest. Each entry point says which of them its command line or
  /// scenario sets.
  PlayerViewConfig view;
};

}  // namespace volumetric_kit::player
