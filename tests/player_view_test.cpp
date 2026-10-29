// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// PlayerView on a device, over gfx's headless surface and the tests' stream
// (test_stream.hpp), with gfx's ImGuiTouch as the platform half: create
// refuses what it cannot use; frames present and shown() advances, each with
// its record's set, their triangles sharing vertices, or asked not to, three
// of their own each, onto a frame that meshes nothing too, which
// still presents; 0 x 0 skips without blocking; a step moves one frame; the
// worker's failure surfaces from frame(); a tap on Normals switches the mesh's
// flags from lit to normals and back, and frames keep presenting; a tap on
// Deblock while paused shows the frame shown rebuilt, and the stats say so; the
// panel draws at a phone's scale; in portrait and in landscape, at the iOS
// shell's sizes, its controls fit a phone and a fingertip, as does Source,
// clear of them, and so do the compact stats, clear of both, with a program's
// line or none, a live view's too, with the profiler's draw row and its loop's
// in the renderer's table, the draw's device half timing the whole pass; the
// worker and the draw are device-timed while the stats are open, not while they
// are closed, and the worker as a measurement fixes it whatever they show, the
// draw then only while they are open and the timing is not off, each frame
// taken counted once; Source is off by default, and a tap on it is one request;
// ui_wants_pointer() follows a finger on the panel, which controls_test.cpp
// checks on the stats' header and body; each mesh is drawn behind a GPU wait on
// its ready value, reached, with no error the layer reports; a frame drawn is
// stamped from its due time to its present call, and the stats show its
// latency; the renderer's figures are the profiler's while the draw is timed
// (render_metrics), and none while it is not; and teardown mid-playback, the
// profiler attached, is clean. With hooks (frame_hooks.hpp): after_mesh's extra
// reaches the commit and before_draw, and is released only once every display
// frame that drew its frame has run on the GPU; a bound image's set drawn with
// kHybridMeshLit cleared shows the image's value in an offscreen pixel
// (record_mesh), with Texture on, and with it off, as at first, the pixel a
// binding of no set draws, its Texture box shown once a set is bound, flipping
// it and staying; a binding of no set applies with Texture off as on; Unlit,
// from the panel's box or set_unlit, shows the vertex colour unshaded, and off
// it is shaded; a commit hook holds and skips frames; the hooks' stats line is
// the stats' last; and a before_draw failure stops the view. A live view
// presents, on the two images its swapchain asks for (checked on the host).
// A program's device requirements (PlayerViewConfig::device_requirements),
// merged with grid's on the host (worker_requirements): none leave grid's
// alone, and a program's add to grid's, which stay. On the device, none leave
// the device as grid's alone makes it; an optional extension is enabled on the
// hooks' devices where the GPU offers it and skipped where not; grid's stay
// enabled under requirements that leave them out, and frames play; and a
// required extension no GPU offers refuses the view, its hooks never made.
// The view makes its own shared device, validated as the test policy asks, so
// the layer's errors fail the test. The tests skip where the loader offers no
// headless surface (NVIDIA's ICD), but fail on Apple, where MoltenVK has one.
// MoltenVK plans two queue families, so the mesh's buffers are CONCURRENT and
// read across. One family, EXCLUSIVE and read in place, is tested on the host
// only (mesh_wiring_test.cpp) until a leg runs the view on one.

#include "volumetric_kit/player/player_view.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <glm/vec2.hpp>

#include "controls_layout.hpp"
#include "imgui.h"
#include "imgui_internal.h"  // FindWindowByName, to find the panel
#include "program_requirements.hpp"
#include "test_allocation_failure.hpp"
#include "test_stream.hpp"
#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/image.hpp"
#include "volumetric_kit/core/vulkan/shared_device.hpp"
#include "volumetric_kit/core/vulkan/sync.hpp"
#include "volumetric_kit/gfx/app/shared_device_config.hpp"
#include "volumetric_kit/gfx/app/windowed_app.hpp"
#include "volumetric_kit/gfx/core/offscreen_target.hpp"
#include "volumetric_kit/gfx/core/render_target.hpp"
#include "volumetric_kit/gfx/core/texture_upload.hpp"
#include "volumetric_kit/gfx/pipelines/hybrid_mesh_pipeline.hpp"
#include "volumetric_kit/gfx/pipelines/hybrid_mesh_texture_set.hpp"
#include "volumetric_kit/gfx/ui/imgui_touch.hpp"
#include "volumetric_kit/gfx/windowing/frame_loop.hpp"
#include "volumetric_kit/gfx/windowing/surface_source.hpp"
#include "volumetric_kit/grid/base/device_requirements.hpp"
#include "volumetric_kit/grid/mesh/device_mesh.hpp"
#include "volumetric_kit/grid/mesh/mesh.hpp"
#include "volumetric_kit/grid/networking/stream_server.hpp"
#include "volumetric_kit/grid/stream/stream_reader.hpp"
#include "volumetric_kit/grid/volume/voxel_block_grid.hpp"
#include "volumetric_kit/player/controls.hpp"
#include "volumetric_kit/player/frame_hooks.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace gfx = volumetric_kit::gfx;
namespace grid = volumetric_kit::grid;
using namespace std::chrono_literals;
using volumetric_kit::player::DeviceTiming;
using volumetric_kit::player::DrawBinding;
using volumetric_kit::player::FrameCommit;
using volumetric_kit::player::FrameHooks;
using volumetric_kit::player::FrameStamp;
using volumetric_kit::player::HookContext;
using volumetric_kit::player::kControlsWindow;
using volumetric_kit::player::kSourceWindow;
using volumetric_kit::player::kStatsWindow;
using volumetric_kit::player::LiveState;
using volumetric_kit::player::MakeHooks;
using volumetric_kit::player::PlayerView;
using volumetric_kit::player::PlayerViewConfig;
using volumetric_kit::player::StatsPanel;
using volumetric_kit::player::status_line;
using volumetric_kit::player::StreamPlayer;
using volumetric_kit::player::view_swapchain;
using volumetric_kit::player::worker_requirements;
using SteadyClock = std::chrono::steady_clock;

constexpr VkExtent2D kExtent{320, 240};
// The narrowest phone in portrait: 375 x 667 points, at 3 pixels a point.
constexpr float kPhonePixelsPerPoint = 3.0f;
constexpr VkExtent2D kPhone{1125, 2001};
// The shortest with a notch, in landscape: 812 x 375 points, the notch's side
// and the home indicator kept clear (the iPhone 12 mini's insets).
constexpr VkExtent2D kLandscapePhone{2436, 1125};
constexpr gfx::ui::EdgeInsets kLandscapeSafeArea{0.0f, 50.0f, 21.0f, 50.0f};
// A phone's drawable and the insets it keeps clear: those two.
struct Phone {
  VkExtent2D pixels;
  gfx::ui::EdgeInsets safe_area;
};
constexpr Phone kPhones[] = {{kPhone, {}},
                             {kLandscapePhone, kLandscapeSafeArea}};
// The iOS shell's sizes (PlayerViewController.mm).
constexpr float kPhoneScale = 1.3f;
constexpr float kFingertip = 44.0f;
// A program's line (FrameHooks::stats_line) as long as DECISIONS.md says fits
// a phone: 44 characters, its longer part 35.
constexpr const char* kProgramLine =
    "saving · 1,234 meshes in 12.3 s at 61.2 MB/s";
// How long a frame the test let go may take to arrive (the worker builds its
// kernels first); how long one it did not is waited for; and the longest a
// call that must not block takes.
constexpr auto kArrival = 10s;
constexpr auto kQuiet = 300ms;
constexpr auto kPrompt = 1s;

TEST(PlayerViewAllocation, InvalidCreateDiagnosticIsContained) {
  auto& failure = vp_test::allocation_failure;
  failure = {};
  failure.measure = true;
  const auto measured = PlayerView::create({}, kExtent, nullptr);
  failure.measure = false;
  ASSERT_FALSE(measured.ok());
  ASSERT_GT(failure.bytes, 0u);
  failure.armed = true;
  core::Result<std::unique_ptr<PlayerView>> result =
      std::unique_ptr<PlayerView>{};
  bool escaped = false;
  try {
    result = PlayerView::create({}, kExtent, nullptr);
  } catch (const std::bad_alloc&) {
    escaped = true;
  }
  failure.armed = false;
  const bool injected = failure.injected;
  failure = {};
  ASSERT_TRUE(injected);
  ASSERT_FALSE(escaped);
  EXPECT_EQ(result.status().domain(), core::Status::Code::OutOfMemory);
  EXPECT_EQ(PlayerView::create({}, kExtent, nullptr).status().domain(),
            core::Status::Code::InvalidArgument);
}

// --- A program's device requirements, merged on the host --------------------

// Expects requirements `got` to be `want`, field by field; `what` names the
// check in a failure.
void expect_requirements(const core::DeviceRequirements& got,
                         const core::DeviceRequirements& want,
                         const char* what) {
  SCOPED_TRACE(what);
  EXPECT_EQ(got.api_version, want.api_version);
  EXPECT_EQ(got.queue_flags, want.queue_flags);
  EXPECT_EQ(got.needs_present, want.needs_present);
  EXPECT_EQ(got.extensions, want.extensions);
  EXPECT_EQ(got.optional_extensions, want.optional_extensions);
  EXPECT_EQ(std::memcmp(&got.features, &want.features, sizeof got.features), 0);
  EXPECT_EQ(got.timeline_semaphore, want.timeline_semaphore);
  EXPECT_EQ(got.scalar_block_layout, want.scalar_block_layout);
  EXPECT_EQ(got.dynamic_rendering, want.dynamic_rendering);
  EXPECT_EQ(got.feature_chain, want.feature_chain);
}

// A view config's default asks for nothing beyond grid's: the worker's
// device is made with grid's alone, as before a program could add to them.
TEST(WorkerRequirements, NoneFromAProgramAreGridsAlone) {
  const core::Result<core::DeviceRequirements> merged =
      worker_requirements(PlayerViewConfig{}.device_requirements);
  ASSERT_TRUE(merged.ok()) << merged.status().message();
  expect_requirements(*merged, grid::device_requirements(), "the default");
}

// A program's are added to grid's, which stay, though the program's leave
// them out.
TEST(WorkerRequirements, AProgramsAddToGridsWhichStay) {
  core::DeviceRequirements program;
  program.api_version = VK_API_VERSION_1_3;
  program.extensions = {"VK_VP_test_required"};
  program.optional_extensions = {"VK_VP_test_optional"};
  program.features.shaderInt64 = VK_TRUE;
  program.timeline_semaphore = false;
  program.scalar_block_layout = false;
  const core::Result<core::DeviceRequirements> merged =
      worker_requirements(program);
  ASSERT_TRUE(merged.ok()) << merged.status().message();
  core::DeviceRequirements want = grid::device_requirements();
  want.api_version = VK_API_VERSION_1_3;
  want.extensions = program.extensions;
  want.optional_extensions = program.optional_extensions;
  want.features.shaderInt64 = VK_TRUE;
  expect_requirements(*merged, want, "a program's");
  EXPECT_TRUE(merged->scalar_block_layout) << "grid's dropped";
  EXPECT_TRUE(merged->timeline_semaphore) << "grid's dropped";
}

// Hooks with `line` as their stats line; none for an empty one.
MakeHooks line_hooks(const std::string& line) {
  if (line.empty()) {
    return {};
  }
  return [line](const HookContext&) {
    FrameHooks hooks;
    hooks.stats_line = [line] { return line; };
    return core::Result<FrameHooks>(std::move(hooks));
  };
}

