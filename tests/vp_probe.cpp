// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// vp_probe (--file PATH | --connect HOST[:PORT]) [--frames N] [--passes P]
//          [--timing off|sampled|all] [--tile-depth 0|1] [--labels on|off]
//          [--share-vertices on|off] [--csv PATH] [--worker]:
// plays a .vrst file or a sender's live stream through PlayerView on gfx's
// headless surface, with no window, at 60 ticks a second, and prints the
// status line, the stats lines and the stage tables once a second. With
// --worker it runs the worker alone, with no view, on the GPU the core picks
// (a discrete one first), its meshes taken as a renderer takes them: for a
// GPU with no headless surface, as NVIDIA's has none. With --csv it logs each
// stream frame taken (measure_log.hpp), the file appearing at PATH once the
// run ends. A file plays P passes of its track, 1 unless given. It exits 0
// once N stream frames are taken, or once the last pass ends; 1 on a GPU or
// log failure; 2 for its usage; and never on a network error, which the
// status line shows. It validates in Debug and as the core's test policy asks
// (VKC_TEST_VALIDATION), and then exits 1 on any error the layer reports or
// on a layer that did not load. The measurement runner's (tools/measure/,
// DECISIONS.md), and agents'. A tool, not a test.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/stage_metrics.hpp"
#include "volumetric_kit/core/testing/vulkan_policy.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"
#include "volumetric_kit/gfx/ui/imgui_touch.hpp"
#include "volumetric_kit/gfx/windowing/surface_source.hpp"
#include "volumetric_kit/player/command_line.hpp"
#include "volumetric_kit/player/controls.hpp"
#include "volumetric_kit/player/measure_log.hpp"
#include "volumetric_kit/player/mesh_wiring.hpp"
#include "volumetric_kit/player/playback_stats.hpp"
#include "volumetric_kit/player/player_view.hpp"
#include "volumetric_kit/player/stream_player.hpp"
#include "volumetric_kit/recon/core/device_requirements.hpp"
#include "volumetric_kit/recon/mesh/device_mesh.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace gfx = volumetric_kit::gfx;
namespace test = volumetric_kit::core::test;
namespace vkp = volumetric_kit::player;
using Clock = std::chrono::steady_clock;
using DeviceMesh = volumetric_kit::recon::mesh::DeviceMesh;

constexpr const char* kUsage =
    "usage: vp_probe (--file PATH | --connect HOST[:PORT]) [--frames N]\n"
    "                [--passes P] [--timing off|sampled|all]\n"
    "                [--tile-depth 0|1] [--labels on|off]\n"
    "                [--share-vertices on|off] [--csv PATH] [--worker]\n"
    "  --file PATH            play a .vrst file\n"
    "  --connect HOST[:PORT]  play a sender's live stream\n"
    "  --frames N             stop once N stream frames are taken\n"
    "  --passes P             a file: play P passes of its track (default 1)\n"
    "  --timing T             the worker's device timing (default all)\n"
    "  --tile-depth D         its timed batches' tile depth (default 0)\n"
    "  --labels on|off        debug labels on the instance (default on)\n"
    "  --share-vertices on|off  triangles share vertices (default on)\n"
    "  --csv PATH             log each frame taken into PATH\n"
    "  --worker               the worker alone, with no view\n";
