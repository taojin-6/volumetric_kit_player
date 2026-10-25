// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The consumer's program (CMakeLists.txt here): it calls the player library's
// StreamPlayer, which reaches core and recon, and PlayerView, which reaches
// gfx, so it compiles against each sibling's headers and links each, gfx with
// no GLFW tier. It runs on the host alone: each refuses its input before any
// device is made.

#include <memory>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/gfx/windowing/surface_source.hpp"
#include "volumetric_kit/player/player_view.hpp"
#include "volumetric_kit/player/stream_player.hpp"

int main() {
  namespace vkc = volumetric_kit::core;
  namespace vkp = volumetric_kit::player;
  const vkc::Result<std::unique_ptr<vkp::StreamPlayer>> player =
      vkp::StreamPlayer::open("missing.vrst");
  const vkc::Result<std::unique_ptr<vkp::PlayerView>> view =
      vkp::PlayerView::create(volumetric_kit::gfx::windowing::SurfaceSource{},
                              VkExtent2D{1, 1}, nullptr);
  const bool refused =
      !player.ok() && player.status().domain() == vkc::Status::Code::NotFound &&
      !view.ok() &&
      view.status().domain() == vkc::Status::Code::InvalidArgument;
  return refused ? 0 : 1;
}