class PlayerViewTest : public core::test::VulkanTest {
 protected:
  // The fixture's physical device says whether there is one for the view,
  // which makes its own device.
  core::DeviceRequirements requirements() const override {
    return grid::device_requirements();
  }

  void SetUp() override {
    VulkanTest::SetUp();
    if (base_setup_incomplete()) {
      return;
    }
    core::Result<gfx::windowing::SurfaceSource> headless =
        gfx::windowing::headless_surface();
    if (!headless.ok()) {
#ifdef __APPLE__
      FAIL() << headless.status().message();  // MoltenVK has one
#else
      GTEST_SKIP() << headless.status().message();
#endif
    }
    surface_ = std::move(headless).value();
    path_ = scratch_.copy(VP_TEST_STREAMS, vp_test::kStream);
    ASSERT_FALSE(path_.empty());
  }

  // The view goes while the fixture still counts the layer's errors.
  void TearDown() override {
    touch_.reset();
    view_.reset();
    VulkanTest::TearDown();
  }

  // The view's config: at `ui_scale_` and `ui_min_frame_height_`, with
  // Source while `source_button_`, validated as the test policy asks, Unlit
  // as `unlit_`, Texture as `textured_` and with `make_hooks_`'s hooks and
  // `device_requirements_`.
  PlayerViewConfig config() {
    PlayerViewConfig config;
    config.ui_scale = ui_scale_;
    config.ui_min_frame_height = ui_min_frame_height_;
    config.source_button = source_button_;
    config.validation = active_validation() != core::test::Validation::Off;
    config.compact_stats = compact_stats_;
    config.device_timing = device_timing_;
    config.share_vertices = share_vertices_;
    config.unlit = unlit_;
    config.textured = textured_;
    config.device_requirements = device_requirements_;
    if (make_hooks_) {
      // The context's devices and pipeline noted, for the test's own work on
      // them; they live as long as the view.
      config.make_hooks = [this](const HookContext& context) {
        context_.emplace(
            Context{&context.device, &context.allocator, &context.commit_device,
                    &context.commit_allocator, context.pipeline,
                    context.compute_family, context.graphics_family});
        return make_hooks_(context);
      };
    }
    return config;
  }

  // The view's mesh, drawn again as its last frame drew it
  // (PlayerView::record_mesh) into a `side` x `side` target laid out as its
  // swapchain, on its drawing device, behind the mesh's ready value: the
  // middle pixel, as R, G, B, A; empty with a failure recorded. Needs
  // make_hooks_, for the devices.
  std::vector<int> mesh_pixel(std::uint32_t side = 64) {
    if (!context_) {
      ADD_FAILURE() << "no hooks' context: set make_hooks_";
      return {};
    }
    const gfx::RenderTargetLayout layout = view_->target_layout();
    gfx::OffscreenTargetDesc desc;
    desc.extent = {side, side};
    desc.color_format = layout.color_formats[0];
    desc.depth_format = layout.depth_format;
    core::Result<gfx::OffscreenTarget> made =
        gfx::OffscreenTarget::create(*context_->commit_allocator, desc);
    if (!made.ok()) {
      ADD_FAILURE() << made.status().message();
      return {};
    }
    const gfx::OffscreenTarget& target = made.value();
    std::vector<core::TimelinePoint> waits;
    if (view_->mesh().ready.semaphore != nullptr) {
      waits.push_back(view_->mesh().ready);
    }
    core::Result<core::Device::PendingSubmit> drawn =
        context_->commit_device->submit_pending(
            [&](VkCommandBuffer cmd) {
              target.prepare(cmd);
              gfx::RenderTargetBeginInfo begin;
              begin.clear_color.float32[3] = 1.0f;
              const gfx::RenderTarget pass = target.target();
              pass.begin(cmd, begin);
              view_->record_mesh(cmd, {side, side});
              pass.end(cmd);
              target.record_readback(cmd);
            },
            waits, {});
    if (!drawn.ok()) {
      ADD_FAILURE() << drawn.status().message();
      return {};
    }
    const core::Status waited = drawn->wait();
    if (!waited.ok()) {
      ADD_FAILURE() << waited.message();
      return {};
    }
    const auto* pixels = static_cast<const std::uint8_t*>(target.pixels());
    const std::uint8_t* middle =
        pixels + 4 * (static_cast<std::size_t>(side / 2) * side + side / 2);
    const bool bgra = desc.color_format == VK_FORMAT_B8G8R8A8_SRGB ||
                      desc.color_format == VK_FORMAT_B8G8R8A8_UNORM;
    return bgra ? std::vector<int>{middle[2], middle[1], middle[0], middle[3]}
                : std::vector<int>{middle[0], middle[1], middle[2], middle[3]};
  }

  // Expects the mesh's pixel (mesh_pixel) to be `expected` within 1 code in
  // each of R, G and B; `what` names the check in a failure.
  void expect_mesh_pixel(const std::vector<int>& expected, const char* what) {
    ASSERT_GE(expected.size(), 3u) << what;
    const std::vector<int> pixel = mesh_pixel();
    ASSERT_EQ(pixel.size(), 4u) << what;
    for (std::size_t c = 0; c < 3; ++c) {
      EXPECT_NEAR(pixel[c], expected[c], 1) << what << ", component " << c;
    }
  }

  // Whether pixels `a` and `b` are more than 10 codes apart in R, G or B, so
  // that expect_mesh_pixel tells them apart; false if either has no R, G, B.
  static bool differs(const std::vector<int>& a, const std::vector<int>& b) {
    if (a.size() < 3 || b.size() < 3) {
      return false;
    }
    for (std::size_t c = 0; c < 3; ++c) {
      if (std::abs(a[c] - b[c]) > 10) {
        return true;
      }
    }
    return false;
  }

  // The camera on frame `index`'s sphere (test_stream.hpp: 0.08 m along x a
  // frame, 0.12 m round), 0.6 m off, level, so the sphere fills the middle.
  void aim_at(std::uint64_t index) {
    view_->camera().set_target({0.08f * static_cast<float>(index), 0.0f, 0.0f});
    view_->camera().set_distance(0.6f);
    view_->camera().set_elevation(0.0f);
  }

  // A player over the tests' stream, or null with a failure recorded.
  std::unique_ptr<StreamPlayer> open() {
    core::Result<std::unique_ptr<StreamPlayer>> opened =
        StreamPlayer::open(path_);
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    return opened.ok() ? std::move(opened).value() : nullptr;
  }

  // The view over the stream at `path` on an `extent_` drawable, paused first
  // unless `playing`, with the touch half bound to its controls; false with a
  // failure recorded.
  bool make_view(bool playing = true) { return make_view(path_, playing); }
  bool make_view(const std::string& path, bool playing = true) {
    core::Result<std::unique_ptr<StreamPlayer>> opened =
        StreamPlayer::open(path);
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    if (!opened.ok()) {
      return false;
    }
    if (!playing) {
      opened.value()->pause();
    }
    return view_of(std::move(opened).value());
  }

  // A live view on an `extent_` drawable of `server`, which serves the tests'
  // stream from `reader`, with the touch half bound to its controls, shown
  // each record in turn as `server` publishes it; false with a failure
  // recorded.
  bool make_live_view(grid::stream::StreamReader& reader,
                      grid::networking::StreamServer& server) {
    core::Result<std::unique_ptr<StreamPlayer>> live =
        StreamPlayer::connect({"127.0.0.1", server.port()});
    EXPECT_TRUE(live.ok()) << live.status().message();
    if (!live.ok() || !view_of(std::move(live).value()) || !tick_until([&] {
          return view_->player().live_status().state == LiveState::kConnected;
        })) {
      return false;
    }
    grid::stream::Position pos = reader.begin();
    grid::stream::Record record;
    std::vector<std::uint8_t> payload;
    for (std::uint64_t index = 0;; ++index) {
      const core::Result<bool> more = reader.next(&pos, &record);
      EXPECT_TRUE(more.ok()) << more.status().message();
      if (!more.ok() || !*more) {
        return more.ok();
      }
      const core::Status read = reader.read(record, &payload);
      EXPECT_TRUE(read.ok()) << read.message();
      const core::Status published =
          read.ok()
              ? server.publish(record.info, payload.data(), payload.size())
              : read;
      EXPECT_TRUE(published.ok()) << published.message();
      if (!published.ok() || !tick_until([&] {
            return view_->shown().has_value() && view_->shown()->index == index;
          })) {
        ADD_FAILURE() << "record " << index << " not shown";
        return false;
      }
    }
  }

  // The view of `player` on an `extent_` drawable, with the touch half bound
  // to its controls; false with a failure recorded.
  bool view_of(std::unique_ptr<StreamPlayer> player) {
    core::Result<std::unique_ptr<PlayerView>> view =
        PlayerView::create(surface_, extent_, std::move(player), config());
    EXPECT_TRUE(view.ok()) << view.status().message();
    if (!view.ok()) {
      return false;
    }
    view_ = std::move(view).value();
    core::Result<std::unique_ptr<gfx::ui::ImGuiTouch>> touch =
        gfx::ui::ImGuiTouch::create(view_->ui_context());
    EXPECT_TRUE(touch.ok()) << touch.status().message();
    touch_ = touch.ok() ? std::move(touch).value() : nullptr;
    return touch_ != nullptr;
  }

  // At the iOS shell's sizes (PlayerViewController.mm), with Source and the
  // compact stats, as on a phone.
  void size_as_a_phone() {
    pixels_per_point_ = kPhonePixelsPerPoint;
    ui_scale_ = kPhoneScale;
    ui_min_frame_height_ = kFingertip;
    source_button_ = true;
    compact_stats_ = true;
  }

  // The compact stats on a phone, as the last frame laid them out: the
  // panel, its GPU and latency line included, fits across, ends above the
  // controls and starts below Source, and its header is a fingertip tall,
  // left of Source.
  void expect_stats_fit() {
    ImGui::SetCurrentContext(view_->ui_context());
    const ImGuiWindow* const header = ImGui::FindWindowByName(kStatsWindow);
    ImGuiWindow* const body = stats_body();
    const ImGuiWindow* const controls =
        ImGui::FindWindowByName(kControlsWindow);
    const ImGuiWindow* const source = ImGui::FindWindowByName(kSourceWindow);
    ASSERT_NE(header, nullptr);
    ASSERT_NE(body, nullptr);
    ASSERT_NE(controls, nullptr);
    ASSERT_NE(source, nullptr);
    // The widths ImGui fits the body and the header to.
    EXPECT_LE(body->ContentSize.x + 2.0f * body->WindowPadding.x,
              widest_panel());
    EXPECT_LE(header->ContentSize.x + 2.0f * header->WindowPadding.x,
              widest_panel());
    EXPECT_LE(body->Pos.y + body->Size.y, controls->Pos.y);
    EXPECT_LE(source->Pos.y + source->Size.y, body->Pos.y);
    EXPECT_LE(header->Pos.x + header->Size.x, source->Pos.x);
    EXPECT_GE(header->Size.y, kFingertip);
  }

  // One tick, as a shell makes it: the touch half's frame, `safe_area_` kept
  // clear, then the view's.
  core::Result<bool> tick() {
    seconds_ += 1.0 / 60.0;
    gfx::ui::DisplayFrame display;
    display.pixels = extent_;
    display.pixels_per_point = pixels_per_point_;
    display.safe_area = safe_area_;
    display.seconds = seconds_;
    touch_->new_frame(display);
    return view_->frame(extent_);
  }