constexpr VkExtent2D kExtent{640, 480};
constexpr std::chrono::microseconds kTick{16'667};  // a 60 Hz display's
constexpr std::chrono::seconds kReport{1};
constexpr std::uint32_t kFramesInFlight =
    2;  // the view's, which --worker takes

struct Args {
  std::string file;
  std::optional<vkp::LiveAddress> address;
  std::optional<std::uint64_t> frames;
  std::optional<std::uint64_t> passes;
  vkp::DeviceTiming timing = vkp::DeviceTiming::kAll;
  std::uint32_t tile_depth = 0;
  bool labels = true;
  bool share_vertices = true;
  std::string csv;
  bool worker = false;
};

// The validation the run asks for: the core's test policy's, and the layer at
// least in Debug.
test::Validation validation() {
#ifndef NDEBUG
  return std::max(test::requested_validation(), test::Validation::On);
#else
  return test::requested_validation();
#endif
}

// The command line, or none for the usage.
std::optional<Args> parse(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--worker" && !args.worker) {
      args.worker = true;
      continue;
    }
    if (i + 1 == argc) {
      return std::nullopt;  // every other option takes a value
    }
    const std::string_view value = argv[++i];
    if (arg == "--file" && args.file.empty() && !value.empty()) {
      args.file = value;
    } else if (arg == "--connect" && !args.address) {
      core::Result<vkp::LiveAddress> address = vkp::parse_live_address(value);
      if (!address.ok()) {
        return std::nullopt;
      }
      args.address = std::move(address).value();
    } else if (arg == "--frames" && !args.frames) {
      args.frames = vkp::parse_count(value);
      if (!args.frames) {
        return std::nullopt;
      }
    } else if (arg == "--passes" && !args.passes) {
      args.passes = vkp::parse_count(value);
      if (!args.passes) {
        return std::nullopt;
      }
    } else if (arg == "--timing") {
      const std::optional<vkp::DeviceTiming> timing =
          vkp::parse_device_timing(value);
      if (!timing) {
        return std::nullopt;
      }
      args.timing = *timing;
    } else if (arg == "--tile-depth") {
      const std::optional<std::uint32_t> depth = vkp::parse_tile_depth(value);
      if (!depth) {
        return std::nullopt;
      }
      args.tile_depth = *depth;
    } else if (arg == "--labels" || arg == "--share-vertices") {
      const std::optional<bool> on = vkp::parse_switch(value);
      if (!on) {
        return std::nullopt;
      }
      (arg == "--labels" ? args.labels : args.share_vertices) = *on;
    } else if (arg == "--csv" && args.csv.empty() && !value.empty()) {
      args.csv = value;
    } else {
      return std::nullopt;
    }
  }
  // One source; passes are a file's.
  if (args.file.empty() == !args.address || (args.address && args.passes)) {
    return std::nullopt;
  }
  return args;
}

// How the run drives its frames, for its log's `run`: its options, less the
// stream, the timing, the tile depth, the labels, the vertex sharing and the
// log, which the log states apart.
std::string run_text(const Args& args) {
  std::string text = "vp_probe";
  if (args.worker) {
    text += " --worker";
  }
  if (args.passes) {
    text += " --passes " + std::to_string(*args.passes);
  }
  if (args.frames) {
    text += " --frames " + std::to_string(*args.frames);
  }
  return text;
}

// A row's busy, wait and device times, as the stats' tables show them.
std::string row_text(const core::StageRow& row) {
  char text[128];
  std::snprintf(text, sizeof text, " | %s %.2f ms", row.name,
                row.cpu_ms - row.wait_ms);
  std::string line = text;
  if (row.wait_ms > 0.0) {
    std::snprintf(text, sizeof text, ", wait %.2f", row.wait_ms);
    line += text;
  }
  switch (row.device) {
    case core::DeviceTime::Measured:
      std::snprintf(text, sizeof text, ", GPU %.2f", row.gpu_ms);
      line += text;
      break;
    case core::DeviceTime::NotTimed:
      line += ", GPU n/t";
      break;
    case core::DeviceTime::Unavailable:
      line += ", GPU n/a";
      break;
    case core::DeviceTime::None:
      break;
  }
  return line;
}

// The player's status line, as the controls show it.
std::string status_text(vkp::StreamPlayer& player,
                        const std::optional<vkp::FrameStamp>& shown) {
  return player.live()
             ? vkp::live_status_line(player.live_status(), Clock::now())
             : vkp::status_line(shown, player.frame_count(), player.playing(),
                                player.loop());
}

// The frames a run takes, until its limits, each logged with --csv.
class Run {
 public:
  Run(const Args& args, bool live) : args_(args), live_(live) {}

  // Open the log, when asked for, with what it measures.
  core::Status open_log(const core::Device& device, std::string stream,
                        std::chrono::system_clock::time_point started) {
    if (args_.csv.empty()) {
      return {};
    }
    vkp::MeasureContext context;
    context.device = vkp::device_line(device);
    context.rings = vkp::rings_line(device);
    context.stream = std::move(stream);
    context.run = run_text(args_);
    context.timing = args_.timing;
    context.tile_depth = args_.tile_depth;
    context.debug_labels = args_.labels;
    context.share_vertices = args_.share_vertices;
    context.started = started;
    VKC_ASSIGN(log_, vkp::MeasureLog::open(args_.csv, context));
    return {};
  }

