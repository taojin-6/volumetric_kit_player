// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// vp_live_probe host[:port] [--frames N]: plays a sender's live stream through
// PlayerView on gfx's headless surface, with no window, at 60 ticks a second,
// and prints the status line, the stats lines and the stage table once a
// second. It exits 0 once N stream frames are drawn (without --frames, it runs
// until killed), 1 on a GPU failure, and never on a network error, which the
// status line shows. For agents on a Mac: NVIDIA's ICD has no headless
// surface (CMakeLists.txt). A tool, not a test.

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/stage_metrics.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"
#include "volumetric_kit/gfx/ui/imgui_touch.hpp"
#include "volumetric_kit/gfx/windowing/surface_source.hpp"
#include "volumetric_kit/player/controls.hpp"
#include "volumetric_kit/player/player_view.hpp"
#include "volumetric_kit/player/stream_player.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace gfx = volumetric_kit::gfx;
namespace vkp = volumetric_kit::player;
using Clock = std::chrono::steady_clock;

constexpr const char* kUsage =
    "usage: vp_live_probe host[:port] [--frames N]\n"
    "  --frames N  exit once N stream frames are drawn (N >= 1)\n";
constexpr VkExtent2D kExtent{640, 480};
constexpr std::chrono::microseconds kTick{16'667};  // a 60 Hz display's
constexpr std::chrono::seconds kReport{1};

struct Args {
  vkp::LiveAddress address;
  std::optional<std::uint64_t> frames;
};

// The command line, or none for the usage.
std::optional<Args> parse(int argc, char** argv) {
  Args args;
  bool addressed = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--frames" && i + 1 < argc && !args.frames) {
      const std::string_view n = argv[++i];
      std::uint64_t frames = 0;
      const auto [end, error] =
          std::from_chars(n.data(), n.data() + n.size(), frames);
      if (error != std::errc() || end != n.data() + n.size() || frames == 0) {
        return std::nullopt;
      }
      args.frames = frames;
    } else if (!arg.empty() && arg[0] != '-' && !addressed) {
      core::Result<vkp::LiveAddress> address = vkp::parse_live_address(arg);
      if (!address.ok()) {
        return std::nullopt;
      }
      args.address = std::move(address).value();
      addressed = true;
    } else {
      return std::nullopt;
    }
  }
  if (!addressed) {
    return std::nullopt;
  }
  return args;
}

// The status line, the stats lines and the stage table, as the panels show
// them, on one line.
void report(vkp::PlayerView& view) {
  std::string line =
      vkp::live_status_line(view.player().live_status(), Clock::now());
  for (const std::string& stat : view.stats_panel().lines()) {
    line += " | " + stat;
  }
  for (const core::StageRow& row : view.stats_panel().rows()) {
    char text[96];
    if (row.has_gpu) {
      std::snprintf(text, sizeof text, " | %s %.1f ms, GPU %.1f ms", row.name,
                    row.cpu_ms, row.gpu_ms);
    } else {
      std::snprintf(text, sizeof text, " | %s %.1f ms", row.name, row.cpu_ms);
    }
    line += text;
  }
  std::printf("%s\n", line.c_str());
  std::fflush(stdout);
}

core::Status run(const Args& args) {
  VKC_ASSIGN(std::unique_ptr<vkp::StreamPlayer> player,
             vkp::StreamPlayer::connect(args.address));
  VKC_ASSIGN(const gfx::windowing::SurfaceSource surface,
             gfx::windowing::headless_surface());
  vkp::PlayerViewConfig config;
#ifndef NDEBUG
  config.validation = true;
#endif
  VKC_ASSIGN(
      const std::unique_ptr<vkp::PlayerView> view,
      vkp::PlayerView::create(surface, kExtent, std::move(player), config));
  VKC_ASSIGN(const std::unique_ptr<gfx::ui::ImGuiTouch> touch,
             gfx::ui::ImGuiTouch::create(view->ui_context()));
  const Clock::time_point start = Clock::now();
  Clock::time_point tick = start;
  Clock::time_point due = start + kReport;  // the next report
  std::uint64_t drawn = 0;                  // stream frames, each once
  std::optional<std::uint64_t> last;        // the last one's index
  for (;;) {
    gfx::ui::DisplayFrame display;
    display.pixels = kExtent;
    display.pixels_per_point = 1.0f;
    display.seconds = std::chrono::duration<double>(tick - start).count();
    touch->new_frame(display);
    VKC_ASSIGN(const bool presented, view->frame(kExtent));
    const std::optional<vkp::FrameStamp>& shown = view->shown();
    if (presented && shown && last != shown->index) {
      last = shown->index;
      ++drawn;
    }
    const bool done = args.frames && drawn >= *args.frames;
    if (done || Clock::now() >= due) {
      report(*view);
      due += kReport;
    }
    if (done) {
      return {};
    }
    tick += kTick;
    std::this_thread::sleep_until(tick);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::optional<Args> args = parse(argc, argv);
  if (!args) {
    std::fputs(kUsage, stderr);
    return 2;
  }
  const core::Status status = run(*args);
  if (!status.ok()) {
    const std::string_view domain = core::to_string(status.domain());
    std::fprintf(stderr, "vp_live_probe: %.*s: %s\n",
                 static_cast<int>(domain.size()), domain.data(),
                 status.message().c_str());
    return 1;
  }
  return 0;
}
