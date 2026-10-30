// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// run_probe (probe.hpp) as a program's own main calls it, with hooks of the
// program's (AppConfig::view.make_hooks) that count their calls. On the tests'
// stream, one pass: the worker alone makes them once, with no pipeline, and
// runs after_mesh once for each frame it publishes, every frame of the pass,
// and the commit only for frames after_mesh saw; the view does the same with
// its pipeline, where the loader has a headless surface (MoltenVK; it fails
// on Apple, and skips on NVIDIA's ICD). Each run's log (--csv) names the
// program's hooks in its `run`. Hooks that skip frame 1 and hold the commit
// of frame 2 until frame 4 is meshed keep five meshes of the ring at once,
// which the ring one mesh deeper for a program's hooks (mesh_wiring's
// `holds`) has room for: the run passes, by the worker alone and through the
// view. A program's device requirements (AppConfig::view.device_requirements),
// by the worker alone and through the view: an optional extension the GPU
// offers is enabled on the hooks' devices, one it lacks skipped, and grid's
// stay enabled under requirements that leave them out, the run passing; with
// none, the device is grid's alone; and a required extension no GPU offers
// fails the run, its hooks never made. Through the view, --panel on opens a
// program's panel from the start, drawn at --size's extent. On the host:
// --latency and
// --save-meshes exit 2 when a program set make_hooks, which is never made,
// and without it pass to the stream's refusal; and the view's settings
// run_probe plays with (probe_view_config) are the program's, make_hooks and
// device requirements included, with the command line's validation, timing,
// tile depth, labels, vertex sharing and open panel, its defaults included,
// and none for its usage (an open panel or a size with --worker among it).
// --live-delay MS is a floor under a live program's need: the larger of MS
// and the program's own, or MS with none, its other hooks kept; 0 and 1000
// are taken, and past 1000, below 0, not a number, given twice or with
// --file it is the usage. The device tests skip with no device,
// and fail without one under VKC_REQUIRE_VULKAN_DEVICE. run_probe validates
// as the core's test policy asks, its exit code failing on any error the
// layer reports.

#include "volumetric_kit/player/probe.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "program_requirements.hpp"
#include "test_stream.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/core/testing/vulkan_policy.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"
#include "volumetric_kit/gfx/windowing/surface_source.hpp"
#include "volumetric_kit/grid/base/device_requirements.hpp"
#include "volumetric_kit/grid/mesh/device_mesh.hpp"
#include "volumetric_kit/grid/volume/voxel_block_grid.hpp"
#include "volumetric_kit/player/app_config.hpp"
#include "volumetric_kit/player/frame_hooks.hpp"
#include "volumetric_kit/player/player_view.hpp"
#include "volumetric_kit/player/stream_player.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace gfx = volumetric_kit::gfx;
namespace grid = volumetric_kit::grid;
namespace test = volumetric_kit::core::test;
namespace vkp = volumetric_kit::player;
using vp_test::Scratch;

// A frame as (pass, index).
using Frame = std::pair<std::uint64_t, std::uint64_t>;

// A program's hooks, counting their calls on whichever thread makes or runs
// them: after_mesh notes each frame it runs for, and the commit, which draws
// every frame, each frame it is offered.
class CountingHooks {
 public:
  vkp::MakeHooks make_hooks() {
    return [this](const vkp::HookContext& context) { return make(context); };
  }

  int makes() const { return makes_.load(); }
  bool with_pipeline() const { return with_pipeline_.load(); }
  std::vector<Frame> meshed() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return meshed_;
  }
  std::vector<Frame> offered() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return offered_;
  }

 private:
  core::Result<vkp::FrameHooks> make(const vkp::HookContext& context) {
    ++makes_;
    with_pipeline_ = context.pipeline != nullptr;
    vkp::FrameHooks hooks;
    hooks.after_mesh =
        [this](core::CommandBatch&, const grid::volume::VoxelBlockGrid&,
               const grid::mesh::DeviceMesh&, vkp::FrameStamp& stamp) {
          const std::lock_guard<std::mutex> lock(mutex_);
          meshed_.emplace_back(stamp.pass, stamp.index);
          return core::Status();
        };
    hooks.commit = [this](const grid::mesh::DeviceMesh&,
                          vkp::FrameStamp& stamp) {
      const std::lock_guard<std::mutex> lock(mutex_);
      offered_.emplace_back(stamp.pass, stamp.index);
      return core::Result<vkp::FrameCommit>(vkp::FrameCommit::kDraw);
    };
    return hooks;
  }

  std::atomic<int> makes_{0};
  std::atomic<bool> with_pipeline_{false};
  mutable std::mutex mutex_;
  std::vector<Frame> meshed_;
  std::vector<Frame> offered_;
};