  // Ticks until `done` holds or `wait` passes, each tick OK; whether `done`
  // held.
  template <typename Done>
  bool tick_until(Done&& done, SteadyClock::duration wait = kArrival) {
    const SteadyClock::time_point deadline = SteadyClock::now() + wait;
    while (!done()) {
      if (SteadyClock::now() >= deadline) {
        return false;
      }
      const core::Result<bool> ticked = tick();
      EXPECT_TRUE(ticked.ok()) << ticked.status().message();
      if (!ticked.ok()) {
        return false;
      }
      std::this_thread::sleep_for(1ms);
    }
    return true;
  }

  // Ticks for `wait`, each tick OK.
  void tick_for(SteadyClock::duration wait) {
    tick_until([] { return false; }, wait);
  }

  // Ticks until one presents, each tick OK; whether one did.
  bool present() {
    const SteadyClock::time_point deadline = SteadyClock::now() + kArrival;
    while (SteadyClock::now() < deadline) {
      const core::Result<bool> ticked = tick();
      EXPECT_TRUE(ticked.ok()) << ticked.status().message();
      if (!ticked.ok() || ticked.value()) {
        return ticked.ok();
      }
    }
    return false;
  }

  // The middle of the controls' panel, in points, as the last frame laid it
  // out.
  glm::vec2 panel_middle() const {
    ImGui::SetCurrentContext(view_->ui_context());
    const ImGuiWindow* panel = ImGui::FindWindowByName(kControlsWindow);
    EXPECT_NE(panel, nullptr);
    if (panel == nullptr) {
      return {0.0f, 0.0f};
    }
    return {panel->Pos.x + 0.5f * panel->Size.x,
            panel->Pos.y + 0.5f * panel->Size.y};
  }

  // The stats' body window, which gfx names after the header; null before it
  // is first drawn. The view's context is current.
  static ImGuiWindow* stats_body() {
    return ImGui::FindWindowByName(
        (std::string(kStatsWindow) + "##body").c_str());
  }

  // Whether the view's UI wants a finger at `at`, in points, as of the frame
  // it lands in. The finger then goes, pressing nothing.
  bool wants_pointer_at(glm::vec2 at) {
    touch_->touch_began(1, at);
    EXPECT_TRUE(present());
    const bool wants = view_->ui_wants_pointer();
    touch_->touch_cancelled(1);
    for (int i = 0; i < 2; ++i) {
      EXPECT_TRUE(present());
    }
    return wants;
  }

  // A finger lifts where it lands, and the frames ImGui takes the tap over
  // present: its position, its press, its release, and its position cleared.
  void tap(glm::vec2 at) {
    touch_->touch_began(1, at);
    touch_->touch_ended(1);
    for (int i = 0; i < 4; ++i) {
      EXPECT_TRUE(present());
    }
  }

  // A tap on the stats' header, which closes or opens them, as the last
  // frame laid it out.
  void tap_stats_header() {
    ImGui::SetCurrentContext(view_->ui_context());
    const ImGuiWindow* const header = ImGui::FindWindowByName(kStatsWindow);
    ASSERT_NE(header, nullptr);
    tap({header->Pos.x + 0.5f * header->Size.x,
         header->Pos.y + 0.5f * header->Size.y});
  }

  // Whether the renderer's table holds the draw's row, as of its last
  // rebuild.
  bool draw_row_shown() const {
    for (const core::StageRow& row : view_->stats_panel().render_rows()) {
      if (std::string(row.name) == "draw (mesh+UI)") {
        return true;
      }
    }
    return false;
  }

  // The rows of the stats' table `id`, "stages" or "render", after its
  // header, as the last frame drew it; -1 with no table.
  static int stats_rows(const char* id) {
    const ImGuiTable* const table = vp_test::stats_table(id);
    return table != nullptr ? table->CurrentRow : -1;
  }

  // The ImGui item under a finger at `at`, in points, as the frame it lands in
  // sees it; 0 for none. The finger then goes, pressing nothing.
  ImGuiID item_under(glm::vec2 at) {
    touch_->touch_began(1, at);
    EXPECT_TRUE(present());
    ImGui::SetCurrentContext(view_->ui_context());
    const ImGuiID item = ImGui::GetCurrentContext()->HoveredId;
    // ImGui takes the press a frame after the position, then the release with
    // the position cleared, off every control.
    touch_->touch_cancelled(1);
    for (int i = 0; i < 2; ++i) {
      EXPECT_TRUE(present());
    }
    return item;
  }

  // The widest ImGui lets an auto-sized panel grow, in points: the work area
  // less its safe-area padding on each side. The view's context is current.
  static float widest_panel() {
    return ImGui::GetMainViewport()->WorkSize.x -
           2.0f * ImGui::GetStyle().DisplaySafeAreaPadding.x;
  }

  vp_test::Scratch scratch_;
  std::string path_;  // the test's copy of the stream
  gfx::windowing::SurfaceSource surface_;
  std::unique_ptr<PlayerView> view_;
  std::unique_ptr<gfx::ui::ImGuiTouch> touch_;  // destroyed before the view
  double seconds_ = 0.0;
  // Set before make_view.
  VkExtent2D extent_ = kExtent;
  float pixels_per_point_ = 1.0f;
  float ui_scale_ = 1.0f;
  float ui_min_frame_height_ = 0.0f;
  bool source_button_ = PlayerViewConfig{}.source_button;
  bool compact_stats_ = PlayerViewConfig{}.compact_stats;
  std::optional<DeviceTiming> device_timing_;
  bool share_vertices_ = PlayerViewConfig{}.share_vertices;
  bool unlit_ = PlayerViewConfig{}.unlit;
  bool textured_ = PlayerViewConfig{}.textured;
  MakeHooks make_hooks_;  // the view's hooks, with the context noted
  // The program's: as the view's config has them unless a test sets them.
  core::DeviceRequirements device_requirements_ =
      PlayerViewConfig{}.device_requirements;
  // What the hooks were made with, as the view's create gave it.
  struct Context {
    core::Device* device;
    core::Allocator* allocator;
    core::Device* commit_device;
    core::Allocator* commit_allocator;
    const gfx::pipelines::HybridMeshPipeline* pipeline;
    std::uint32_t compute_family;
    std::uint32_t graphics_family;
  };
  std::optional<Context> context_;
  gfx::ui::EdgeInsets safe_area_;
};

TEST_F(PlayerViewTest, CreateRefusesWhatItCannotUse) {
  const core::Status no_player =
      PlayerView::create(surface_, kExtent, nullptr, config()).status();
  EXPECT_EQ(no_player.domain(), core::Status::Code::InvalidArgument);
  EXPECT_NE(no_player.message().find("needs a player"), std::string::npos)
      << no_player.message();

  const core::Status no_pixels =
      PlayerView::create(surface_, {kExtent.width, 0}, open(), config())
          .status();
  EXPECT_EQ(no_pixels.domain(), core::Status::Code::InvalidArgument);
  EXPECT_NE(no_pixels.message().find("at least 1 x 1"), std::string::npos)
      << no_pixels.message();

  PlayerViewConfig unscaled = config();
  unscaled.ui_scale = 0.0f;
  const core::Status no_scale =
      PlayerView::create(surface_, kExtent, open(), unscaled).status();
  EXPECT_EQ(no_scale.domain(), core::Status::Code::InvalidArgument);
  EXPECT_NE(no_scale.message().find("scale"), std::string::npos)
      << no_scale.message();

  PlayerViewConfig sunken = config();
  sunken.ui_min_frame_height = -1.0f;
  const core::Status no_height =
      PlayerView::create(surface_, kExtent, open(), sunken).status();
  EXPECT_EQ(no_height.domain(), core::Status::Code::InvalidArgument);
  EXPECT_NE(no_height.message().find("min_frame_height"), std::string::npos)
      << no_height.message();

  // start refuses a stopped player, and the view goes with no worker.
  std::unique_ptr<StreamPlayer> stopped = open();
  ASSERT_NE(stopped, nullptr);
  stopped->stop();
  const core::Status not_started =
      PlayerView::create(surface_, kExtent, std::move(stopped), config())
          .status();
  EXPECT_EQ(not_started.domain(), core::Status::Code::InvalidArgument);
  EXPECT_NE(not_started.message().find("already started or stopped"),
            std::string::npos)
      << not_started.message();
}

TEST_F(PlayerViewTest, FramesPresentAndShownAdvances) {
  ASSERT_TRUE(make_view());
  std::uint64_t presented = 0;
  std::vector<std::uint64_t> shown;
  const bool advanced = tick_until([&] {
    if (view_->shown() &&
        (shown.empty() || shown.back() != view_->shown()->index)) {
      shown.push_back(view_->shown()->index);
    }
    return shown.size() >= 3;
  });
  ASSERT_TRUE(advanced) << shown.size() << " frame(s) shown";
  // In order, from the first; the loop wraps only after kFrames.
  EXPECT_EQ(shown, (std::vector<std::uint64_t>{0, 1, 2}));
  EXPECT_EQ(view_->shown()->set, vp_test::set_of(2)) << "its record's";
  // Its triangles share their vertices by default (mesh_wiring): fewer than
  // 3 each.
  const grid::mesh::DeviceMesh& mesh = view_->mesh();
  EXPECT_TRUE(mesh.shares_vertices);
  EXPECT_GT(mesh.triangle_count, 0u);
  EXPECT_LT(mesh.vertex_count, 3 * mesh.triangle_count);
  for (int i = 0; i < 3; ++i) {
    const core::Result<bool> ticked = tick();
    ASSERT_TRUE(ticked.ok()) << ticked.status().message();
    presented += ticked.value() ? 1 : 0;
  }
  EXPECT_EQ(presented, 3u) << "a headless surface never settles";
  EXPECT_TRUE(view_->player().status().ok())
      << view_->player().status().message();
}

// Asked not to share them (PlayerViewConfig::share_vertices), each triangle
// has three vertices of its own, and frames present as before.
TEST_F(PlayerViewTest, UnsharedEachTriangleHasThreeVerticesOfItsOwn) {
  share_vertices_ = false;
  ASSERT_TRUE(make_view());
  ASSERT_TRUE(tick_until([&] {
    return view_->shown() && view_->shown()->index >= 1 &&
           view_->mesh().triangle_count > 0;
  }));
  const grid::mesh::DeviceMesh& mesh = view_->mesh();
  EXPECT_FALSE(mesh.shares_vertices);
  EXPECT_EQ(mesh.vertex_count, 3 * mesh.triangle_count);
  ASSERT_TRUE(present());
  EXPECT_TRUE(view_->player().status().ok())
      << view_->player().status().message();
}

TEST_F(PlayerViewTest, ShownMovesOntoAFrameThatMeshesNothing) {
  const std::string path =
      scratch_.copy(VP_TEST_STREAMS, vp_test::kEmptyFrameStream);
  ASSERT_FALSE(path.empty());
  ASSERT_TRUE(make_view(path, /*playing=*/false));
  view_->player().step();
  ASSERT_TRUE(tick_until([&] { return view_->shown().has_value(); }));
  EXPECT_EQ(view_->shown()->index, 0u);
  // Frame 1 draws nothing, and the read-out moves onto it.
  view_->player().step();
  ASSERT_TRUE(tick_until([&] { return view_->shown()->index != 0; }))
      << "shown() stayed on frame 0";
  EXPECT_EQ(view_->shown()->index, 1u);
  EXPECT_EQ(view_->shown()->pts_ns, vp_test::pts_of(1));
  ASSERT_TRUE(present());  // drawing nothing, the frame still presents
  // The frame after it draws again.
  view_->player().step();
  ASSERT_TRUE(tick_until([&] { return view_->shown()->index != 1; }));
  EXPECT_EQ(view_->shown()->index, 2u);
  EXPECT_TRUE(view_->player().status().ok())
      << view_->player().status().message();
}

