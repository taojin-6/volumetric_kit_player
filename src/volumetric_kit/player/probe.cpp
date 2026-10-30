// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// vp_probe (probe.hpp): a file or a sender's stream played with no window,
// through the view on gfx's headless surface or by the worker alone.

#include "volumetric_kit/player/probe.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/stage_metrics.hpp"
#include "volumetric_kit/core/testing/vulkan_policy.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"
#include "volumetric_kit/gfx/ui/imgui_touch.hpp"
#include "volumetric_kit/gfx/windowing/surface_source.hpp"
#include "volumetric_kit/grid/mesh/device_mesh.hpp"
#include "volumetric_kit/grid/mesh/mesh.hpp"
#include "volumetric_kit/grid/mesh_io/ply_writer.hpp"
#include "volumetric_kit/grid/volume/voxel_block_grid.hpp"
#include "volumetric_kit/player/app_config.hpp"
#include "volumetric_kit/player/command_line.hpp"
#include "volumetric_kit/player/controls.hpp"
#include "volumetric_kit/player/frame_hooks.hpp"
#include "volumetric_kit/player/measure_log.hpp"
#include "volumetric_kit/player/mesh_wiring.hpp"
#include "volumetric_kit/player/playback_stats.hpp"
#include "volumetric_kit/player/player_view.hpp"
#include "volumetric_kit/player/stream_player.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace gfx = volumetric_kit::gfx;
namespace test = volumetric_kit::core::test;
namespace vkp = volumetric_kit::player;
using Clock = std::chrono::steady_clock;
using DeviceMesh = volumetric_kit::grid::mesh::DeviceMesh;