// A program's hooks that skip frame 1 of the first pass and, offered its
// frame 2, wait for after_mesh to run for its frame 4 (kWait at most) before
// drawing it. Meanwhile frame 0 is drawn, 1 skipped, 2 offered, 3 published
// and 4 meshed, five meshes of the ring at once: the frame drawn stays live,
// so none of them is released, and the worker meshes frame 4 once frame 3 is
// published, which the take of frame 2 allows.
class SkippingHooks {
 public:
  static constexpr std::chrono::seconds kWait{10};

  vkp::MakeHooks make_hooks() {
    return [this](const vkp::HookContext&) { return make(); };
  }

  bool skipped() const { return skipped_.load(); }
  // Whether frame 4 was meshed while frame 2 waited.
  bool waited() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return waited_;
  }

 private:
  core::Result<vkp::FrameHooks> make() {
    vkp::FrameHooks hooks;
    hooks.after_mesh =
        [this](core::CommandBatch&, const grid::volume::VoxelBlockGrid&,
               const grid::mesh::DeviceMesh&, vkp::FrameStamp& stamp) {
          if (stamp.pass == 1 && stamp.index == 4) {
            {
              const std::lock_guard<std::mutex> lock(mutex_);
              meshed_ = true;
            }
            changed_.notify_all();
          }
          return core::Status();
        };
    hooks.commit = [this](const grid::mesh::DeviceMesh&,
                          vkp::FrameStamp& stamp) {
      if (stamp.pass == 1 && stamp.index == 1) {
        skipped_ = true;
        return core::Result<vkp::FrameCommit>(vkp::FrameCommit::kSkip);
      }
      if (stamp.pass == 1 && stamp.index == 2) {
        std::unique_lock<std::mutex> lock(mutex_);
        waited_ = changed_.wait_for(lock, kWait, [this] { return meshed_; });
      }
      return core::Result<vkp::FrameCommit>(vkp::FrameCommit::kDraw);
    };
    return hooks;
  }

  std::atomic<bool> skipped_{false};
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool meshed_ = false;  // frame 4
  bool waited_ = false;
};

// A program's hooks that note, as they are made, what their devices enabled:
// extension `name`, the absent one (program_requirements.hpp), and grid's
// requirements. Made on the thread that calls run_probe.
class DeviceHooks {
 public:
  explicit DeviceHooks(std::string name) : name_(std::move(name)) {}

  vkp::MakeHooks make_hooks() {
    return [this](const vkp::HookContext& context) {
      ++makes_;
      const char* name = name_.c_str();
      enabled_ = context.device.extension_enabled(name) &&
                 context.commit_device.extension_enabled(name);
      absent_ =
          context.device.extension_enabled(vp_test::kAbsentExtension) ||
          context.commit_device.extension_enabled(vp_test::kAbsentExtension);
      grids_ = context.device.check_enabled(grid::device_requirements());
      return core::Result<vkp::FrameHooks>(vkp::FrameHooks{});
    };
  }

  int makes() const { return makes_; }
  // Whether both devices enabled `name`.
  bool enabled() const { return enabled_; }
  // Whether either enabled the absent extension.
  bool absent() const { return absent_; }
  // The worker's device's check of grid's requirements.
  const core::Status& grids() const { return grids_; }

 private:
  std::string name_;
  int makes_ = 0;
  bool enabled_ = false;
  bool absent_ = false;
  core::Status grids_ = core::Status::invalid_argument("never made");
};

// A program's hooks with a panel that notes, as the view records it on the
// thread that calls run_probe, how often and the pass's extent.
class PanelHooks {
 public:
  vkp::MakeHooks make_hooks() {
    return [this](const vkp::HookContext&) {
      vkp::FrameHooks hooks;
      hooks.panel.title = "Probe test";
      hooks.panel.record = [this](VkCommandBuffer,
                                  const vkp::PanelFrame& frame) {
        ++records_;
        drawable_ = frame.drawable;
      };
      return core::Result<vkp::FrameHooks>(std::move(hooks));
    };
  }