  // A frame taken, with the render rows as of the tick that took it
  // (PlayerView::render_metrics): false once the run is done. A file's next
  // pass's first frame past the last pass ends it, uncounted.
  core::Result<bool> take(const vkp::FrameStamp& stamp, const DeviceMesh& mesh,
                          const std::vector<core::StageRow>& render = {}) {
    if (!live_ && stamp.pass > args_.passes.value_or(1)) {
      return false;
    }
    if (log_ != nullptr) {
      VKC_TRY(log_->add(stamp, mesh, render));
    }
    ++taken_;
    return !args_.frames || taken_ < *args_.frames;
  }

  // Finish the log, if any.
  core::Status finish() {
    if (log_ == nullptr) {
      return {};
    }
    VKC_TRY(log_->finish());
    std::printf("vp_probe: logged %llu frame(s) into %s\n",
                static_cast<unsigned long long>(log_->frames()),
                log_->path().c_str());
    return {};
  }

 private:
  const Args& args_;
  bool live_;
  std::unique_ptr<vkp::MeasureLog> log_;
  std::uint64_t taken_ = 0;
};

// The stream a log names: a file's name and track, or a sender's address.
std::string stream_of(const Args& args, const vkp::StreamPlayer& player) {
  if (args.address) {
    return vkp::to_string(*args.address);
  }
  return std::filesystem::path(args.file).filename().string() + ", track " +
         std::to_string(player.track());
}

// Ticks at 60 Hz until `tick` says the run is done, reporting once a second.
template <typename Tick, typename Report>
core::Status loop(Tick&& tick, Report&& report) {
  const Clock::time_point start = Clock::now();
  Clock::time_point at = start;
  Clock::time_point due = start + kReport;  // the next report
  for (;;) {
    VKC_ASSIGN(const bool more, tick(at - start));
    if (!more || Clock::now() >= due) {
      report();
      due += kReport;
    }
    if (!more) {
      return {};
    }
    at += kTick;
    std::this_thread::sleep_until(at);
  }
}

core::Status run_view(const Args& args,
                      std::unique_ptr<vkp::StreamPlayer> player) {
  Run run(args, player->live());
  const std::string stream = stream_of(args, *player);
  VKC_ASSIGN(const gfx::windowing::SurfaceSource surface,
             gfx::windowing::headless_surface());
  vkp::PlayerViewConfig config;
  config.validation = validation() != test::Validation::Off;
  config.device_timing = args.timing;
  config.tile_depth = args.tile_depth;
  config.debug_labels = args.labels;
  config.share_vertices = args.share_vertices;
  const std::chrono::system_clock::time_point started =
      std::chrono::system_clock::now();
  VKC_ASSIGN(
      const std::unique_ptr<vkp::PlayerView> view,
      vkp::PlayerView::create(surface, kExtent, std::move(player), config));
  VKC_ASSIGN(const std::unique_ptr<gfx::ui::ImGuiTouch> touch,
             gfx::ui::ImGuiTouch::create(view->ui_context()));
  VKC_TRY(run.open_log(view->compute_device(), stream, started));
  std::uint64_t seen = 0;  // the view's frames taken
  VKC_TRY(loop(
      [&](Clock::duration since) -> core::Result<bool> {
        gfx::ui::DisplayFrame display;
        display.pixels = kExtent;
        display.pixels_per_point = 1.0f;
        display.seconds = std::chrono::duration<double>(since).count();
        touch->new_frame(display);
        VKC_TRY(view->frame(kExtent).status());
        if (view->frames_taken() == seen) {
          return true;
        }
        seen = view->frames_taken();
        return run.take(*view->shown(), view->mesh(),
                        view->render_metrics().sections);
      },
      [&] {
        std::string line = status_text(view->player(), view->shown());
        for (const std::string& stat : view->stats_panel().lines()) {
          line += " | " + stat;
        }
        line += " | per stream frame:";
        for (const core::StageRow& row : view->stats_panel().rows()) {
          line += row_text(row);
        }
        line += " | per display frame:";
        for (const core::StageRow& row : view->stats_panel().render_rows()) {
          line += row_text(row);
        }
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
      }));
  return run.finish();
}

// Stops the worker and waits for the device before what the worker uses
// goes, on every return (stream_player.hpp's teardown).
struct Teardown {
  std::unique_ptr<vkp::StreamPlayer>& player;
  const core::Device& device;
  ~Teardown() {
    player->stop();
    (void)device.wait_idle();
    player.reset();
  }
};

