// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The consumer's program (CMakeLists.txt here): it calls the player library's
// StreamPlayer, which reaches core and recon, and PlayerView, which reaches
// gfx, so it compiles against each sibling's headers and links each, gfx with
// no GLFW tier; and run_probe, and with VP_CONSUMER_DESKTOP run_desktop, the
// entry points a program's own main calls. It runs on the host alone: each
// refuses its input, or prints its usage, before any device is made.

#include <memory>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/gfx/windowing/surface_source.hpp"
#include "volumetric_kit/player/app_config.hpp"
#include "volumetric_kit/player/frame_hooks.hpp"
#include "volumetric_kit/player/player_view.hpp"
#include "volumetric_kit/player/probe.hpp"
#include "volumetric_kit/player/stream_player.hpp"
#ifdef VP_CONSUMER_DESKTOP
#include "volumetric_kit/player/desktop.hpp"
#endif

int main() {
  namespace vkc = volumetric_kit::core;
  namespace vkp = volumetric_kit::player;
  const vkc::Result<std::unique_ptr<vkp::StreamPlayer>> player =
      vkp::StreamPlayer::open("missing.vrst");
  const vkc::Result<std::unique_ptr<vkp::PlayerView>> view =
      vkp::PlayerView::create(volumetric_kit::gfx::windowing::SurfaceSource{},
                              VkExtent2D{1, 1}, nullptr);
  bool refused = !player.ok() &&
                 player.status().domain() == vkc::Status::Code::NotFound &&
                 !view.ok() &&
                 view.status().domain() == vkc::Status::Code::InvalidArgument;
  // A program's own hooks, with which run_probe refuses --latency.
  vkp::AppConfig config;
  config.view.make_hooks = [](const vkp::HookContext&) {
    return vkc::Result<vkp::FrameHooks>(vkp::FrameHooks{});
  };
  char program[] = "vp_consumer";
  char latency[] = "--latency";
  char file[] = "--file";
  char path[] = "missing.vrst";
  char* probe_argv[] = {program, latency, file, path, nullptr};
  refused = refused && vkp::run_probe(4, probe_argv, config) == 2;
#ifdef VP_CONSUMER_DESKTOP
  char help[] = "--help";
  char* desktop_argv[] = {program, help, nullptr};
  refused = refused && vkp::run_desktop(2, desktop_argv, config) == 0;
#endif
  return refused ? 0 : 1;
}