  int records() const { return records_; }
  // The pass's extent at the last record.
  VkExtent2D drawable() const { return drawable_; }

 private:
  int records_ = 0;
  VkExtent2D drawable_{};
};

// `call(argc, argv)` with `args` after the program's name, as main has them.
template <typename Call>
auto with_argv(std::vector<std::string> args, Call&& call) {
  args.insert(args.begin(), "vp_probe_test");
  std::vector<char*> argv;
  for (std::string& arg : args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  return call(static_cast<int>(args.size()), argv.data());
}

// run_probe on `args`, after the program's name, with `config`.
int run(std::vector<std::string> args, const vkp::AppConfig& config) {
  return with_argv(std::move(args), [&config](int argc, char** argv) {
    return vkp::run_probe(argc, argv, config);
  });
}

// probe_view_config on `args`, after the program's name, with `config`.
std::optional<vkp::PlayerViewConfig> view_of(std::vector<std::string> args,
                                             const vkp::AppConfig& config) {
  return with_argv(std::move(args), [&config](int argc, char** argv) {
    return vkp::probe_view_config(config, argc, argv);
  });
}

// The `run` a measure log at `path` states (measure_log.hpp), or empty.
std::string logged_run(const std::string& path) {
  std::ifstream in(path);
  const std::string key = "# run: ";
  for (std::string line; std::getline(in, line);) {
    if (line.rfind(key, 0) == 0) {
      return line.substr(key.size());
    }
  }
  return {};
}

// The hooks' calls after a run of the stream's one pass: made once; after_mesh
// once a frame, for every frame of the pass and no frame of a pass after the
// next, whose first frame ends the run; and the commit offered only frames
// after_mesh saw.
void expect_counted(const CountingHooks& hooks) {
  EXPECT_EQ(hooks.makes(), 1);
  const std::vector<Frame> meshed = hooks.meshed();
  const std::set<Frame> distinct(meshed.begin(), meshed.end());
  EXPECT_EQ(distinct.size(), meshed.size()) << "a frame meshed twice";
  for (std::uint64_t i = 0; i < vp_test::kFrames; ++i) {
    EXPECT_EQ(distinct.count({1, i}), 1u) << "frame " << i;
  }
  for (const Frame& frame : meshed) {
    EXPECT_LE(frame.first, 2u) << "pass " << frame.first;
  }
  const std::vector<Frame> offered = hooks.offered();
  EXPECT_FALSE(offered.empty());
  for (const Frame& frame : offered) {
    EXPECT_EQ(distinct.count(frame), 1u)
        << "frame " << frame.second << " of pass " << frame.first
        << " offered with no after_mesh";
  }
}

// On a device that meets grid's requirements, over the tests' stream.
class RunProbeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const core::Result<core::Instance> instance =
        core::Instance::create(test::instance_config(test::Validation::Off));
    core::Status found = instance.status();
    if (instance.ok()) {
      const core::Result<core::PhysicalDeviceInfo> physical =
          instance->select_physical_device(grid::device_requirements());
      found = physical.status();
      if (physical.ok()) {
        offered_ = vp_test::offered_extension(*physical);
      }
    }
    if (!found.ok()) {
      if (test::device_required()) {
        FAIL() << "no Vulkan device for this test: " << found.message();
      }
      GTEST_SKIP() << "no Vulkan device for this test: " << found.message();
    }
    path_ = scratch_.copy(VP_TEST_STREAMS, vp_test::kStream);
    ASSERT_FALSE(path_.empty());
  }

  // run_probe's device, by the worker alone or through the view, with a
  // program's optional extension the GPU offers and one it lacks, in
  // requirements that leave grid's out: on the hooks' devices the one
  // offered is enabled, the other not, and grid's are; the run passes.
  void expect_optional_enabled(bool worker) {
    ASSERT_FALSE(offered_.empty()) << "the GPU offers none of the extensions";
    DeviceHooks hooks(offered_);
    vkp::AppConfig config;
    config.view.make_hooks = hooks.make_hooks();
    config.view.device_requirements.optional_extensions = {
        offered_, vp_test::kAbsentExtension};
    config.view.device_requirements.timeline_semaphore = false;
    config.view.device_requirements.scalar_block_layout = false;
    EXPECT_EQ(run(args(worker), config), 0);
    EXPECT_EQ(hooks.makes(), 1);
    EXPECT_TRUE(hooks.enabled()) << offered_;
    EXPECT_FALSE(hooks.absent());
    EXPECT_TRUE(hooks.grids().ok()) << hooks.grids().message();
  }

  // With no program requirements, the device is grid's alone: the extension
  // offered is off, and grid's are on.
  void expect_grids_alone(bool worker) {
    ASSERT_FALSE(offered_.empty()) << "the GPU offers none of the extensions";
    DeviceHooks hooks(offered_);
    vkp::AppConfig config;
    config.view.make_hooks = hooks.make_hooks();
    EXPECT_EQ(run(args(worker), config), 0);
    EXPECT_EQ(hooks.makes(), 1);
    EXPECT_FALSE(hooks.enabled()) << offered_;
    EXPECT_TRUE(hooks.grids().ok()) << hooks.grids().message();
  }

  // A program's required extension no GPU offers fails the run before its
  // hooks are made.
  void expect_required_refused(bool worker) {
    DeviceHooks hooks(vp_test::kAbsentExtension);
    vkp::AppConfig config;
    config.view.make_hooks = hooks.make_hooks();
    config.view.device_requirements.extensions = {vp_test::kAbsentExtension};
    EXPECT_EQ(run(args(worker), config), 1);
    EXPECT_EQ(hooks.makes(), 0);
  }

  // Two frames of the stream, by the worker alone or through the view.
  std::vector<std::string> args(bool worker) const {
    std::vector<std::string> list = {"--frames", "2", "--file", path_};
    if (worker) {
      list.insert(list.begin(), "--worker");
    }
    return list;
  }

  Scratch scratch_;
  std::string path_;
  std::string offered_;  // vp_test::offered_extension of the GPU
};

