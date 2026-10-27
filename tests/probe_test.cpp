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
// view. On the host: --latency and --save-meshes exit 2 when a program set
// make_hooks, which is never made, and without it pass to the stream's
// refusal; and the view's settings run_probe plays with (probe_view_config)
// are the program's, make_hooks included, with the command line's
// validation, timing, tile depth, labels and vertex sharing, its defaults
// included, and none for its usage. The device tests skip with no device,
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

#include "test_stream.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/vulkan_policy.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/gfx/windowing/surface_source.hpp"
#include "volumetric_kit/player/app_config.hpp"
#include "volumetric_kit/player/frame_hooks.hpp"
#include "volumetric_kit/player/player_view.hpp"
#include "volumetric_kit/player/stream_player.hpp"
#include "volumetric_kit/recon/core/device_requirements.hpp"
#include "volumetric_kit/recon/mesh/device_mesh.hpp"
#include "volumetric_kit/recon/volume/voxel_block_grid.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace gfx = volumetric_kit::gfx;
namespace recon = volumetric_kit::recon;
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
        [this](core::CommandBatch&, const recon::volume::VoxelBlockGrid&,
               const recon::mesh::DeviceMesh&, vkp::FrameStamp& stamp) {
          const std::lock_guard<std::mutex> lock(mutex_);
          meshed_.emplace_back(stamp.pass, stamp.index);
          return core::Status();
        };
    hooks.commit = [this](const recon::mesh::DeviceMesh&,
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
        [this](core::CommandBatch&, const recon::volume::VoxelBlockGrid&,
               const recon::mesh::DeviceMesh&, vkp::FrameStamp& stamp) {
          if (stamp.pass == 1 && stamp.index == 4) {
            {
              const std::lock_guard<std::mutex> lock(mutex_);
              meshed_ = true;
            }
            changed_.notify_all();
          }
          return core::Status();
        };
    hooks.commit = [this](const recon::mesh::DeviceMesh&,
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

// On a device that meets recon's requirements, over the tests' stream.
class RunProbeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const core::Result<core::Instance> instance =
        core::Instance::create(test::instance_config(test::Validation::Off));
    const core::Status found =
        instance.ok()
            ? instance->select_physical_device(recon::device_requirements())
                  .status()
            : instance.status();
    if (!found.ok()) {
      if (test::device_required()) {
        FAIL() << "no Vulkan device for this test: " << found.message();
      }
      GTEST_SKIP() << "no Vulkan device for this test: " << found.message();
    }
    path_ = scratch_.copy(VP_TEST_STREAMS, vp_test::kStream);
    ASSERT_FALSE(path_.empty());
  }

  Scratch scratch_;
  std::string path_;
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
// (never made) included, with the command line's validation, timing, tile
// depth, labels and vertex sharing, given or its defaults; none for the
// usage.
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
  const std::optional<vkp::PlayerViewConfig> given =
      view_of({"--file", "s.vrst", "--timing", "sampled", "--tile-depth", "1",
               "--labels", "off", "--share-vertices", "off"},
              config);
  ASSERT_TRUE(given.has_value());
  expect_programs(*given);
  EXPECT_EQ(given->validation, validation);
  EXPECT_EQ(given->device_timing,
            std::optional<vkp::DeviceTiming>(vkp::DeviceTiming::kSampled));
  EXPECT_EQ(given->tile_depth, 1u);
  EXPECT_FALSE(given->debug_labels);
  EXPECT_FALSE(given->share_vertices);
  EXPECT_FALSE(view_of({}, config).has_value()) << "no source";
  EXPECT_FALSE(
      view_of({"--file", "s.vrst", "--tile-depth", "2"}, config).has_value())
      << "a tile depth it refuses";
  EXPECT_EQ(hooks.makes(), 0);
}

}  // namespace