core::Status run_worker(const Args& args,
                        std::unique_ptr<vkp::StreamPlayer> player) {
  Run run(args, player->live());
  const std::string stream = stream_of(args, *player);
  core::InstanceConfig instance_config = test::instance_config(validation());
  instance_config.app_name = "vp_probe";
  instance_config.request_debug_utils = args.labels;
  VKC_ASSIGN(const core::Instance instance,
             core::Instance::create(instance_config));
  VKC_TRY(test::check_layer_loaded(instance));
  const core::DeviceRequirements requirements =
      volumetric_kit::recon::device_requirements();
  VKC_ASSIGN(const core::PhysicalDeviceInfo physical,
             instance.select_physical_device(requirements));
  VKC_ASSIGN(core::Device device,
             core::Device::create(instance, physical, requirements));
  VKC_ASSIGN(core::Allocator allocator,
             core::Allocator::create(instance.handle(), device));
  const vkp::MeshWiring wiring =
      vkp::mesh_wiring(device.queue_family(), device.queue_family(),
                       kFramesInFlight, args.share_vertices);
  vkp::FrameExchange exchange(wiring.exchange);
  vkp::PlaybackStats stats(player->track_stats(), player->rate());
  std::optional<vkp::FrameStamp> shown;
  player->set_device_timing(args.timing);
  player->set_tile_depth(args.tile_depth);
  const std::chrono::system_clock::time_point started =
      std::chrono::system_clock::now();
  VKC_TRY(player->start(device, allocator, wiring.mc, exchange));
  const Teardown teardown{player, device};
  VKC_TRY(run.open_log(device, stream, started));
  std::uint32_t slot = 0;
  VKC_TRY(loop(
      [&](Clock::duration) -> core::Result<bool> {
        VKC_TRY(player->status());
        // Each mesh taken as a renderer of kFramesInFlight frames takes it,
        // with nothing drawn or presented, so every slot's frame has
        // completed.
        bool took = false;
        exchange.begin_frame(slot,
                             [&](const DeviceMesh&, vkp::FrameStamp& stamp) {
                               stamp.taken = vkp::PlaybackStats::Clock::now();
                               stats.add_frame(stamp, *stamp.taken);
                               shown = std::move(stamp);
                               player->taken();
                               took = true;
                               return true;
                             });
        slot = (slot + 1) % kFramesInFlight;
        VKC_TRY(vkp::drawable(exchange));
        if (!took) {
          return true;
        }
        return run.take(*shown, exchange.live());
      },
      [&] {
        std::string line = status_text(*player, shown) + " | per stream frame:";
        for (const core::StageRow& row : stats.stage_means()) {
          line += row_text(row);
        }
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
      }));
  return run.finish();
}

core::Status run(const Args& args) {
  std::unique_ptr<vkp::StreamPlayer> player;
  if (args.address) {
    VKC_ASSIGN(player, vkp::StreamPlayer::connect(*args.address));
  } else {
    VKC_ASSIGN(player, vkp::StreamPlayer::open(args.file));
  }
  return args.worker ? run_worker(args, std::move(player))
                     : run_view(args, std::move(player));
}

}  // namespace

int main(int argc, char** argv) {
  const std::optional<Args> args = parse(argc, argv);
  if (!args) {
    std::fputs(kUsage, stderr);
    return 2;
  }
  // The layer's settings for the run, and its errors counted; everything on
  // the device is gone when run returns.
  const test::ValidationSession session(validation());
  const test::LogCapture log;
  const core::Status status = run(*args);
  int code = 0;
  if (!status.ok()) {
    const std::string_view domain = core::to_string(status.domain());
    std::fprintf(stderr, "vp_probe: %.*s: %s\n",
                 static_cast<int>(domain.size()), domain.data(),
                 status.message().c_str());
    code = 1;
  }
  // A layer the policy asks for that is missing or would not load is the
  // instance's warning: what shows it for the view's instance, which the
  // view keeps (the worker's is checked as it is made).
  if (test::requested_validation() != test::Validation::Off) {
    for (const std::string& warning : log.warnings()) {
      if (warning.find("continuing without it") != std::string::npos) {
        std::fprintf(stderr, "vp_probe: validation asked for: %s\n",
                     warning.c_str());
        code = 1;
      }
    }
  }
  return log.exit_code(code);
}