// Through the view, where the loader has a headless surface: MoltenVK has
// one, so it fails on Apple without, and NVIDIA's ICD none, so it skips.
class RunProbeViewTest : public RunProbeTest {
 protected:
  void SetUp() override {
    RunProbeTest::SetUp();
    if (IsSkipped() || HasFatalFailure()) {
      return;
    }
    const core::Result<gfx::windowing::SurfaceSource> headless =
        gfx::windowing::headless_surface();
    if (!headless.ok()) {
#ifdef __APPLE__
      FAIL() << headless.status().message();
#else
      GTEST_SKIP() << headless.status().message();
#endif
    }
  }
};

TEST_F(RunProbeTest, TheWorkerMakesAProgramsHooksOnceAndRunsThemEachFrame) {
  CountingHooks hooks;
  vkp::AppConfig config;
  config.view.make_hooks = hooks.make_hooks();
  const std::string csv = scratch_.file("worker.csv");
  EXPECT_EQ(run({"--worker", "--file", path_, "--csv", csv}, config), 0);
  expect_counted(hooks);
  EXPECT_FALSE(hooks.with_pipeline()) << "the worker alone has no pipeline";
  EXPECT_EQ(logged_run(csv), "vp_probe --worker with the program's make_hooks");
}

TEST_F(RunProbeViewTest, TheViewMakesAProgramsHooksOnceAndRunsThemEachFrame) {
  CountingHooks hooks;
  vkp::AppConfig config;
  config.view.make_hooks = hooks.make_hooks();
  const std::string csv = scratch_.file("view.csv");
  EXPECT_EQ(run({"--file", path_, "--csv", csv}, config), 0);
  expect_counted(hooks);
  EXPECT_TRUE(hooks.with_pipeline()) << "the view's pipeline";
  EXPECT_EQ(logged_run(csv), "vp_probe with the program's make_hooks");
}

