// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// PlayerView on a device, over gfx's headless surface and the tests' stream
// (test_stream.hpp), with gfx's ImGuiTouch as the platform half: create
// refuses what it cannot use; frames present and shown() advances, their
// triangles sharing vertices, onto a frame that meshes nothing too, which
// still presents; 0 x 0 skips without blocking; a step moves one frame; the
// worker's failure surfaces from frame(); the panel draws at a phone's
// scale, and its controls fit a phone and a fingertip at the iOS shell's
// sizes, as do the stats, with the profiler's draw row in their table, whose
// device half times the whole pass; ui_wants_pointer() follows a finger on
// the panel and on the stats' header, not on their body; and teardown
// mid-playback, the profiler attached, is clean. A live view presents, on
// the two images its swapchain asks for (checked on the host). The view
// makes its own shared device, validated as the test policy asks, so the
// layer's errors fail the test. The tests skip where the
// loader offers no headless surface (NVIDIA's ICD). On Apple a skip fails
// (CMakeLists.txt), and MoltenVK plans two queue families, so the mesh's
// buffers are CONCURRENT and read across. One family, EXCLUSIVE and read in
// place, is tested on the host only (mesh_wiring_test.cpp) until a leg runs
// the view on one.

#include "volumetric_kit/player/player_view.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <glm/vec2.hpp>

#include "controls_layout.hpp"
#include "imgui.h"
#include "imgui_internal.h"  // FindWindowByName, to find the panel
#include "test_stream.hpp"
#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/core/vulkan/shared_device.hpp"
#include "volumetric_kit/gfx/app/shared_device_config.hpp"
#include "volumetric_kit/gfx/app/windowed_app.hpp"
#include "volumetric_kit/gfx/ui/imgui_touch.hpp"
#include "volumetric_kit/gfx/windowing/surface_source.hpp"
#include "volumetric_kit/player/controls.hpp"
#include "volumetric_kit/recon/core/device_requirements.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace gfx = volumetric_kit::gfx;
namespace recon = volumetric_kit::recon;
using namespace std::chrono_literals;
using volumetric_kit::player::FrameStamp;
using volumetric_kit::player::kControlsWindow;
using volumetric_kit::player::kStatsWindow;
using volumetric_kit::player::PlayerView;
using volumetric_kit::player::PlayerViewConfig;
using volumetric_kit::player::StatsPanel;
using volumetric_kit::player::status_line;
using volumetric_kit::player::StreamPlayer;
using volumetric_kit::player::view_swapchain;
using SteadyClock = std::chrono::steady_clock;

constexpr VkExtent2D kExtent{320, 240};
// The narrowest phone in portrait: 375 x 667 points, at 3 pixels a point.
constexpr float kPhonePixelsPerPoint = 3.0f;
constexpr VkExtent2D kPhone{1125, 2001};
// The iOS shell's sizes (PlayerViewController.mm).
constexpr float kPhoneScale = 1.3f;
constexpr float kFingertip = 44.0f;
// How long a frame the test let go may take to arrive (the worker builds its
// kernels first); how long one it did not is waited for; and the longest a
// call that must not block takes.
constexpr auto kArrival = 10s;
constexpr auto kQuiet = 300ms;
constexpr auto kPrompt = 1s;

class PlayerViewTest : public core::test::VulkanDeviceTest {
 protected:
  // The fixture's device writes the stream; the view makes its own.
  core::DeviceRequirements requirements() const override {
    return recon::device_requirements();
  }

  void SetUp() override {
    VulkanDeviceTest::SetUp();
    if (base_setup_incomplete()) {
      return;
    }
    core::Result<gfx::windowing::SurfaceSource> headless =
        gfx::windowing::headless_surface();
    if (!headless.ok()) {
      GTEST_SKIP() << headless.status().message();
    }
    surface_ = std::move(headless).value();
    const core::Status written =
        vp_test::write_stream(device(), allocator(), path_);
    ASSERT_TRUE(written.ok()) << written.message();
  }

  // The view goes while the fixture still counts the layer's errors.
  void TearDown() override {
    touch_.reset();
    view_.reset();
    VulkanDeviceTest::TearDown();
  }

