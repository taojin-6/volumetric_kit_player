// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// vp_desktop_hooks --config <scenario.json>: the desktop app run as a program
// of its own runs it (desktop.hpp), with hooks of the program's that count
// their calls, printed as it exits. CI's Xvfb step runs it as it runs the app
// (DECISIONS.md); never ctest, as it opens a window (AGENTS.md).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <utility>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"
#include "volumetric_kit/player/app_config.hpp"
#include "volumetric_kit/player/desktop.hpp"
#include "volumetric_kit/player/frame_hooks.hpp"
#include "volumetric_kit/recon/mesh/device_mesh.hpp"
#include "volumetric_kit/recon/volume/voxel_block_grid.hpp"

namespace core = volumetric_kit::core;
namespace recon = volumetric_kit::recon;
namespace vkp = volumetric_kit::player;

int main(int argc, char** argv) {
  // Outlive the view, so every call is counted by the time run_desktop
  // returns.
  std::atomic<int> made{0};
  std::atomic<std::uint64_t> meshed{0};
  std::atomic<std::uint64_t> drawn{0};
  vkp::AppConfig config;
  config.view.make_hooks = [&](const vkp::HookContext&) {
    ++made;
    vkp::FrameHooks hooks;
    hooks.after_mesh =
        [&meshed](core::CommandBatch&, const recon::volume::VoxelBlockGrid&,
                  const recon::mesh::DeviceMesh&, vkp::FrameStamp&) {
          ++meshed;
          return core::Status();
        };
    hooks.before_draw = [&drawn](VkCommandBuffer, const vkp::FrameStamp&) {
      ++drawn;
      return core::Result<vkp::DrawBinding>(vkp::DrawBinding{});
    };
    return core::Result<vkp::FrameHooks>(std::move(hooks));
  };
  const int code = vkp::run_desktop(argc, argv, config);
  std::printf("vp_desktop_hooks: made %d, after_mesh %llu, before_draw %llu\n",
              made.load(), static_cast<unsigned long long>(meshed.load()),
              static_cast<unsigned long long>(drawn.load()));
  return code;
}