// --panel on opens a program's panel from the start, recorded into a pass of
// --size's extent, and the log's run names both; by default it stays closed.
TEST_F(RunProbeViewTest, PanelOnOpensAProgramsPanelAtTheSizeGiven) {
  PanelHooks open;
  vkp::AppConfig config;
  config.view.make_hooks = open.make_hooks();
  const std::string csv = scratch_.file("panel.csv");
  EXPECT_EQ(run({"--frames", "2", "--file", path_, "--panel", "on", "--size",
                 "320x200", "--csv", csv},
                config),
            0);
  EXPECT_GT(open.records(), 0);
  EXPECT_EQ(open.drawable().width, 320u);
  EXPECT_EQ(open.drawable().height, 200u);
  EXPECT_EQ(logged_run(csv),
            "vp_probe --panel on --size 320x200 --frames 2 with the "
            "program's make_hooks");
  PanelHooks closed;
  config.view.make_hooks = closed.make_hooks();
  EXPECT_EQ(run(args(/*worker=*/false), config), 0);
  EXPECT_EQ(closed.records(), 0) << "closed by default";
}

// A program's hooks keep a mesh more of the ring (SkippingHooks), which the
// worker alone, as the view, reserves for them.
TEST_F(RunProbeTest, TheWorkerKeepsAMeshMoreForAProgramsHooks) {
  SkippingHooks hooks;
  vkp::AppConfig config;
  config.view.make_hooks = hooks.make_hooks();
  EXPECT_EQ(run({"--worker", "--file", path_}, config), 0);
  EXPECT_TRUE(hooks.skipped());
  EXPECT_TRUE(hooks.waited()) << "frame 4 not meshed while frame 2 waited";
}

TEST_F(RunProbeViewTest, TheViewKeepsAMeshMoreForAProgramsHooks) {
  SkippingHooks hooks;
  vkp::AppConfig config;
  config.view.make_hooks = hooks.make_hooks();
  EXPECT_EQ(run({"--file", path_}, config), 0);
  EXPECT_TRUE(hooks.skipped());
  EXPECT_TRUE(hooks.waited()) << "frame 4 not meshed while frame 2 waited";
}

TEST_F(RunProbeTest, TheWorkersDeviceEnablesAProgramsOptionalExtension) {
  expect_optional_enabled(/*worker=*/true);
}

TEST_F(RunProbeViewTest, TheViewsDeviceEnablesAProgramsOptionalExtension) {
  expect_optional_enabled(/*worker=*/false);
}

TEST_F(RunProbeTest, WithNoProgramRequirementsTheWorkersDeviceIsGrids) {
  expect_grids_alone(/*worker=*/true);
}

TEST_F(RunProbeViewTest, WithNoProgramRequirementsTheViewsDeviceIsGrids) {
  expect_grids_alone(/*worker=*/false);
}

TEST_F(RunProbeTest, AProgramsRequiredExtensionTheGpuLacksFailsTheWorker) {
  expect_required_refused(/*worker=*/true);
}

TEST_F(RunProbeViewTest, AProgramsRequiredExtensionTheGpuLacksFailsTheView) {
  expect_required_refused(/*worker=*/false);
}

// Refused before the stream opens or the hooks are made; with no program's
// hooks the same command line reaches the stream, which is missing.
TEST(RunProbe, ItsHooksOptionsWithAProgramsMakeHooksExit2) {
  const Scratch scratch;
  const std::string missing = scratch.file("missing.vrst");
  const std::string meshes = scratch.file("meshes");
  CountingHooks hooks;
  vkp::AppConfig config;
  config.view.make_hooks = hooks.make_hooks();
  const std::vector<std::vector<std::string>> cases = {
      {"--latency", "--file", missing},
      {"--worker", "--latency", "--file", missing},
      {"--save-meshes", meshes, "--file", missing},
      {"--worker", "--save-meshes", meshes, "--latency", "--file", missing}};
  for (const std::vector<std::string>& args : cases) {
    EXPECT_EQ(run(args, config), 2) << args.front();
    EXPECT_EQ(run(args, {}), 1) << args.front();
  }
  EXPECT_EQ(run({}, config), 2) << "the usage";
  EXPECT_EQ(hooks.makes(), 0);
}

