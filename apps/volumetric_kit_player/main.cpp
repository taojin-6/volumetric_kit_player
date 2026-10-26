// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// volumetric_kit_player --config <scenario.json>: plays a scenario's stream,
// from a file or live from a sender, in a GLFW window (desktop.hpp).

#include "volumetric_kit/player/desktop.hpp"

int main(int argc, char** argv) {
  return volumetric_kit::player::run_desktop(argc, argv);
}