constexpr const char* kUsage =
    "usage: vp_probe (--file PATH | --connect HOST[:PORT]) [--frames N]\n"
    "                [--passes P] [--timing off|sampled|all]\n"
    "                [--tile-depth 0|1] [--labels on|off]\n"
    "                [--share-vertices on|off] [--csv PATH] [--latency]\n"
    "                [--save-meshes DIR] [--worker]\n"
    "  --file PATH            play a .vrst file\n"
    "  --connect HOST[:PORT]  play a sender's live stream\n"
    "  --frames N             stop once N stream frames are taken\n"
    "  --passes P             a file: play P passes of its track (default 1)\n"
    "  --timing T             the worker's device timing (default all)\n"
    "  --tile-depth D         its timed batches' tile depth (default 0)\n"
    "  --labels on|off        debug labels on the instance (default on)\n"
    "  --share-vertices on|off  triangles share vertices (default on)\n"
    "  --csv PATH             log each frame taken into PATH\n"
    "  --latency              time each mesh to its commit and its draws\n"
    "  --save-meshes DIR      write each mesh as DIR/<frame>.ply\n"
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
  bool latency = false;
  std::string save_meshes;
  bool worker = false;
  bool program_hooks = false;  // the program set make_hooks (AppConfig)
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
    if (arg == "--latency" && !args.latency) {
      args.latency = true;
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
    } else if (arg == "--save-meshes" && args.save_meshes.empty() &&
               !value.empty()) {
      args.save_meshes = value;
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

// The view's settings (probe_view_config): the program's, then the command
// line's.
vkp::PlayerViewConfig view_config(const Args& args, const vkp::AppConfig& app) {
  vkp::PlayerViewConfig config = app.view;
  config.validation = validation() != test::Validation::Off;
  config.device_timing = args.timing;
  config.tile_depth = args.tile_depth;
  config.debug_labels = args.labels;
  config.share_vertices = args.share_vertices;
  return config;
}

// How the run drives its frames, for its log's `run`: its options, less the
// stream, the timing, the tile depth, the labels, the vertex sharing and the
// log, which the log states apart. The hooks' options add a batch a frame, as
// a program's hooks do, which it names too.
std::string run_text(const Args& args) {
  std::string text = "vp_probe";
  if (args.worker) {
    text += " --worker";
  }
  if (args.latency) {
    text += " --latency";
  }
  if (!args.save_meshes.empty()) {
    text += " --save-meshes";
  }
  if (args.passes) {
    text += " --passes " + std::to_string(*args.passes);
  }
  if (args.frames) {
    text += " --frames " + std::to_string(*args.frames);
  }
  if (args.program_hooks) {
    text += " with the program's make_hooks";
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

// --latency: each frame timed from its mesh finished on the worker
// (after_mesh) to its commit and to each draw of it (before_draw), through a
// record its stamp carries (FrameStamp::extra). The worker makes records, the
// drawing thread reads them, and a park forgets those in flight.
class LatencyProbe {
 public:
  // A frame's record, for its stamp: its mesh finished now. Its release is
  // counted, even after the probe goes.
  std::shared_ptr<const void> meshed() {
    auto record = std::make_unique<Record>();
    record->meshed = Clock::now();
    record->epoch = epoch_.load();
    ++*made_;
    const std::shared_ptr<std::atomic<std::uint64_t>> released = released_;
    return std::shared_ptr<const void>(
        record.release(), [released](const void* record) {
          delete static_cast<const Record*>(record);
          ++*released;
        });
  }
  // The frame of `extra`, a record of meshed's, committed now.
  void committed(const std::shared_ptr<const void>& extra) {
    const Record* record = counted(extra);
    if (record != nullptr) {
      std::lock_guard<std::mutex> lock(mutex_);
      to_commit_.push_back(ms_since(record->meshed));
    }
  }
  // The frame of `extra` drawn now: its first draw, and its age at each.
  void drawn(const std::shared_ptr<const void>& extra) {
    const Record* record = counted(extra);
    if (record != nullptr) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!record->drawn) {
        record->drawn = true;
        to_first_draw_.push_back(ms_since(record->meshed));
      }
      ages_.push_back(ms_since(record->meshed));
    }
  }
  // A park: the frames in flight are not counted, so a suspend is not.
  void forget() { ++epoch_; }

  // The figures since the last report, in ms, and the frames held and
  // skipped meanwhile; draws only with a view.
  std::string report(bool draws, std::uint64_t held, std::uint64_t skipped) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string line = "latency (ms): mesh to commit" + figures(&to_commit_);
    if (draws) {
      line += " | mesh to first draw" + figures(&to_first_draw_);
      line += " | age at draw" + figures(&ages_);
    }
    char counts[96];
    std::snprintf(counts, sizeof counts, " | held %llu, skipped %llu",
                  static_cast<unsigned long long>(held - held_),
                  static_cast<unsigned long long>(skipped - skipped_));
    held_ = held;
    skipped_ = skipped;
    return line + counts;
  }
  std::uint64_t made() const { return made_->load(); }
  // Records not yet released.
  std::uint64_t left() const { return made_->load() - released_->load(); }

 private:
  struct Record {
    Clock::time_point meshed;
    std::uint64_t epoch = 0;
    mutable bool drawn = false;  // under mutex_
  };

  // `extra`'s record, unless a park forgot it; null for none.
  const Record* counted(const std::shared_ptr<const void>& extra) const {
    const auto* record = static_cast<const Record*>(extra.get());
    return record != nullptr && record->epoch == epoch_.load() ? record
                                                               : nullptr;
  }
  static double ms_since(Clock::time_point at) {
    return std::chrono::duration<double, std::milli>(Clock::now() - at).count();
  }
  // " p50 a, p95 b, max c (n)" of `samples`, which it clears; " -" for none.
  static std::string figures(std::vector<double>* samples) {
    if (samples->empty()) {
      return " -";
    }
    std::sort(samples->begin(), samples->end());
    const auto at = [samples](double q) {
      return (*samples)[static_cast<std::size_t>(
          q * static_cast<double>(samples->size() - 1))];
    };
    char text[96];
    std::snprintf(text, sizeof text, " p50 %.2f, p95 %.2f, max %.2f (%zu)",
                  at(0.5), at(0.95), samples->back(), samples->size());
    samples->clear();
    return text;
  }

  std::atomic<std::uint64_t> epoch_{0};
  std::shared_ptr<std::atomic<std::uint64_t>> made_ =
      std::make_shared<std::atomic<std::uint64_t>>(0);
  std::shared_ptr<std::atomic<std::uint64_t>> released_ =
      std::make_shared<std::atomic<std::uint64_t>>(0);
  std::mutex mutex_;
  std::vector<double> to_commit_;
  std::vector<double> to_first_draw_;
  std::vector<double> ages_;
  std::uint64_t held_ = 0;  // as of the last report
  std::uint64_t skipped_ = 0;
};