// The view's settings run_probe plays with: the program's, its make_hooks
// (never made) and device requirements included, with the command line's
// validation, timing, tile depth, labels and vertex sharing, given or its
// defaults; none for the usage.
TEST(RunProbe, TheViewKeepsTheProgramsSettingsUnderTheCommandLines) {
  CountingHooks hooks;
  vkp::AppConfig config;
  config.view.ui_scale = 1.3f;
  config.view.ui_min_frame_height = 44.0f;
  config.view.source_button = true;
  config.view.compact_stats = true;
  config.view.unlit = true;
  config.view.textured = true;
  config.view.device_timing = vkp::DeviceTiming::kOff;
  config.view.tile_depth = 1;
  config.view.debug_labels = false;
  config.view.share_vertices = false;
  config.view.make_hooks = hooks.make_hooks();
  config.view.device_requirements.optional_extensions = {"VK_VP_test_kept"};
  config.view.panel.state.open = true;
  config.view.panel.state.right_share = 0.3f;
#ifdef NDEBUG
  const bool validation = test::requested_validation() != test::Validation::Off;
#else
  const bool validation = true;
#endif
  config.view.validation = !validation;
  const auto expect_programs = [](const vkp::PlayerViewConfig& view) {
    EXPECT_EQ(view.ui_scale, 1.3f);
    EXPECT_EQ(view.ui_min_frame_height, 44.0f);
    EXPECT_TRUE(view.source_button);
    EXPECT_TRUE(view.compact_stats);
    EXPECT_TRUE(view.unlit);
    EXPECT_TRUE(view.textured);
    EXPECT_TRUE(static_cast<bool>(view.make_hooks)) << "make_hooks dropped";
    EXPECT_EQ(view.device_requirements.optional_extensions,
              std::vector<std::string>{"VK_VP_test_kept"});
    EXPECT_EQ(view.panel.state.right_share, 0.3f) << "the panel's share";
  };
  const std::optional<vkp::PlayerViewConfig> defaults =
      view_of({"--worker", "--file", "s.vrst"}, config);
  ASSERT_TRUE(defaults.has_value());
  expect_programs(*defaults);
  EXPECT_EQ(defaults->validation, validation);
  EXPECT_EQ(defaults->device_timing,
            std::optional<vkp::DeviceTiming>(vkp::DeviceTiming::kAll));
  EXPECT_EQ(defaults->tile_depth, 0u);
  EXPECT_TRUE(defaults->debug_labels);
  EXPECT_TRUE(defaults->share_vertices);
  EXPECT_FALSE(defaults->panel.state.open) << "closed unless --panel on";
  const std::optional<vkp::PlayerViewConfig> given =
      view_of({"--file", "s.vrst", "--timing", "sampled", "--tile-depth", "1",
               "--labels", "off", "--share-vertices", "off", "--panel", "on",
               "--size", "3456x2234"},
              config);
  ASSERT_TRUE(given.has_value());
  expect_programs(*given);
  EXPECT_EQ(given->validation, validation);
  EXPECT_EQ(given->device_timing,
            std::optional<vkp::DeviceTiming>(vkp::DeviceTiming::kSampled));
  EXPECT_EQ(given->tile_depth, 1u);
  EXPECT_FALSE(given->debug_labels);
  EXPECT_FALSE(given->share_vertices);
  EXPECT_TRUE(given->panel.state.open);
  EXPECT_FALSE(view_of({}, config).has_value()) << "no source";
  EXPECT_FALSE(
      view_of({"--file", "s.vrst", "--tile-depth", "2"}, config).has_value())
      << "a tile depth it refuses";
  EXPECT_TRUE(
      view_of({"--worker", "--file", "s.vrst", "--panel", "off"}, config)
          .has_value())
      << "a closed panel needs no view";
  for (const std::vector<std::string>& args :
       std::vector<std::vector<std::string>>{
           {"--worker", "--file", "s.vrst", "--panel", "on"},
           {"--worker", "--file", "s.vrst", "--size", "640x480"},
           {"--file", "s.vrst", "--panel", "yes"},
           {"--file", "s.vrst", "--size", "640"},
           {"--file", "s.vrst", "--size", "0x480"},
           {"--file", "s.vrst", "--size", "8x8", "--size", "8x8"}}) {
    EXPECT_FALSE(view_of(args, config).has_value())
        << args[args.size() - 2] << " " << args.back();
  }
  EXPECT_EQ(hooks.makes(), 0);
}

// --live-delay's floor, through probe_view_config's hooks, made on a device
// as the view makes them.
class ProbeLiveDelayTest : public core::test::VulkanDeviceTest {
 protected:
  core::DeviceRequirements requirements() const override {
    return grid::device_requirements();
  }

