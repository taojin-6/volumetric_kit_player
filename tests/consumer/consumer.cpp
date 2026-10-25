// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The consumer's program (CMakeLists.txt here): it links the player's library
// through StreamPlayer, whose worker reaches recon and gfx, so the build links
// the library's whole closure. It runs on the host alone: a missing file is
// refused before any device is made.

#include <memory>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/player/stream_player.hpp"

int main() {
  namespace vkc = volumetric_kit::core;
  namespace vkp = volumetric_kit::player;
  const vkc::Result<std::unique_ptr<vkp::StreamPlayer>> player =
      vkp::StreamPlayer::open("missing.vrst");
  const bool refused =
      !player.ok() && player.status().code() == vkc::Status::Code::NotFound;
  return refused ? 0 : 1;
}