// --save-meshes DIR: each mesh read back in its after_mesh batch, and written
// as DIR/<frame>.ply (grid's mesh_io::write_ply) once that batch has finished:
// at the next mesh, as the player finishes the batch before it, or at the end.
class MeshSaver {
 public:
  explicit MeshSaver(std::string dir) : dir_(std::move(dir)) {}

  // The worker's, in after_mesh: write the mesh before, then record this
  // one's readback into `batch`.
  core::Status record(core::CommandBatch& batch,
                      const volumetric_kit::grid::volume::VoxelBlockGrid& grid,
                      const DeviceMesh& mesh, const vkp::FrameStamp& stamp) {
    VKC_TRY(finish());
    Pending& pending = pending_.emplace();
    pending.index = stamp.index;
    pending.blocks = grid.map().allocated_blocks();
    pending.mesh.vertices.resize(mesh.vertex_count);
    pending.mesh.indices.resize(std::size_t{3} * mesh.triangle_count);
    const VkDeviceSize vertices = VkDeviceSize{mesh.vertex_count} *
                                  sizeof(volumetric_kit::grid::mesh::Vertex);
    const VkDeviceSize indices =
        VkDeviceSize{3} * mesh.triangle_count * sizeof(std::uint32_t);
    if (vertices != 0) {
      VKC_TRY(batch.readback(
          view(mesh.vertices, vertices, mesh.vertex_usage, mesh.sharing_mode),
          0, vertices, pending.mesh.vertices.data()));
    }
    if (indices != 0) {
      VKC_TRY(batch.readback(
          view(mesh.indices, indices, mesh.index_usage, mesh.sharing_mode), 0,
          indices, pending.mesh.indices.data()));
    }
    return {};
  }

  // Write the mesh recorded last, if any, its batch finished.
  core::Status finish() {
    if (!pending_) {
      return {};
    }
    const std::string path = (std::filesystem::path(dir_) /
                              (std::to_string(pending_->index) + ".ply"))
                                 .string();
    VKC_TRY(volumetric_kit::grid::mesh_io::write_ply(path, pending_->mesh));
    std::printf("vp_probe: saved %s: %zu vertices, %zu triangles, %u blocks\n",
                path.c_str(), pending_->mesh.vertices.size(),
                pending_->mesh.triangle_count(), pending_->blocks);
    std::fflush(stdout);
    pending_.reset();
    return {};
  }

 private:
  struct Pending {
    std::uint64_t index = 0;
    std::uint32_t blocks = 0;  // the grid's allocated blocks
    volumetric_kit::grid::mesh::Mesh mesh;
  };

  // A view of the extractor's `buffer`: its deleter frees nothing.
  static core::Buffer view(VkBuffer buffer, VkDeviceSize bytes,
                           VkBufferUsageFlags usage, VkSharingMode sharing) {
    return core::Buffer(
        buffer, bytes, usage, sharing, nullptr, [] {}, std::nullopt);
  }

  std::string dir_;
  std::optional<Pending> pending_;
};

// The hooks --latency and --save-meshes ask for: after_mesh saves the mesh
// and stamps its record, the commit and before_draw time it, and on_park
// forgets the frames in flight.
vkp::FrameHooks probe_hooks(LatencyProbe* latency, MeshSaver* saver) {
  vkp::FrameHooks hooks;
  hooks.after_mesh =
      [latency, saver](core::CommandBatch& batch,
                       const volumetric_kit::grid::volume::VoxelBlockGrid& grid,
                       const DeviceMesh& mesh, vkp::FrameStamp& stamp) {
        if (saver != nullptr) {
          VKC_TRY(saver->record(batch, grid, mesh, stamp));
        }
        if (latency != nullptr) {
          stamp.extra = latency->meshed();
        }
        return core::Status();
      };
  if (latency != nullptr) {
    hooks.commit = [latency](const DeviceMesh&, vkp::FrameStamp& stamp) {
      latency->committed(stamp.extra);
      return core::Result<vkp::FrameCommit>(vkp::FrameCommit::kDraw);
    };
    hooks.before_draw = [latency](VkCommandBuffer,
                                  const vkp::FrameStamp& stamp) {
      latency->drawn(stamp.extra);
      return core::Result<vkp::DrawBinding>(vkp::DrawBinding{});
    };
    hooks.on_park = [latency] { latency->forget(); };
  }
  return hooks;
}