TEST_F(PlayerViewTest, AZeroExtentSkipsWithoutBlocking) {
  ASSERT_TRUE(make_view());
  ASSERT_TRUE(tick_until([&] { return view_->shown().has_value(); }));
  const SteadyClock::time_point start = SteadyClock::now();
  const core::Result<bool> skipped = view_->frame({0, 0});
  EXPECT_LT(SteadyClock::now() - start, kPrompt);
  ASSERT_TRUE(skipped.ok()) << skipped.status().message();
  EXPECT_FALSE(skipped.value());
  // Restored, it presents again.
  const core::Result<bool> restored = tick();
  ASSERT_TRUE(restored.ok()) << restored.status().message();
  EXPECT_TRUE(restored.value());
}

TEST_F(PlayerViewTest, AStepMovesOneFrame) {
  ASSERT_TRUE(make_view(/*playing=*/false));
  tick_for(kQuiet);
  EXPECT_FALSE(view_->shown().has_value()) << "paused, nothing goes";
  view_->player().step();
  ASSERT_TRUE(tick_until([&] { return view_->shown().has_value(); }));
  EXPECT_EQ(view_->shown()->index, 0u);
  tick_for(kQuiet);
  EXPECT_EQ(view_->shown()->index, 0u) << "one step, one frame";
  view_->player().step();
  ASSERT_TRUE(tick_until([&] { return view_->shown()->index != 0; }));
  EXPECT_EQ(view_->shown()->index, 1u);
  EXPECT_FALSE(view_->player().playing());
}

TEST_F(PlayerViewTest, TheWorkersFailureSurfacesFromFrame) {
  const std::string undecodable = scratch_.file("undecodable.vrst");
  const core::Status written =
      vp_test::write_undecodable_stream(undecodable, 3);
  ASSERT_TRUE(written.ok()) << written.message();
  ASSERT_TRUE(make_view(undecodable));
  core::Status failed;
  const SteadyClock::time_point deadline = SteadyClock::now() + kArrival;
  while (failed.ok() && SteadyClock::now() < deadline) {
    failed = tick().status();
    std::this_thread::sleep_for(1ms);
  }
  ASSERT_FALSE(failed.ok()) << "frame() kept presenting";
  EXPECT_EQ(failed.message(), view_->player().status().message());
  EXPECT_NE(failed.message().find("no frame of the geometry track decodes"),
            std::string::npos)
      << failed.message();
  EXPECT_FALSE(view_->shown().has_value());
}

// Lit at first; a tap on Normals, the row's last control, flips normals() and
// mesh_flags() to the normals', and frames keep presenting; another flips them
// back, and set_normals does as a tap does. Whether frame()'s draw takes
// mesh_flags() is not checked, as gfx's swapchain images are colour
// attachments only and cannot be read back; the normals' colours are gfx's to
// test.
TEST_F(PlayerViewTest, ATapOnNormalsSwitchesTheMeshsFlags) {
  using gfx::pipelines::kHybridMeshLit;
  using gfx::pipelines::kHybridMeshNormals;
  ASSERT_TRUE(make_view());
  ASSERT_TRUE(tick_until([&] { return view_->shown().has_value(); }));
  EXPECT_FALSE(view_->normals());
  EXPECT_EQ(view_->mesh_flags(), kHybridMeshLit);
  ImGui::SetCurrentContext(view_->ui_context());
  const ImGuiWindow* const panel = ImGui::FindWindowByName(kControlsWindow);
  ASSERT_NE(panel, nullptr);
  const glm::vec2 normals = vp_test::control_middle(*panel, 3);
  tap(normals);
  EXPECT_TRUE(view_->normals());
  EXPECT_EQ(view_->mesh_flags(), kHybridMeshNormals);
  const std::uint64_t before = view_->shown()->index;
  ASSERT_TRUE(tick_until([&] { return view_->shown()->index != before; }))
      << "no frame drawn in normals";
  tap(normals);
  EXPECT_FALSE(view_->normals());
  EXPECT_EQ(view_->mesh_flags(), kHybridMeshLit);
  view_->set_normals(true);
  EXPECT_EQ(view_->mesh_flags(), kHybridMeshNormals);
  EXPECT_TRUE(present());
  EXPECT_TRUE(view_->player().playing()) << "the taps pressed Normals alone";
}

// The true-SDF stream, which the deblock runs on, paused on frame 0, none of
// whose blocks it keeps exact, as the stats say: a tap turns the deblock
// off, and the worker's rebuild of frame 0 is drawn; the stats, past a
// refresh, say it is off.
TEST_F(PlayerViewTest, ATapOnDeblockWhilePausedRebuildsTheFrameShown) {
  const std::string path =
      scratch_.copy(VP_TEST_STREAMS, vp_test::kTrueSdfStream);
  ASSERT_FALSE(path.empty());
  ASSERT_TRUE(make_view(path, /*playing=*/false));
  view_->player().step();
  ASSERT_TRUE(tick_until([&] { return view_->shown().has_value(); }));
  EXPECT_TRUE(view_->shown()->deblock_on);
  EXPECT_STREQ(view_->shown()->deblock.filter, "R");
  // Under 1,000 blocks, so their count has no thousands to group.
  const std::uint32_t blocks = view_->shown()->deblock.block_count;
  EXPECT_GT(blocks, 0u);
  EXPECT_LT(blocks, 1'000u);
  EXPECT_EQ(view_->shown()->deblock.exact_blocks, 0u);
  tick_for(2 * StatsPanel::kRefresh);
  const std::vector<std::string> before = view_->stats_panel().lines();
  ASSERT_EQ(before.size(), 6u);
  EXPECT_EQ(before[2], "deblock R · T 1.962 mm · 0 of " +
                           std::to_string(blocks) + " blocks exact");
  ImGui::SetCurrentContext(view_->ui_context());
  const ImGuiWindow* const panel = ImGui::FindWindowByName(kControlsWindow);
  ASSERT_NE(panel, nullptr);
  tap(vp_test::control_middle(*panel, vp_test::kDeblock));
  EXPECT_FALSE(view_->player().deblock());
  ASSERT_TRUE(tick_until([&] { return view_->shown()->rebuild; }))
      << "no rebuild drawn";
  EXPECT_EQ(view_->shown()->index, 0u);
  EXPECT_FALSE(view_->shown()->deblock_on);
  tick_for(2 * StatsPanel::kRefresh);
  const std::vector<std::string> after = view_->stats_panel().lines();
  ASSERT_EQ(after.size(), 6u);
  EXPECT_EQ(after[2], "deblock off · T 1.962 mm");
  EXPECT_FALSE(view_->player().playing());
  EXPECT_TRUE(view_->player().status().ok())
      << view_->player().status().message();
}

TEST_F(PlayerViewTest, UiWantsPointerFollowsAFingerOnThePanel) {
  ASSERT_TRUE(make_view(/*playing=*/false));
  // An auto-sized panel takes its size on its second frame.
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(present());
  }
  EXPECT_FALSE(view_->ui_wants_pointer());
  touch_->touch_began(1, panel_middle());
  EXPECT_FALSE(view_->ui_wants_pointer()) << "not before the next frame";
  ASSERT_TRUE(present());
  EXPECT_TRUE(view_->ui_wants_pointer());
  touch_->touch_ended(1);
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(present());
  }
  EXPECT_FALSE(view_->ui_wants_pointer());
  // The top-right corner: the scene.
  touch_->touch_began(2, {static_cast<float>(kExtent.width) - 4.0f, 4.0f});
  ASSERT_TRUE(present());
  EXPECT_FALSE(view_->ui_wants_pointer());
  touch_->touch_ended(2);
}

// A phone's scale takes ImGui's vector font rather than its pixel one, which
// must also hold the status line's dot.
TEST_F(PlayerViewTest, ThePanelDrawsAtAPhonesScale) {
  ui_scale_ = kPhoneScale;
  ASSERT_TRUE(make_view(/*playing=*/false));
  EXPECT_TRUE(present());
  ImGui::SetCurrentContext(view_->ui_context());
  const ImVector<ImFont*>& fonts = ImGui::GetIO().Fonts->Fonts;
  ASSERT_FALSE(fonts.empty()) << "the first frame adds the font";
  EXPECT_STREQ(fonts[0]->GetDebugName(), "ProggyForever.ttf");
  EXPECT_TRUE(fonts[0]->IsGlyphInFont(0x00B7));
}

// On the narrowest phone in portrait and the shortest with a notch in
// landscape, at the iOS shell's sizes, Source included: Deblock is below the
// row in portrait and on it in landscape; a fingertip's square at each
// control's top left and at Source's lands on it, as does Source's far edge;
// "Pause", "Normals" and "Source" fit their buttons; the panel fits across,
// both its row and the widest status line, a 5-digit track's playing in a
// loop; and Source ends above it, so the two never overlap.
TEST_F(PlayerViewTest, TheControlsFitAPhoneAndAFingertip) {
  struct Target {
    ImGuiID id;
    glm::vec2 corner;
  };
  pixels_per_point_ = kPhonePixelsPerPoint;
  ui_scale_ = kPhoneScale;
  ui_min_frame_height_ = kFingertip;
  source_button_ = true;
  for (const Phone& phone : kPhones) {
    SCOPED_TRACE(phone.pixels.width);
    touch_.reset();
    view_.reset();
    extent_ = phone.pixels;
    safe_area_ = phone.safe_area;
    ASSERT_TRUE(make_view(/*playing=*/false));
    for (int i = 0; i < 3; ++i) {
      ASSERT_TRUE(present());
    }
    ImGui::SetCurrentContext(view_->ui_context());
    ImGuiWindow* const panel = ImGui::FindWindowByName(kControlsWindow);
    ImGuiWindow* const source = ImGui::FindWindowByName(kSourceWindow);
    ASSERT_NE(panel, nullptr);
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(vp_test::deblock_on_row(*panel),
              phone.pixels.width == kLandscapePhone.width);
    // The width ImGui fits the panel to. Before the first frame the status
    // line says "loading", so it fits the row.
    const float padding = 2.0f * panel->WindowPadding.x;
    EXPECT_LE(panel->ContentSize.x + padding, widest_panel());
    FrameStamp last;
    last.index = 99'998;
    const std::string widest_line = status_line(last, 99'999, true, true);
    EXPECT_LE(vp_test::text_width(*panel, widest_line) + padding,
              widest_panel());
    // Every button is kButtonEms font sizes wide (control_size).
    const float label_padding = 2.0f * ImGui::GetStyle().FramePadding.x;
    for (const char* label : {"Pause", "Normals", "Source"}) {
      EXPECT_GE(vp_test::control_size(*panel, 0).x,
                vp_test::text_width(*panel, label) + label_padding)
          << label;
    }
    const Target targets[] = {
        {panel->GetID("###play"), vp_test::control_top_left(*panel, 0)},
        {panel->GetID("Step"), vp_test::control_top_left(*panel, 1)},
        {panel->GetID("Loop"), vp_test::control_top_left(*panel, 2)},
        {panel->GetID("Normals"), vp_test::control_top_left(*panel, 3)},
        {panel->GetID("Deblock"),
         vp_test::control_top_left(*panel, vp_test::kDeblock)},
        {source->GetID("Source"), vp_test::source_top_left(*source)}};
    for (int i = 0; i < 6; ++i) {
      for (const float dx : {0.5f, kFingertip - 0.5f}) {
        for (const float dy : {0.5f, kFingertip - 0.5f}) {
          EXPECT_EQ(item_under(targets[i].corner + glm::vec2(dx, dy)),
                    targets[i].id)
              << "control " << i << " at +" << dx << ", +" << dy;
        }
      }
    }
    // Source's far corner, so it is as wide as the labels' check takes it to
    // be, less the half point its window's whole-point width drops.
    EXPECT_EQ(item_under(vp_test::source_top_left(*source) +
                         glm::vec2(vp_test::control_size(*source, 0).x - 1.0f,
                                   kFingertip - 0.5f)),
              targets[5].id);
    EXPECT_LE(source->Pos.y + source->Size.y, panel->Pos.y);
    // The fingers pressed nothing.
    EXPECT_FALSE(view_->player().playing());
    EXPECT_TRUE(view_->player().loop());
    EXPECT_FALSE(view_->normals());
    EXPECT_TRUE(view_->player().deblock());
    EXPECT_FALSE(view_->take_source_request());
    EXPECT_FALSE(view_->shown().has_value());
  }
}

// As the desktop's: no Source, so a finger at the top right is the scene's,
// and a tap there asks for nothing.
TEST_F(PlayerViewTest, SourceIsOffByDefault) {
  ASSERT_TRUE(make_view(/*playing=*/false));
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(present());
  }
  ImGui::SetCurrentContext(view_->ui_context());
  EXPECT_EQ(ImGui::FindWindowByName(kSourceWindow), nullptr);
  const glm::vec2 top_right(static_cast<float>(kExtent.width) - 4.0f, 4.0f);
  EXPECT_FALSE(wants_pointer_at(top_right));
  tap(top_right);
  EXPECT_FALSE(view_->take_source_request());
}

