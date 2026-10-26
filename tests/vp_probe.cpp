// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// vp_probe: a file or a sender's stream played with no window (probe.hpp). The
// measurement runner's (tools/measure/, DECISIONS.md), and agents'. A tool,
// not a test.

#include "volumetric_kit/player/probe.hpp"

int main(int argc, char** argv) {
  return volumetric_kit::player::run_probe(argc, argv);
}