// What the hooks' options made, for a run: none without them. They outlive
// the view or the worker, and every stamp.
struct Probes {
  std::optional<LatencyProbe> latency;
  std::optional<MeshSaver> saver;

  bool any() const { return latency || saver; }
};

// The hooks `probes` asks for, made as PlayerViewConfig::make_hooks makes
// them, for the view and for the worker alone alike.
vkp::MakeHooks make_probe_hooks(Probes& probes) {
  return [&probes](const vkp::HookContext&) -> core::Result<vkp::FrameHooks> {
    return probe_hooks(probes.latency ? &*probes.latency : nullptr,
                       probes.saver ? &*probes.saver : nullptr);
  };
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

// A line of the latency's figures, with --latency.
void report_latency(Probes& probes, bool draws, std::uint64_t held,
                    std::uint64_t skipped) {
  if (probes.latency) {
    std::printf("vp_probe: %s\n",
                probes.latency->report(draws, held, skipped).c_str());
    std::fflush(stdout);
  }
}

core::Status run_view(const Args& args, const vkp::PlayerViewConfig& config,
                      std::unique_ptr<vkp::StreamPlayer> player,
                      Probes& probes) {
  Run run(args, player->live());
  const std::string stream = stream_of(args, *player);
  VKC_ASSIGN(const gfx::windowing::SurfaceSource surface,
             gfx::windowing::headless_surface());
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
        report_latency(probes, /*draws=*/true, view->frames_held(),
                       view->frames_skipped());
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

core::Status run_worker(const Args& args, const vkp::PlayerViewConfig& config,
                        std::unique_ptr<vkp::StreamPlayer> player,
                        Probes& probes) {
  const vkp::MakeHooks& make_hooks = config.make_hooks;
  Run run(args, player->live());
  const std::string stream = stream_of(args, *player);
  core::InstanceConfig instance_config = test::instance_config(validation());
  instance_config.app_name = "vp_probe";
  instance_config.request_debug_utils = args.labels;
  VKC_ASSIGN(const core::Instance instance,
             core::Instance::create(instance_config));
  VKC_TRY(test::check_layer_loaded(instance));
  // The view's: grid's, and the program's (PlayerView::create).
  VKC_ASSIGN(const core::DeviceRequirements requirements,
             vkp::worker_requirements(config.device_requirements));
  VKC_ASSIGN(const core::PhysicalDeviceInfo physical,
             instance.select_physical_device(requirements));
  VKC_ASSIGN(core::Device device,
             core::Device::create(instance, physical, requirements));
  VKC_ASSIGN(core::Allocator allocator,
             core::Allocator::create(instance.handle(), device));
  const vkp::MeshWiring wiring = vkp::mesh_wiring(
      device.queue_family(), device.queue_family(), kFramesInFlight,
      args.share_vertices, static_cast<bool>(make_hooks));
  vkp::FrameExchange exchange(wiring.exchange);
  vkp::PlaybackStats stats(player->track_stats(), player->rate());
  std::optional<vkp::FrameStamp> shown;
  // Made as the view makes them, with no view: the worker's device takes
  // the meshes.
  vkp::FrameHooks hooks;
  if (make_hooks) {
    const vkp::HookContext context{device,
                                   allocator,
                                   device,
                                   allocator,
                                   nullptr,
                                   device.queue_family(),
                                   device.queue_family(),
                                   {}};  // no view, no target layout
    VKC_ASSIGN(hooks, make_hooks(context));
  }
  player->set_device_timing(args.timing);
  player->set_tile_depth(args.tile_depth);
  const std::chrono::system_clock::time_point started =
      std::chrono::system_clock::now();
  VKC_TRY(player->start(device, allocator, wiring.mc, exchange, &hooks));
  const Teardown teardown{player, device};
  // Each mesh taken as a renderer of kFramesInFlight frames takes it, with
  // nothing drawn or presented, so every slot's frame has completed.
  vkp::FrameTaker taker(*player, exchange, kFramesInFlight, &hooks);
  VKC_TRY(run.open_log(device, stream, started));
  std::uint32_t slot = 0;
  VKC_TRY(loop(
      [&](Clock::duration) -> core::Result<bool> {
        VKC_TRY(player->status());
        VKC_ASSIGN(std::optional<vkp::FrameStamp> taken, taker.take(slot));
        slot = (slot + 1) % kFramesInFlight;
        VKC_TRY(vkp::drawable(exchange));
        if (!taken) {
          return true;
        }
        stats.add_frame(*taken, *taken->taken);
        shown = std::move(taken);
        return run.take(*shown, exchange.live());
      },
      [&] {
        std::string line = status_text(*player, shown) + " | per stream frame:";
        for (const core::StageRow& row : stats.stage_means()) {
          line += row_text(row);
        }
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
        report_latency(probes, /*draws=*/false, taker.frames_held(),
                       taker.frames_skipped());
      }));
  return run.finish();
}

core::Status run(const Args& args, const vkp::AppConfig& app, Probes& probes) {
  std::unique_ptr<vkp::StreamPlayer> player;
  if (args.address) {
    VKC_ASSIGN(player, vkp::StreamPlayer::connect(*args.address));
  } else {
    VKC_ASSIGN(player, vkp::StreamPlayer::open(args.file));
  }
  if (!args.save_meshes.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(args.save_meshes, ec);
    if (ec) {
      return core::Status::io_error("cannot make " + args.save_meshes + ": " +
                                    ec.message());
    }
    probes.saver.emplace(args.save_meshes);
  }
  if (args.latency) {
    probes.latency.emplace();
  }
  // The hooks' options install theirs as a program's own would.
  vkp::PlayerViewConfig config = view_config(args, app);
  if (probes.any()) {
    config.make_hooks = make_probe_hooks(probes);
  }
  // The view, or the worker, is gone on return: its last mesh is saved.
  VKC_TRY(args.worker ? run_worker(args, config, std::move(player), probes)
                      : run_view(args, config, std::move(player), probes));
  return probes.saver ? probes.saver->finish() : core::Status();
}

}  // namespace

int volumetric_kit::player::run_probe(int argc, char** argv,
                                      const AppConfig& config) {
  std::optional<Args> args = parse(argc, argv);
  if (!args) {
    std::fputs(kUsage, stderr);
    return 2;
  }
  args->program_hooks = static_cast<bool>(config.view.make_hooks);
  if ((args->latency || !args->save_meshes.empty()) && args->program_hooks) {
    std::fputs(
        "vp_probe: --latency and --save-meshes make hooks of their own, and "
        "the program's make_hooks is set\n",
        stderr);
    return 2;
  }
  // The layer's settings for the run, and its errors counted; everything on
  // the device is gone when run returns.
  const test::ValidationSession session(validation());
  const test::LogCapture log;
  Probes probes;  // outlives every stamp
  const core::Status status = run(*args, config, probes);
  int code = 0;
  if (!status.ok()) {
    const std::string_view domain = core::to_string(status.domain());
    std::fprintf(stderr, "vp_probe: %.*s: %s\n",
                 static_cast<int>(domain.size()), domain.data(),
                 status.message().c_str());
    code = 1;
  }
  // Every stamp is gone, so every record made is released.
  if (probes.latency) {
    const std::uint64_t left = probes.latency->left();
    std::printf("vp_probe: latency: %llu record(s) made, %llu left\n",
                static_cast<unsigned long long>(probes.latency->made()),
                static_cast<unsigned long long>(left));
    if (left != 0) {
      code = 1;
    }
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

std::optional<volumetric_kit::player::PlayerViewConfig>
volumetric_kit::player::probe_view_config(const AppConfig& config, int argc,
                                          char** argv) {
  const std::optional<Args> args = parse(argc, argv);
  if (!args) {
    return std::nullopt;
  }
  return view_config(*args, config);
}