// As the iOS shell's: a tap on Source is one request, taken once.
TEST_F(PlayerViewTest, ATapOnSourceIsOneRequest) {
  source_button_ = true;
  ASSERT_TRUE(make_view(/*playing=*/false));
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(present());
  }
  EXPECT_FALSE(view_->take_source_request());
  ImGui::SetCurrentContext(view_->ui_context());
  const ImGuiWindow* const source = ImGui::FindWindowByName(kSourceWindow);
  ASSERT_NE(source, nullptr);
  tap(vp_test::source_middle(*source));
  EXPECT_TRUE(view_->take_source_request());
  EXPECT_FALSE(view_->take_source_request()) << "taken";
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(present());
  }
  EXPECT_FALSE(view_->take_source_request()) << "one tap, one request";
  EXPECT_FALSE(view_->player().playing());
}

// On the narrowest phone in portrait and the shortest with a notch in
// landscape, at the iOS shell's sizes, Source and the compact stats included,
// playing, past a refresh so the stats hold a frame's figures, with no
// program's line and with kProgramLine: their tables have the worker's three
// stages and the renderer's draw row and its loop's frame wait and present,
// and they fit the phone (expect_stats_fit).
TEST_F(PlayerViewTest, TheStatsFitAPhone) {
  size_as_a_phone();
  for (const std::string line : {"", kProgramLine}) {
    make_hooks_ = line_hooks(line);
    for (const Phone& phone : kPhones) {
      SCOPED_TRACE(std::to_string(phone.pixels.width) + " \"" + line + "\"");
      touch_.reset();
      view_.reset();
      extent_ = phone.pixels;
      safe_area_ = phone.safe_area;
      ASSERT_TRUE(make_view());
      ASSERT_TRUE(tick_until([&] { return view_->shown().has_value(); }));
      tick_for(2 * StatsPanel::kRefresh);
      ImGui::SetCurrentContext(view_->ui_context());
      EXPECT_EQ(stats_rows("stages"), 3);
      EXPECT_EQ(stats_rows("render"), 3);
      expect_stats_fit();
    }
  }
}

// A live view's compact stats on the phones of TheStatsFitAPhone, the tests'
// stream served from 127.0.0.1 and shown record by record, past a refresh,
// with no program's line and with kProgramLine: they fit the phone.
TEST_F(PlayerViewTest, ALiveViewsStatsFitAPhone) {
  size_as_a_phone();
  core::Result<std::unique_ptr<grid::stream::StreamReader>> reader =
      grid::stream::StreamReader::open(path_);
  ASSERT_TRUE(reader.ok()) << reader.status().message();
  for (const std::string line : {"", kProgramLine}) {
    make_hooks_ = line_hooks(line);
    for (const Phone& phone : kPhones) {
      SCOPED_TRACE(std::to_string(phone.pixels.width) + " \"" + line + "\"");
      extent_ = phone.pixels;
      safe_area_ = phone.safe_area;
      grid::networking::ServerOptions options;
      options.host = "127.0.0.1";
      options.port = 0;  // a free one: other tests serve at the same time
      core::Result<std::unique_ptr<grid::networking::StreamServer>> server =
          grid::networking::StreamServer::start((*reader)->session(), options);
      ASSERT_TRUE(server.ok()) << server.status().message();
      ASSERT_TRUE(make_live_view(**reader, **server));
      tick_for(2 * StatsPanel::kRefresh);
      expect_stats_fit();
      // The view goes before its sender.
      touch_.reset();
      view_.reset();
    }
  }
}

// Playing at a phone's size, where the queue can time: the draw row's device
// half spans the whole pass, so the mean of five refreshes reads the pass,
// not the about 0 that a scope inside it reads on Apple GPUs. MoltenVK on an
// M5 Max, under other tests' load too: inside, 0.002 to 0.01 ms, at times
// one spike to 1 ms; around it, mostly 0.02 to 0.2 ms, at times as low as
// 0.002. The mean, unlike the median, a run of low readings does not sink,
// but a spike lifts it: it misses a scope inside about 1 run in 20.
TEST_F(PlayerViewTest, TheDrawRowTimesTheWholePass) {
  extent_ = kPhone;
  ASSERT_TRUE(make_view());
  ASSERT_TRUE(tick_until([&] { return view_->shown().has_value(); }));
  double sum_ms = 0.0;
  int readings = 0;
  for (int i = 0; i < 5; ++i) {
    tick_for(StatsPanel::kRefresh);
    for (const core::StageRow& row : view_->stats_panel().render_rows()) {
      if (std::string(row.name) == "draw (mesh+UI)" &&
          row.device == core::DeviceTime::Measured) {
        sum_ms += row.gpu_ms;
        ++readings;
      }
    }
  }
  if (readings == 0) {
    return;  // the graphics queue cannot time
  }
  EXPECT_GT(sum_ms / readings, 0.012) << "ms, the mean";
}

// The worker's device and the draw are timed while the stats are open, from
// the first frame, and not while they are closed: a frame's stamp says which,
// and only a timed one's rows have device halves; the renderer's table holds
// the draw, and on reopening is empty, the profiler having been detached,
// until its next rebuild.
TEST_F(PlayerViewTest, TheWorkerAndTheDrawAreTimedWhileTheStatsAreOpen) {
  ASSERT_TRUE(make_view());
  ASSERT_TRUE(tick_until([&] { return view_->shown().has_value(); }));
  EXPECT_TRUE(view_->shown()->timed);
  EXPECT_EQ(view_->player().device_timing(), DeviceTiming::kAll);
  tick_for(2 * StatsPanel::kRefresh);
  EXPECT_TRUE(draw_row_shown());
  EXPECT_FALSE(view_->render_metrics().sections.empty());
  ASSERT_NO_FATAL_FAILURE(tap_stats_header());
  ASSERT_FALSE(view_->stats_panel().open());
  EXPECT_EQ(view_->player().device_timing(), DeviceTiming::kOff);
  EXPECT_TRUE(view_->render_metrics().sections.empty()) << "the draw untimed";
  // The worker may have decoded the next frame already, timed.
  const std::uint64_t taken = view_->frames_taken();
  ASSERT_TRUE(tick_until([&] { return view_->frames_taken() >= taken + 2; }));
  EXPECT_FALSE(view_->shown()->timed);
  for (const core::StageRow& row : view_->shown()->stages.rows()) {
    EXPECT_EQ(row.device, core::DeviceTime::None) << row.name;
  }
  // Past a refresh, so the frame they open on rebuilds the figures.
  tick_for(StatsPanel::kRefresh);
  ASSERT_NO_FATAL_FAILURE(tap_stats_header());
  ASSERT_TRUE(view_->stats_panel().open());
  EXPECT_TRUE(view_->stats_panel().render_rows().empty());
  EXPECT_EQ(view_->stats_panel().render_window().frames, 0u);
  tick_for(2 * StatsPanel::kRefresh);
  EXPECT_TRUE(draw_row_shown());
  EXPECT_EQ(view_->player().device_timing(), DeviceTiming::kAll);
}

// A measurement's timing holds whatever the stats show; the draw is timed
// only while they are open and that timing is not off; and each frame taken
// counts once.
TEST_F(PlayerViewTest, AMeasurementsTimingHoldsWhateverTheStatsShow) {
  for (const DeviceTiming timing : {DeviceTiming::kOff, DeviceTiming::kAll}) {
    SCOPED_TRACE(volumetric_kit::player::to_string(timing));
    touch_.reset();
    view_.reset();
    device_timing_ = timing;
    ASSERT_TRUE(make_view());
    std::vector<std::uint64_t> shown;
    ASSERT_TRUE(tick_until([&] {
      if (view_->shown() &&
          (shown.empty() || shown.back() != view_->shown()->index)) {
        shown.push_back(view_->shown()->index);
      }
      return shown.size() >= 2;
    }));
    EXPECT_EQ(view_->frames_taken(), shown.size());
    EXPECT_TRUE(view_->stats_panel().open());
    EXPECT_EQ(view_->player().device_timing(), timing);
    EXPECT_EQ(view_->shown()->timed, timing != DeviceTiming::kOff);
    tick_for(2 * StatsPanel::kRefresh);
    EXPECT_EQ(draw_row_shown(), timing != DeviceTiming::kOff);
    // Closed, the worker's timing holds, and the draw is not timed: the
    // frame they reopen on has nothing of the renderer's.
    ASSERT_NO_FATAL_FAILURE(tap_stats_header());
    ASSERT_FALSE(view_->stats_panel().open());
    EXPECT_EQ(view_->player().device_timing(), timing);
    tick_for(StatsPanel::kRefresh);
    ASSERT_NO_FATAL_FAILURE(tap_stats_header());
    ASSERT_TRUE(view_->stats_panel().open());
    EXPECT_TRUE(view_->stats_panel().render_rows().empty());
  }
}

// The draw waits on the GPU for its mesh's ready value: each frame drawing a
// mesh goes to the queue behind that value at the draw's stages
// (frame_waits). The worker reached it before it published, so every such
// frame presents, and the layer, sync validation included where the test
// policy asks, reports nothing: on the device's families (MoltenVK's two,
// and its specialized ones in their rerun).
TEST_F(PlayerViewTest, TheDrawWaitsOnTheMeshesReadyValue) {
  ASSERT_TRUE(make_view());
  constexpr VkPipelineStageFlags kDrawStages =
      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
  int drawn = 0;
  ASSERT_TRUE(tick_until([&] {
    const grid::mesh::DeviceMesh& mesh = view_->mesh();
    if (mesh.valid() && !mesh.empty()) {
      EXPECT_NE(mesh.ready.semaphore, nullptr);
      // The frame went to the queue behind that value, at the draw's stages.
      const std::vector<gfx::windowing::FrameWait>& waits =
          view_->frame_waits();
      EXPECT_TRUE(std::any_of(waits.begin(), waits.end(),
                              [&](const gfx::windowing::FrameWait& wait) {
                                return wait.point.semaphore ==
                                           mesh.ready.semaphore &&
                                       wait.point.value == mesh.ready.value &&
                                       wait.stages == kDrawStages;
                              }))
          << "no wait on generation " << mesh.generation << "'s ready value";
      if (mesh.ready.semaphore != nullptr) {
        const core::Result<std::uint64_t> reached =
            mesh.ready.semaphore->value();
        EXPECT_TRUE(reached.ok()) << reached.status().message();
        if (reached.ok()) {
          EXPECT_GE(*reached, mesh.ready.value);
        }
      }
      ++drawn;
    }
    return drawn >= 30;
  })) << drawn
      << " frame(s) drew a mesh";
  EXPECT_TRUE(view_->player().status().ok())
      << view_->player().status().message();
}