  // The view's config: at `ui_scale_` and `ui_min_frame_height_`, validated
  // as the test policy asks.
  PlayerViewConfig config() const {
    PlayerViewConfig config;
    config.ui_scale = ui_scale_;
    config.ui_min_frame_height = ui_min_frame_height_;
    config.validation = active_validation() != core::test::Validation::Off;
    return config;
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
    core::Result<std::unique_ptr<PlayerView>> view = PlayerView::create(
        surface_, extent_, std::move(opened).value(), config());
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

  // One tick, as a shell makes it: the touch half's frame, then the view's.
  core::Result<bool> tick() {
    seconds_ += 1.0 / 60.0;
    gfx::ui::DisplayFrame display;
    display.pixels = extent_;
    display.pixels_per_point = pixels_per_point_;
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

  // The rows of the stats' table after its header, as the last frame drew
  // it; -1 with no table.
  static int stats_rows() {
    ImGuiWindow* const body = stats_body();
    const ImGuiTable* const table =
        body != nullptr ? ImGui::TableFindByID(body->GetID("stages")) : nullptr;
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

  // The width of `text` in `panel`'s font as of the last frame, in points,
  // rounded up as ImGui rounds a line's. The view's context is current.
  static float text_width(const ImGuiWindow& panel, const std::string& text) {
    ImFont* const font = ImGui::GetIO().Fonts->Fonts[0];
    return std::ceil(
        font->CalcTextSizeA(panel.FontRefSize, FLT_MAX, 0.0f, text.c_str()).x);
  }

  vp_test::Scratch scratch_;
  const std::string path_ = scratch_.file("sphere.vrst");
  gfx::windowing::SurfaceSource surface_;
  std::unique_ptr<PlayerView> view_;
  std::unique_ptr<gfx::ui::ImGuiTouch> touch_;  // destroyed before the view
  double seconds_ = 0.0;
  // Set before make_view.
  VkExtent2D extent_ = kExtent;
  float pixels_per_point_ = 1.0f;
  float ui_scale_ = 1.0f;
  float ui_min_frame_height_ = 0.0f;
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
  // Its triangles share their vertices (mesh_wiring): fewer than 3 each.
  const recon::mesh::DeviceMesh& mesh = view_->mesh();
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

TEST_F(PlayerViewTest, ShownMovesOntoAFrameThatMeshesNothing) {
  const std::string path = scratch_.file("empty_frame.vrst");
  const core::Status written =
      vp_test::write_stream(device(), allocator(), path, /*empty=*/1);
  ASSERT_TRUE(written.ok()) << written.message();
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

// The top-left corner is the stats' header; their body is the scene's.
TEST_F(PlayerViewTest, UiWantsPointerTakesTheStatsHeaderNotTheirBody) {
  ASSERT_TRUE(make_view(/*playing=*/false));
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(present());
  }
  EXPECT_TRUE(wants_pointer_at({4.0f, 4.0f}));
  ImGui::SetCurrentContext(view_->ui_context());
  const ImGuiWindow* const body = stats_body();
  ASSERT_NE(body, nullptr);
  EXPECT_FALSE(wants_pointer_at(
      {body->Pos.x + 0.5f * body->Size.x, body->Pos.y + 0.5f * body->Size.y}));
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

// On the narrowest phone, at the iOS shell's sizes: a fingertip's square at
// each control's top left lands on it, "Pause" fits its button, and the panel
// fits across, both its row and the widest status line, a 5-digit track's
// playing in a loop.
TEST_F(PlayerViewTest, TheControlsFitAPhoneAndAFingertip) {
  extent_ = kPhone;
  pixels_per_point_ = kPhonePixelsPerPoint;
  ui_scale_ = kPhoneScale;
  ui_min_frame_height_ = kFingertip;
  ASSERT_TRUE(make_view(/*playing=*/false));
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(present());
  }
  ImGui::SetCurrentContext(view_->ui_context());
  ImGuiWindow* const panel = ImGui::FindWindowByName(kControlsWindow);
  ASSERT_NE(panel, nullptr);
  // The width ImGui fits the panel to. Before the first frame the status
  // line says "loading", so it fits the row.
  const float padding = 2.0f * panel->WindowPadding.x;
  EXPECT_LE(panel->ContentSize.x + padding, widest_panel());
  FrameStamp last;
  last.index = 99'998;
  EXPECT_LE(text_width(*panel, status_line(last, 99'999, true, true)) + padding,
            widest_panel());
  EXPECT_GE(
      vp_test::control_size(*panel, 0).x,
      text_width(*panel, "Pause") + 2.0f * ImGui::GetStyle().FramePadding.x);
  const ImGuiID controls[] = {panel->GetID("###play"), panel->GetID("Step"),
                              panel->GetID("Loop")};
  for (int i = 0; i < 3; ++i) {
    const glm::vec2 corner = vp_test::control_top_left(*panel, i);
    for (const float dx : {0.5f, kFingertip - 0.5f}) {
      for (const float dy : {0.5f, kFingertip - 0.5f}) {
        EXPECT_EQ(item_under(corner + glm::vec2(dx, dy)), controls[i])
            << "control " << i << " at +" << dx << ", +" << dy;
      }
    }
  }
  // The fingers pressed nothing.
  EXPECT_FALSE(view_->player().playing());
  EXPECT_TRUE(view_->player().loop());
  EXPECT_FALSE(view_->shown().has_value());
}

// On the narrowest phone, at the iOS shell's sizes, playing, past a refresh
// so the stats hold a frame's figures: their table has the worker's three
// rows and the profiler's draw row, the panel fits across and ends above the
// controls, and its header is a fingertip tall.
TEST_F(PlayerViewTest, TheStatsFitAPhone) {
  extent_ = kPhone;
  pixels_per_point_ = kPhonePixelsPerPoint;
  ui_scale_ = kPhoneScale;
  ui_min_frame_height_ = kFingertip;
  ASSERT_TRUE(make_view());
  ASSERT_TRUE(tick_until([&] { return view_->shown().has_value(); }));
  tick_for(2 * StatsPanel::kRefresh);
  ImGui::SetCurrentContext(view_->ui_context());
  const ImGuiWindow* const header = ImGui::FindWindowByName(kStatsWindow);
  ImGuiWindow* const body = stats_body();
  const ImGuiWindow* const controls = ImGui::FindWindowByName(kControlsWindow);
  ASSERT_NE(header, nullptr);
  ASSERT_NE(body, nullptr);
  ASSERT_NE(controls, nullptr);
  EXPECT_EQ(stats_rows(), 4);
  // The widths ImGui fits the body and the header to.
  EXPECT_LE(body->ContentSize.x + 2.0f * body->WindowPadding.x, widest_panel());
  EXPECT_LE(header->ContentSize.x + 2.0f * header->WindowPadding.x,
            widest_panel());
  EXPECT_LE(body->Pos.y + body->Size.y, controls->Pos.y);
  EXPECT_GE(header->Size.y, kFingertip);
}

// A tap on the stats' header, the top-left corner, closes them; a frame
// stepped meanwhile is still counted, so reopened, past a refresh, their
// table has the worker's rows besides the profiler's.
TEST_F(PlayerViewTest, TheStatsCountTheFramesTakenWhileClosed) {
  ASSERT_TRUE(make_view(/*playing=*/false));
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(present());
  }
  tap({4.0f, 4.0f});
  ImGui::SetCurrentContext(view_->ui_context());
  ASSERT_NE(stats_body(), nullptr);
  EXPECT_FALSE(stats_body()->Active) << "closed, the header alone";
  view_->player().step();
  ASSERT_TRUE(tick_until([&] { return view_->shown().has_value(); }));
  EXPECT_FALSE(stats_body()->Active);
  tap({4.0f, 4.0f});
  tick_for(2 * StatsPanel::kRefresh);
  ImGui::SetCurrentContext(view_->ui_context());
  EXPECT_TRUE(stats_body()->Active);
  EXPECT_EQ(stats_rows(), 4) << "read, decode, mesh and draw";
}

// Playing at a phone's size, where the queue can time: the draw row's device
// half spans the whole pass, so the median of five refreshes reads the pass,
// not the about 0 that a scope inside it reads on Apple GPUs (MoltenVK on an
// M5 Max: 0.004 to 0.008 ms inside, 0.015 to 0.1 ms around it).
TEST_F(PlayerViewTest, TheDrawRowTimesTheWholePass) {
  extent_ = kPhone;
  ASSERT_TRUE(make_view());
  ASSERT_TRUE(tick_until([&] { return view_->shown().has_value(); }));
  std::vector<double> gpu_ms;
  for (int i = 0; i < 5; ++i) {
    tick_for(StatsPanel::kRefresh);
    for (const core::StageRow& row : view_->stats_panel().rows()) {
      if (std::string(row.name) == "draw" && row.has_gpu) {
        gpu_ms.push_back(row.gpu_ms);
      }
    }
  }
  if (gpu_ms.empty()) {
    return;  // the graphics queue cannot time
  }
  std::nth_element(gpu_ms.begin(), gpu_ms.begin() + gpu_ms.size() / 2,
                   gpu_ms.end());
  EXPECT_GT(gpu_ms[gpu_ms.size() / 2], 0.015) << "ms, the median";
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

// On the host: FIFO and D32 depth; live, two images, so a mesh taken shows a
// vsync sooner, and a file's, gfx's default.
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
      gfx::app::make_shared_device(surface_, recon::device_requirements(),
                                   device_config);
  ASSERT_TRUE(shared.ok()) << shared.status().message();
  EXPECT_EQ(shared.value()->plan(), core::QueuePlan::TwoFamilies)
      << core::to_string(shared.value()->plan());
}
#endif

}  // namespace