  // The hooks probe_view_config gives for `args`, made over `config`'s.
  core::Result<vkp::FrameHooks> hooks_of(std::vector<std::string> args,
                                         const vkp::AppConfig& config) {
    const std::optional<vkp::PlayerViewConfig> view =
        view_of(std::move(args), config);
    if (!view || !view->make_hooks) {
      return core::Status::invalid_argument("no hooks");
    }
    const vkp::HookContext context{device(),    allocator(), device(),
                                   allocator(), nullptr,     0,
                                   0,           {},          nullptr};
    return view->make_hooks(context);
  }

  // A program whose own need is `need`, or with no live_delay for none, and
  // a stats line.
  static vkp::AppConfig program(std::optional<std::chrono::nanoseconds> need) {
    vkp::AppConfig config;
    config.view.make_hooks = [need](const vkp::HookContext&) {
      vkp::FrameHooks hooks;
      if (need) {
        hooks.live_delay = [need] { return *need; };
      }
      hooks.stats_line = [] { return std::string("the program's"); };
      return core::Result<vkp::FrameHooks>(std::move(hooks));
    };
    return config;
  }
};

TEST_F(ProbeLiveDelayTest, ItIsAFloorUnderAProgramsNeed) {
  using std::chrono::milliseconds;
  const auto need = [&](const std::string& floor, const vkp::AppConfig& with) {
    core::Result<vkp::FrameHooks> hooks = hooks_of(
        {"--worker", "--connect", "127.0.0.1:47811", "--live-delay", floor},
        with);
    EXPECT_TRUE(hooks.ok()) << hooks.status().message();
    EXPECT_TRUE(hooks.ok() && hooks->live_delay) << "no live_delay";
    return hooks.ok() && hooks->live_delay ? hooks->live_delay()
                                           : std::chrono::nanoseconds(-1);
  };
  EXPECT_EQ(need("150", program(milliseconds(100))), milliseconds(150));
  EXPECT_EQ(need("150", program(milliseconds(200))), milliseconds(200));
  EXPECT_EQ(need("150", program(std::nullopt)), milliseconds(150));
  EXPECT_EQ(need("150", {}), milliseconds(150)) << "no program's hooks";
  EXPECT_EQ(need("0", program(std::nullopt)), milliseconds(0));
  EXPECT_EQ(need("1000", {}), milliseconds(1000));
  // The program's other hooks stay.
  core::Result<vkp::FrameHooks> hooks =
      hooks_of({"--connect", "127.0.0.1", "--live-delay", "150"},
               program(milliseconds(100)));
  ASSERT_TRUE(hooks.ok()) << hooks.status().message();
  ASSERT_TRUE(static_cast<bool>(hooks->stats_line));
  EXPECT_EQ(hooks->stats_line(), "the program's");
  // Without it, the program's hooks are its own.
  hooks = hooks_of({"--connect", "127.0.0.1"}, program(std::nullopt));
  ASSERT_TRUE(hooks.ok()) << hooks.status().message();
  EXPECT_FALSE(static_cast<bool>(hooks->live_delay));
}

// Past a second, below 0, not a number, twice, or for a file: the usage, as
// run_probe exits 2 with it, before the stream opens.
TEST(RunProbe, ALiveDelayOutOfRangeOrForAFileIsTheUsage) {
  const Scratch scratch;
  const std::string missing = scratch.file("missing.vrst");
  for (const std::vector<std::string>& args :
       std::vector<std::vector<std::string>>{
           {"--connect", "127.0.0.1", "--live-delay", "1001"},
           {"--connect", "127.0.0.1", "--live-delay", "-1"},
           {"--connect", "127.0.0.1", "--live-delay", "150ms"},
           {"--connect", "127.0.0.1", "--live-delay", ""},
           {"--connect", "127.0.0.1", "--live-delay", "1", "--live-delay", "2"},
           {"--file", missing, "--live-delay", "150"}}) {
    EXPECT_FALSE(view_of(args, {}).has_value())
        << args[args.size() - 2] << " " << args.back();
    EXPECT_EQ(run(args, {}), 2) << args[args.size() - 2] << " " << args.back();
  }
  EXPECT_TRUE(view_of({"--connect", "127.0.0.1", "--live-delay", "1000"}, {})
                  .has_value());
}

}  // namespace