// A frame drawn is stamped from its due time to its present call, in order,
// and the stats show its latency's legs.
TEST_F(PlayerViewTest, LatencyIsStampedFromDueToPresent) {
  ASSERT_TRUE(make_view());
  ASSERT_TRUE(tick_until([&] {
    return view_->shown().has_value() && view_->shown()->index >= 2;
  }));
  const FrameStamp stamp = *view_->shown();
  ASSERT_TRUE(stamp.taken.has_value());
  ASSERT_TRUE(stamp.presented.has_value());
  EXPECT_NE(stamp.due, SteadyClock::time_point{});
  EXPECT_LE(stamp.due, stamp.published);
  EXPECT_LE(stamp.published, *stamp.taken);
  EXPECT_LE(*stamp.taken, *stamp.presented);
  tick_for(2 * StatsPanel::kRefresh);
  bool latency = false;
  for (const std::string& line : view_->stats_panel().lines()) {
    if (line.rfind("latency ", 0) == 0) {
      latency = true;
      EXPECT_EQ(line.find(" - "), std::string::npos) << line;
    }
  }
  EXPECT_TRUE(latency) << "no latency line";
}

TEST_F(PlayerViewTest, TeardownMidPlaybackIsClean) {
  ASSERT_TRUE(make_view());
  ASSERT_TRUE(tick_until([&] { return view_->shown().has_value(); }));
  tick_for(kQuiet);  // the worker decodes ahead while frames are in flight
  const SteadyClock::time_point start = SteadyClock::now();
  touch_.reset();
  view_.reset();
  EXPECT_LT(SteadyClock::now() - start, kPrompt);
  // The layer loaded where it was asked for; an error it reported fails the
  // test.
  for (const std::string& warning : warnings()) {
    EXPECT_EQ(warning.find("continuing without it"), std::string::npos)
        << warning;
  }
}

// Live, on its two swapchain images: frames present, with no sender (port 1
// refuses) and so nothing to draw.
TEST_F(PlayerViewTest, ALiveViewPresents) {
  core::Result<std::unique_ptr<StreamPlayer>> live =
      StreamPlayer::connect({"127.0.0.1", 1});
  ASSERT_TRUE(live.ok()) << live.status().message();
  core::Result<std::unique_ptr<PlayerView>> view =
      PlayerView::create(surface_, extent_, std::move(live).value(), config());
  ASSERT_TRUE(view.ok()) << view.status().message();
  view_ = std::move(view).value();
  core::Result<std::unique_ptr<gfx::ui::ImGuiTouch>> touch =
      gfx::ui::ImGuiTouch::create(view_->ui_context());
  ASSERT_TRUE(touch.ok()) << touch.status().message();
  touch_ = std::move(touch).value();
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(present());
  }
  EXPECT_FALSE(view_->shown().has_value());
}

// On the host: FIFO, gfx's default, and D32 depth; live, two images, so a mesh
// taken shows a vsync sooner, and a file's, gfx's default.
TEST(ViewSwapchain, LiveAsksForTwoImages) {
  for (const bool live : {false, true}) {
    const gfx::windowing::SwapchainConfig config =
        view_swapchain(kExtent, live);
    EXPECT_EQ(config.extent.width, kExtent.width);
    EXPECT_EQ(config.extent.height, kExtent.height);
    EXPECT_EQ(config.preferred_present_mode, VK_PRESENT_MODE_FIFO_KHR);
    EXPECT_EQ(config.depth_format, VK_FORMAT_D32_SFLOAT);
    EXPECT_EQ(config.min_image_count, live ? 2u : 0u) << "live " << live;
  }
}

#ifdef __APPLE__
// The device create() makes, on each of MoltenVK's family layouts
// (CMakeLists.txt): two families, so the tests above cross them.
TEST_F(PlayerViewTest, MoltenVkPlansTwoFamilies) {
  gfx::app::WindowDeviceConfig device_config;
  device_config.instance.enable_validation = config().validation;
  device_config.graphics = gfx::app::WindowedAppConfig{}.device;
  core::Result<std::unique_ptr<core::SharedDevice>> shared =
      gfx::app::make_shared_device(surface_, grid::device_requirements(),
                                   device_config);
  ASSERT_TRUE(shared.ok()) << shared.status().message();
  EXPECT_EQ(shared.value()->plan(), core::QueuePlan::TwoFamilies)
      << core::to_string(shared.value()->plan());
}
#endif

// --- A program's device requirements (PlayerViewConfig::device_requirements)

// With none, the device is grid's alone: an extension the GPU offers that
// nothing the view asks for enables is off, and grid's are on.
TEST_F(PlayerViewTest, WithNoProgramRequirementsTheDeviceIsGridsAlone) {
  const std::string offered = vp_test::offered_extension(physical());
  ASSERT_FALSE(offered.empty()) << "the GPU offers none of the extensions";
  ASSERT_TRUE(make_view());
  EXPECT_FALSE(view_->compute_device().extension_enabled(offered.c_str()))
      << offered;
  const core::Status grids =
      view_->compute_device().check_enabled(grid::device_requirements());
  EXPECT_TRUE(grids.ok()) << grids.message();
}

// A program's optional extension is enabled where the GPU offers it, on both
// the hooks' devices (one VkDevice), and skipped where it does not.
TEST_F(PlayerViewTest, AProgramsOptionalExtensionIsEnabledWhereOffered) {
  const std::string offered = vp_test::offered_extension(physical());
  ASSERT_FALSE(offered.empty()) << "the GPU offers none of the extensions";
  device_requirements_.optional_extensions = {offered,
                                              vp_test::kAbsentExtension};
  make_hooks_ = [](const HookContext&) {
    return core::Result<FrameHooks>(FrameHooks{});
  };
  ASSERT_TRUE(make_view());
  ASSERT_TRUE(context_.has_value());
  for (const core::Device* device :
       {context_->device, context_->commit_device}) {
    EXPECT_TRUE(device->extension_enabled(offered.c_str())) << offered;
    EXPECT_FALSE(device->extension_enabled(vp_test::kAbsentExtension));
  }
}

// A program's requirements that leave grid's out keep them: the worker's
// device enables them, and frames play.
TEST_F(PlayerViewTest, GridsRequirementsHoldUnderAProgramsOwn) {
  const std::string offered = vp_test::offered_extension(physical());
  ASSERT_FALSE(offered.empty()) << "the GPU offers none of the extensions";
  device_requirements_.timeline_semaphore = false;
  device_requirements_.scalar_block_layout = false;
  device_requirements_.optional_extensions = {offered};
  ASSERT_TRUE(make_view());
  const core::Status grids =
      view_->compute_device().check_enabled(grid::device_requirements());
  EXPECT_TRUE(grids.ok()) << grids.message();
  EXPECT_TRUE(view_->compute_device().extension_enabled(offered.c_str()))
      << offered;
  ASSERT_TRUE(tick_until([&] {
    return view_->shown().has_value() && view_->shown()->index >= 2;
  }));
  EXPECT_TRUE(view_->player().status().ok())
      << view_->player().status().message();
}

// A program's required extension no GPU offers refuses the view, naming it,
// before its hooks are made.
TEST_F(PlayerViewTest, AProgramsRequiredExtensionTheGpuLacksRefusesTheView) {
  device_requirements_.extensions = {vp_test::kAbsentExtension};
  int makes = 0;
  make_hooks_ = [&makes](const HookContext&) {
    ++makes;
    return core::Result<FrameHooks>(FrameHooks{});
  };
  const core::Status refused =
      PlayerView::create(surface_, kExtent, open(), config()).status();
  EXPECT_EQ(refused.domain(), core::Status::Code::Unsupported)
      << refused.message();
  EXPECT_NE(refused.message().find(vp_test::kAbsentExtension),
            std::string::npos)
      << refused.message();
  EXPECT_EQ(makes, 0);
}

// --- Hooks (frame_hooks.hpp) through the view --------------------------------

// The byte a target stores for linear light `linear`, sRGB-encoded: an _SRGB
// target encodes it, and the pipeline encodes for a UNORM one.
int encoded(double linear) {
  linear = std::clamp(linear, 0.0, 1.0);
  const double srgb = linear <= 0.0031308
                          ? 12.92 * linear
                          : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
  return static_cast<int>(std::lround(255.0 * srgb));
}

// A view of a mesh's vertex buffer, `count` vertices: its deleter frees
// nothing.
core::Buffer vertex_view(const grid::mesh::DeviceMesh& mesh,
                         std::uint32_t count) {
  return core::Buffer(
      mesh.vertices, VkDeviceSize{count} * sizeof(grid::mesh::Vertex),
      mesh.vertex_usage, mesh.sharing_mode, nullptr, [] {}, std::nullopt);
}

// The hooks' context names the worker's device, the view's own and its
// pipeline; after_mesh's extra reaches the commit and before_draw with its
// frame.
TEST_F(PlayerViewTest, AnExtraReachesTheCommitAndBeforeDraw) {
  struct Seen {
    std::atomic<int> commits{0};
    std::atomic<int> draws{0};
    std::atomic<int> wrong{0};
  };
  const auto seen = std::make_shared<Seen>();
  const auto check = [seen](const FrameStamp& stamp) {
    const auto* index = static_cast<const std::uint64_t*>(stamp.extra.get());
    if (index == nullptr || *index != stamp.index) {
      ++seen->wrong;
    }
  };
  make_hooks_ = [seen, check](const HookContext&) {
    FrameHooks hooks;
    hooks.after_mesh = [](core::CommandBatch&,
                          const grid::volume::VoxelBlockGrid&,
                          const grid::mesh::DeviceMesh&, FrameStamp& stamp) {
      stamp.extra = std::make_shared<const std::uint64_t>(stamp.index);
      return core::Status();
    };
    hooks.commit = [seen, check](const grid::mesh::DeviceMesh&,
                                 FrameStamp& stamp) {
      check(stamp);
      ++seen->commits;
      return core::Result<FrameCommit>(FrameCommit::kDraw);
    };
    hooks.before_draw = [seen, check](VkCommandBuffer cmd,
                                      const FrameStamp& stamp) {
      check(stamp);
      if (cmd == VK_NULL_HANDLE) {
        ++seen->wrong;
      }
      ++seen->draws;
      return core::Result<DrawBinding>(DrawBinding{});
    };
    return core::Result<FrameHooks>(std::move(hooks));
  };
  ASSERT_TRUE(make_view());
  ASSERT_TRUE(context_.has_value());
  EXPECT_EQ(context_->device, &view_->compute_device());
  EXPECT_NE(context_->commit_device, context_->device);
  EXPECT_NE(context_->pipeline, nullptr);
  ASSERT_TRUE(tick_until([&] {
    return view_->shown().has_value() && view_->shown()->index >= 2;
  }));
  EXPECT_GE(seen->commits, 3);
  EXPECT_GE(seen->draws, seen->commits);
  EXPECT_EQ(seen->wrong, 0);
  EXPECT_TRUE(view_->player().status().ok())
      << view_->player().status().message();
}

// A frame's extra is released only once every display frame that drew it
// has run on the GPU. before_draw marks each display frame in a buffer the
// host reads, from that frame's own commands; at an extra's release, every
// display frame that drew its frame has marked it, and the view has begun at
// least its frames in flight since the last of them, each once the slot's
// last frame completed. At teardown, after the view's idle wait, the marks
// alone are checked.
TEST_F(PlayerViewTest, AnExtraIsReleasedOnlyOnceEveryDrawOfItsFrameHasRun) {
  constexpr std::uint32_t kMarks = 4096;
  constexpr std::uint64_t kFramesInFlight = 2;  // the view's
  struct Results {
    std::atomic<int> released{0};
    std::atomic<int> early{0};  // fewer frames begun since its last draw
    std::atomic<int> unrun{0};  // a draw of its frame not marked
    std::atomic<bool> teardown{false};
  };
  // The buffer and the draws; the hooks' and the extras' alone, so it goes
  // with the view, before its device.
  struct Marks {
    core::Buffer buffer;  // kMarks words, host-visible, on the view's device
    std::mutex mutex;
    std::uint64_t display = 0;  // display frames drawn so far
    std::map<std::uint64_t, std::vector<std::uint64_t>> drew;  // by frame
    std::uint64_t made = 0;  // extras made, a key each
    std::shared_ptr<Results> results;
  };
  const auto results = std::make_shared<Results>();
  make_hooks_ =
      [results](const HookContext& context) -> core::Result<FrameHooks> {
    const auto marks = std::make_shared<Marks>();
    marks->results = results;
    core::BufferDesc desc;
    desc.size = kMarks * sizeof(std::uint32_t);
    desc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    desc.memory = core::MemoryUsage::Staging;
    desc.host_access = core::HostAccess::Random;
    VKC_ASSIGN(marks->buffer, context.commit_allocator.create_buffer(desc));
    std::memset(marks->buffer.mapped(), 0, desc.size);
    FrameHooks hooks;
    // Each frame's extra, keyed apart from its index, which a pass repeats.
    hooks.after_mesh =
        [marks](core::CommandBatch&, const grid::volume::VoxelBlockGrid&,
                const grid::mesh::DeviceMesh&, FrameStamp& stamp) {
          std::uint64_t key = 0;
          {
            std::lock_guard<std::mutex> lock(marks->mutex);
            key = ++marks->made;
          }
          stamp.extra = std::shared_ptr<const void>(
              new std::uint64_t(key), [marks](const void* extra) {
                const auto* frame = static_cast<const std::uint64_t*>(extra);
                Results& results = *marks->results;
                std::lock_guard<std::mutex> lock(marks->mutex);
                const auto* words =
                    static_cast<const std::uint32_t*>(marks->buffer.mapped());
                std::uint64_t last = 0;
                for (const std::uint64_t display : marks->drew[*frame]) {
                  if (words[display % kMarks] != display + 1) {
                    ++results.unrun;
                  }
                  last = std::max(last, display + 1);
                }
                // The display frame under way, whose begin waited for its slot.
                if (!results.teardown && last != 0 &&
                    marks->display + 1 < last + kFramesInFlight) {
                  ++results.early;
                }
                ++results.released;
                delete frame;
              });
          return core::Status();
        };
    hooks.before_draw = [marks](VkCommandBuffer cmd, const FrameStamp& stamp) {
      std::lock_guard<std::mutex> lock(marks->mutex);
      const std::uint64_t display = marks->display++;
      marks->drew[*static_cast<const std::uint64_t*>(stamp.extra.get())]
          .push_back(display);
      vkCmdFillBuffer(cmd, marks->buffer.handle(),
                      (display % kMarks) * sizeof(std::uint32_t),
                      sizeof(std::uint32_t),
                      static_cast<std::uint32_t>(display + 1));
      VkMemoryBarrier read{};
      read.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      read.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &read, 0, nullptr,
                           0, nullptr);
      return core::Result<DrawBinding>(DrawBinding{});
    };
    return hooks;
  };
  ASSERT_TRUE(make_view());
  ASSERT_TRUE(tick_until([&] {
    return view_->shown().has_value() && view_->shown()->index >= 4;
  }));
  EXPECT_GE(results->released, 3) << "the frames before are released";
  results->teardown = true;
  touch_.reset();
  view_.reset();
  EXPECT_GE(results->released, 5);
  EXPECT_EQ(results->early, 0);
  EXPECT_EQ(results->unrun, 0);
}

// Every pixel of the tests' bound image, as sRGB bytes.
constexpr std::array<std::uint8_t, 4> kBoundValue{200, 120, 40, 255};

// Hooks that bind a 2x2 image of kBoundValue (gfx's HybridMeshTextureSet
// against the view's pipeline) with kHybridMeshLit cleared, while `bind`
// holds, else a binding of no set; after_mesh sets each mesh's vertices to
// sample the image's middle, and `drawn` counts before_draw's calls.
MakeHooks bound_image_hooks(std::shared_ptr<std::atomic<bool>> bind,
                            std::shared_ptr<std::atomic<int>> drawn) {
  struct Bound {
    core::Image image;
    std::unique_ptr<gfx::pipelines::HybridMeshTextureSet> set;
  };
  return [bind, drawn](const HookContext& context) -> core::Result<FrameHooks> {
    std::array<std::uint8_t, 16> pixels{};
    for (std::size_t i = 0; i < pixels.size(); ++i) {
      pixels[i] = kBoundValue[i % 4];
    }
    gfx::ImageUploadDesc desc;
    desc.extent = {2, 2};
    desc.format = VK_FORMAT_R8G8B8A8_SRGB;
    desc.pixels = pixels.data();
    desc.size = pixels.size();
    const auto bound = std::make_shared<Bound>();
    VKC_ASSIGN(bound->image,
               gfx::upload_texture(context.commit_device,
                                   context.commit_allocator, desc));
    VKC_ASSIGN(bound->set, gfx::pipelines::HybridMeshTextureSet::create(
                               context.commit_device, context.commit_allocator,
                               *context.pipeline, bound->image.view()));
    FrameHooks hooks;
    core::Device* const device = &context.device;
    core::Allocator* const allocator = &context.allocator;
    hooks.after_mesh = [device, allocator](core::CommandBatch& batch,
                                           const grid::volume::VoxelBlockGrid&,
                                           const grid::mesh::DeviceMesh& mesh,
                                           FrameStamp&) -> core::Status {
      if (mesh.vertex_count == 0) {
        return {};
      }
      // Its vertices read back with a host wait of the test's, each set to
      // sample the image's middle, and written back in after_mesh's batch.
      const core::Buffer vertices = vertex_view(mesh, mesh.vertex_count);
      std::vector<grid::mesh::Vertex> read(mesh.vertex_count);
      core::CommandBatch readback(*device, *allocator);
      VKC_TRY(readback.readback(vertices, 0, vertices.size(), read.data()));
      VKC_TRY(readback.submit());
      for (grid::mesh::Vertex& vertex : read) {
        vertex.uv0.x = 0.5f;
        vertex.uv0.y = 0.5f;
      }
      return batch.upload(vertices, 0, read.data(), vertices.size());
    };
    hooks.before_draw = [bound, bind, drawn](VkCommandBuffer,
                                             const FrameStamp&) {
      ++*drawn;
      if (!*bind) {
        return core::Result<DrawBinding>(DrawBinding{});
      }
      return core::Result<DrawBinding>(
          DrawBinding{bound->set->set(), gfx::pipelines::kHybridMeshLit});
    };
    return hooks;
  };
}

// before_draw's binding reaches the draw: with a 2x2 image's set bound
// (bound_image_hooks) and kHybridMeshLit cleared, Texture on at start
// (PlayerViewConfig::textured), the mesh, whose vertices after_mesh sets to
// sample the image's middle, draws the image's value within 1 code.
TEST_F(PlayerViewTest, ABoundImagesSetIsDrawnWithTheClearedFlags) {
  make_hooks_ = bound_image_hooks(std::make_shared<std::atomic<bool>>(true),
                                  std::make_shared<std::atomic<int>>(0));
  textured_ = true;
  ASSERT_TRUE(make_view(/*playing=*/false));
  EXPECT_TRUE(view_->textured());
  view_->player().step();
  ASSERT_TRUE(tick_until([&] { return view_->shown().has_value(); }));
  aim_at(view_->shown()->index);
  ASSERT_TRUE(present());
  expect_mesh_pixel(std::vector<int>(kBoundValue.begin(), kBoundValue.end()),
                    "the image");
  EXPECT_TRUE(view_->player().status().ok())
      << view_->player().status().message();
}

// Texture: off at first, the frame whose before_draw binds the image's set
// draws as a binding of no set does, lit, the same pixel within 1 code, and
// before_draw still runs each frame; set_textured(true) draws the image's
// value at the next frame, and off again as with no set. The panel has no
// Texture box until a set is bound, and has it then, Texture off; a tap on
// it flips Texture, as set_textured does, and the box stays once none is
// bound. Unlit, Texture off, a frame that binds a set draws as one that
// binds none.
TEST_F(PlayerViewTest, TextureOffDrawsAsNoBindingAndOnDrawsTheTexture) {
  const auto bind = std::make_shared<std::atomic<bool>>(false);
  const auto drawn = std::make_shared<std::atomic<int>>(0);
  make_hooks_ = bound_image_hooks(bind, drawn);
  ASSERT_TRUE(make_view(/*playing=*/false));
  ASSERT_FALSE(view_->textured()) << "off at first";
  view_->player().step();
  ASSERT_TRUE(tick_until([&] { return view_->shown().has_value(); }));
  aim_at(view_->shown()->index);
  ASSERT_TRUE(present());
  // The pixel a binding of no set draws: lit, and not the image's value, or
  // the comparisons below would show nothing.
  const std::vector<int> none = mesh_pixel();
  const std::vector<int> image(kBoundValue.begin(), kBoundValue.end());
  ASSERT_TRUE(differs(none, image)) << "the mesh's own pixel is the image's";
  ImGui::SetCurrentContext(view_->ui_context());
  ImGuiWindow* const controls = ImGui::FindWindowByName(kControlsWindow);
  ASSERT_NE(controls, nullptr);
  const ImGuiID texture_id = controls->GetID("Texture");
  EXPECT_NE(item_under(vp_test::control_middle(*controls, vp_test::kTexture)),
            texture_id)
      << "a Texture box with no set bound";

  *bind = true;
  const int before = *drawn;
  ASSERT_TRUE(present());
  ASSERT_TRUE(present());
  EXPECT_GE(*drawn - before, 2) << "before_draw ran each frame, Texture off";
  expect_mesh_pixel(none, "off");
  // The box, now that a set is bound, after Unlit, with Texture never on.
  EXPECT_EQ(item_under(vp_test::control_middle(*controls, vp_test::kTexture)),
            texture_id)
      << "no box while Texture is off";
  view_->set_textured(true);
  ASSERT_TRUE(present());
  expect_mesh_pixel(image, "on");
  view_->set_textured(false);
  ASSERT_TRUE(present());
  expect_mesh_pixel(none, "off again");

  tap(vp_test::control_middle(*controls, vp_test::kTexture));
  EXPECT_TRUE(view_->textured());
  ASSERT_TRUE(present());
  expect_mesh_pixel(image, "on from the box");
  tap(vp_test::control_middle(*controls, vp_test::kTexture));
  EXPECT_FALSE(view_->textured());
  ASSERT_TRUE(present());
  expect_mesh_pixel(none, "off from the box");
  EXPECT_FALSE(view_->unlit()) << "the tap pressed Texture alone";
  EXPECT_FALSE(view_->player().playing()) << "the tap pressed Texture alone";

  // The box stays with no set bound since: a frame binds none, and the next,
  // whose controls follow it, still has the box.
  *bind = false;
  ASSERT_TRUE(present());
  EXPECT_EQ(item_under(vp_test::control_middle(*controls, vp_test::kTexture)),
            texture_id)
      << "the box went with the set";

  // Unlit applies with Texture off and a set bound: the frame draws the pixel
  // Unlit draws with no set, which is not the lit one.
  view_->set_unlit(true);
  ASSERT_TRUE(present());
  const std::vector<int> unlit = mesh_pixel();
  ASSERT_TRUE(differs(none, unlit)) << "lit and unlit draw the same pixel";
  *bind = true;
  ASSERT_TRUE(present());
  expect_mesh_pixel(unlit, "off, a set bound, Unlit");
  EXPECT_TRUE(view_->player().status().ok())
      << view_->player().status().message();
}

// A binding that names no set applies with Texture off as on: a before_draw
// that clears kHybridMeshLit with no set draws the mesh as set_unlit(true)
// does, the same pixel within 1 code, Texture off and then on; that pixel is
// not the lit one, or the comparisons would show nothing.
TEST_F(PlayerViewTest, ABindingOfNoSetAppliesWithTextureOffOrOn) {
  const auto clear = std::make_shared<std::atomic<bool>>(false);
  make_hooks_ = [clear](const HookContext&) -> core::Result<FrameHooks> {
    FrameHooks hooks;
    hooks.before_draw = [clear](VkCommandBuffer, const FrameStamp&) {
      DrawBinding binding;
      if (*clear) {
        binding.mesh_flags_clear = gfx::pipelines::kHybridMeshLit;
      }
      return core::Result<DrawBinding>(binding);
    };
    return hooks;
  };
  ASSERT_TRUE(make_view(/*playing=*/false));
  ASSERT_FALSE(view_->textured()) << "off at first";
  view_->player().step();
  ASSERT_TRUE(tick_until([&] { return view_->shown().has_value(); }));
  aim_at(view_->shown()->index);
  ASSERT_TRUE(present());
  const std::vector<int> lit = mesh_pixel();
  view_->set_unlit(true);
  ASSERT_TRUE(present());
  const std::vector<int> unlit = mesh_pixel();
  view_->set_unlit(false);
  ASSERT_TRUE(differs(lit, unlit)) << "lit and unlit draw the same pixel";

  *clear = true;
  ASSERT_TRUE(present());
  expect_mesh_pixel(unlit, "Texture off");
  view_->set_textured(true);
  ASSERT_TRUE(present());
  expect_mesh_pixel(unlit, "Texture on");
  EXPECT_TRUE(view_->player().status().ok())
      << view_->player().status().message();
}

// Unlit: the view's own binding clears kHybridMeshLit, so a pixel of the
// mesh is its vertex colour within 1 code; lit, the same pixel is shaded
// darker, as the level camera faces no surface toward the light, which comes
// from above. The panel's box flips it, as set_unlit does.
TEST_F(PlayerViewTest, UnlitShowsTheVertexColourUnshaded) {
  make_hooks_ = [](const HookContext&) {
    return core::Result<FrameHooks>(FrameHooks{});
  };
  unlit_ = true;
  ASSERT_TRUE(make_view(/*playing=*/false));
  EXPECT_TRUE(view_->unlit());
  view_->player().step();
  ASSERT_TRUE(tick_until([&] { return view_->shown().has_value(); }));
  aim_at(view_->shown()->index);
  ASSERT_TRUE(present());
  // The mesh's vertex colour, one for every vertex.
  const grid::mesh::DeviceMesh& mesh = view_->mesh();
  ASSERT_GT(mesh.vertex_count, 0u);
  std::vector<grid::mesh::Vertex> read(mesh.vertex_count);
  core::CommandBatch readback(*context_->device, *context_->allocator);
  const core::Buffer vertices = vertex_view(mesh, mesh.vertex_count);
  ASSERT_TRUE(
      readback.readback(vertices, 0, vertices.size(), read.data()).ok());
  ASSERT_TRUE(readback.submit().ok());
  const grid::mesh::Vertex& first = read.front();
  for (const grid::mesh::Vertex& vertex : read) {
    ASSERT_EQ(vertex.color.x, first.color.x);
    ASSERT_EQ(vertex.color.y, first.color.y);
    ASSERT_EQ(vertex.color.z, first.color.z);
  }
  const std::vector<int> colour{encoded(first.color.x), encoded(first.color.y),
                                encoded(first.color.z)};
  expect_mesh_pixel(colour, "Unlit");
  ImGui::SetCurrentContext(view_->ui_context());
  const ImGuiWindow* const panel = ImGui::FindWindowByName(kControlsWindow);
  ASSERT_NE(panel, nullptr);
  tap(vp_test::control_middle(*panel, vp_test::kUnlit));
  EXPECT_FALSE(view_->unlit());
  ASSERT_TRUE(present());
  const std::vector<int> lit = mesh_pixel();
  ASSERT_EQ(lit.size(), 4u);
  for (std::size_t c = 0; c < 3; ++c) {
    EXPECT_LT(lit[c], colour[c] - 10) << "component " << c << " unshaded";
  }
  view_->set_unlit(true);
  ASSERT_TRUE(present());
  expect_mesh_pixel(colour, "Unlit again");
  EXPECT_FALSE(view_->player().playing()) << "the tap pressed Unlit alone";
}

// A commit hook that skips frame 2 and holds frame 3 for five asks, through
// the view: frame 2 is never shown, frame 3 is after its holds, and the
// view counts the skip and the hold.
TEST_F(PlayerViewTest, ACommitHookHoldsAndSkipsFrames) {
  const auto asked = std::make_shared<std::atomic<int>>(0);  // for frame 3
  make_hooks_ = [asked](const HookContext&) {
    FrameHooks hooks;
    hooks.commit = [asked](const grid::mesh::DeviceMesh&, FrameStamp& stamp) {
      if (stamp.index == 2) {
        return core::Result<FrameCommit>(FrameCommit::kSkip);
      }
      if (stamp.index == 3 && ++*asked <= 5) {
        return core::Result<FrameCommit>(FrameCommit::kHold);
      }
      return core::Result<FrameCommit>(FrameCommit::kDraw);
    };
    return core::Result<FrameHooks>(std::move(hooks));
  };
  ASSERT_TRUE(make_view());
  std::vector<std::uint64_t> shown;
  ASSERT_TRUE(tick_until([&] {
    if (view_->shown() &&
        (shown.empty() || shown.back() != view_->shown()->index)) {
      shown.push_back(view_->shown()->index);
    }
    return shown.size() >= 4;
  }));
  EXPECT_EQ(shown, (std::vector<std::uint64_t>{0, 1, 3, 4}));
  EXPECT_EQ(view_->frames_skipped(), 1u);
  EXPECT_GE(view_->frames_held(), 1u);
  EXPECT_EQ(*asked, 6);
  EXPECT_TRUE(view_->player().status().ok())
      << view_->player().status().message();
}

// The hooks' stats line, each read numbered, is the stats' last: the view
// hands it to its panel.
TEST_F(PlayerViewTest, TheHooksStatsLineIsTheStatsLast) {
  const auto asked = std::make_shared<int>(0);
  make_hooks_ = [asked](const HookContext&) {
    FrameHooks hooks;
    hooks.stats_line = [asked] { return "read " + std::to_string(++*asked); };
    return core::Result<FrameHooks>(std::move(hooks));
  };
  ASSERT_TRUE(make_view());
  ASSERT_TRUE(present());
  ASSERT_GE(*asked, 1);
  EXPECT_EQ(view_->stats_panel().lines().back(),
            "read " + std::to_string(*asked));
}

// A before_draw failure stops the view with its status: the tick that drew
// the frame returns it, and every tick after refuses with it, the hook not
// asked again.
TEST_F(PlayerViewTest, ABeforeDrawFailureStopsTheView) {
  const auto asked = std::make_shared<std::atomic<int>>(0);
  make_hooks_ = [asked](const HookContext&) {
    FrameHooks hooks;
    hooks.before_draw = [asked](
                            VkCommandBuffer,
                            const FrameStamp&) -> core::Result<DrawBinding> {
      ++*asked;
      return core::Status::unsupported("the test's before_draw refuses");
    };
    return core::Result<FrameHooks>(std::move(hooks));
  };
  ASSERT_TRUE(make_view());
  core::Status failed;
  const SteadyClock::time_point deadline = SteadyClock::now() + kArrival;
  while (failed.ok() && SteadyClock::now() < deadline) {
    failed = tick().status();
    std::this_thread::sleep_for(1ms);
  }
  ASSERT_FALSE(failed.ok()) << "frame() kept presenting";
  EXPECT_NE(failed.message().find("before_draw refuses"), std::string::npos)
      << failed.message();
  EXPECT_EQ(*asked, 1);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(tick().status().message(), failed.message());
  }
  EXPECT_EQ(*asked, 1);
  EXPECT_TRUE(view_->player().status().ok())
      << view_->player().status().message();
}

// Target the small per-frame wait-vector allocation, in a begun frame: once
// asked, before_draw, which the view calls just before it adds the mesh's
// wait, arms the failure of the next allocation of a FrameWait's size on this
// UI thread, never a worker's. Armed there rather than at a measured call
// site, it never fails the validation layer's or the driver's allocations on
// this thread, which share sizes and call sites, and which the core may
// contain in a call that fails cleanly.
TEST_F(PlayerViewTest, AllocationFailureStopsFurtherFrameAcquisition) {
  const auto arm = std::make_shared<bool>(false);
  make_hooks_ = [arm](const HookContext&) -> core::Result<FrameHooks> {
    FrameHooks hooks;
    hooks.before_draw = [arm](VkCommandBuffer, const FrameStamp&) {
      if (*arm) {
        *arm = false;
        auto& failure = vp_test::allocation_failure;
        failure.bytes = sizeof(gfx::windowing::FrameWait);
        failure.armed = true;
      }
      return core::Result<DrawBinding>(DrawBinding{});
    };
    return hooks;
  };
  ASSERT_TRUE(make_view());
  ASSERT_TRUE(tick_until([this] { return !view_->frame_waits().empty(); }));
  view_->player().pause();
  auto& failure = vp_test::allocation_failure;
  failure = {};
  *arm = true;
  core::Result<bool> result = false;
  bool escaped = false;
  try {
    result = tick();
  } catch (const std::bad_alloc&) {
    escaped = true;
  }
  failure.armed = false;
  const bool injected = failure.injected;
  failure = {};
  ASSERT_TRUE(injected);
  ASSERT_FALSE(escaped);
  EXPECT_EQ(result.status().domain(), core::Status::Code::OutOfMemory);
  // There may be a partially recorded frame. It must never be acquired again
  // or submitted; destroying this view is the recovery path.
  const auto again = view_->frame(kExtent);
  EXPECT_FALSE(again.ok());
  EXPECT_EQ(again.status().domain(), core::Status::Code::InvalidArgument);
  touch_.reset();
  view_.reset();
  // The caller can recover by opening a new view, on an independent device.
  ASSERT_TRUE(make_view(false));
  EXPECT_TRUE(tick().ok());
}

}  // namespace
