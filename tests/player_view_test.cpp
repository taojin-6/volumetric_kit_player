// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// PlayerView on a device, over gfx's headless surface and the tests' stream
// (test_stream.hpp), with gfx's ImGuiTouch as the platform half: create
// refuses what it cannot use; frames present and shown() advances, their
// triangles sharing vertices, onto a frame that meshes nothing too, which
// still presents; 0 x 0 skips without blocking; a step moves one frame; the
// worker's failure surfaces from frame(); a tap on Normals switches the mesh's
// flags from lit to normals and back, and frames keep presenting; the panel
// draws at a phone's scale; in portrait and in landscape, at the iOS shell's
// sizes, its controls fit a phone and a fingertip, as does Source, clear of
// them, and so do the compact stats, clear of both, with the profiler's draw
// row and its loop's in the renderer's table, the draw's device half timing
// the whole pass; the worker and the draw are device-timed while the stats
// are open, not while they are closed, and the worker as a measurement fixes
// it whatever they show, the draw then only while they are open and the
// timing is not off, each frame taken counted once; Source is off by
// default, and a tap on it is one request; ui_wants_pointer() follows a finger
// on the panel, which controls_test.cpp checks on the stats' header and body;
// and teardown mid-playback, the profiler attached, is clean. A live view
// presents, on the two images its swapchain asks for (checked on the host).
// The view makes its own shared device, validated as the test policy asks, so
// the layer's errors fail the test. The tests skip where the loader offers no
// headless surface (NVIDIA's ICD), but fail on Apple, where MoltenVK has
// one. MoltenVK plans two queue families, so the mesh's
// buffers are CONCURRENT and read across. One family, EXCLUSIVE and read in
// place, is tested on the host only (mesh_wiring_test.cpp) until a leg runs
// the view on one.

#include "volumetric_kit/player/player_view.hpp"

#include <gtest/gtest.h>

#include <chrono>
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
using volumetric_kit::player::DeviceTiming;
using volumetric_kit::player::FrameStamp;
using volumetric_kit::player::kControlsWindow;
using volumetric_kit::player::kSourceWindow;
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
// How long a frame the test let go may take to arrive (the worker builds its
// kernels first); how long one it did not is waited for; and the longest a
// call that must not block takes.
constexpr auto kArrival = 10s;
constexpr auto kQuiet = 300ms;
constexpr auto kPrompt = 1s;

class PlayerViewTest : public core::test::VulkanTest {
 protected:
  // The fixture's physical device says whether there is one for the view,
  // which makes its own device.
  core::DeviceRequirements requirements() const override {
    return recon::device_requirements();
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
  // Source while `source_button_`, validated as the test policy asks.
  PlayerViewConfig config() const {
    PlayerViewConfig config;
    config.ui_scale = ui_scale_;
    config.ui_min_frame_height = ui_min_frame_height_;
    config.source_button = source_button_;
    config.validation = active_validation() != core::test::Validation::Off;
    config.compact_stats = compact_stats_;
    config.device_timing = device_timing_;
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
      if (std::string(row.name) == "draw") {
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
// landscape, at the iOS shell's sizes, Source included: a fingertip's square
// at each control's top left and at Source's lands on it, as does Source's far
// edge; "Pause", "Normals" and "Source" fit their buttons; the panel fits
// across, both its row and the widest status line, a 5-digit track's playing
// in a loop; and Source ends above it, so the two never overlap.
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
        {source->GetID("Source"), vp_test::source_top_left(*source)}};
    for (int i = 0; i < 5; ++i) {
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
              targets[4].id);
    EXPECT_LE(source->Pos.y + source->Size.y, panel->Pos.y);
    // The fingers pressed nothing.
    EXPECT_FALSE(view_->player().playing());
    EXPECT_TRUE(view_->player().loop());
    EXPECT_FALSE(view_->normals());
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
// playing, past a refresh so the stats hold a frame's figures: their tables
// have the worker's three stages and the renderer's draw row and its loop's
// three, the panel fits across, ends above the controls and starts below
// Source, and its header is a fingertip tall, left of Source.
TEST_F(PlayerViewTest, TheStatsFitAPhone) {
  pixels_per_point_ = kPhonePixelsPerPoint;
  ui_scale_ = kPhoneScale;
  ui_min_frame_height_ = kFingertip;
  source_button_ = true;
  compact_stats_ = true;
  for (const Phone& phone : kPhones) {
    SCOPED_TRACE(phone.pixels.width);
    touch_.reset();
    view_.reset();
    extent_ = phone.pixels;
    safe_area_ = phone.safe_area;
    ASSERT_TRUE(make_view());
    ASSERT_TRUE(tick_until([&] { return view_->shown().has_value(); }));
    tick_for(2 * StatsPanel::kRefresh);
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
    EXPECT_EQ(stats_rows("stages"), 3);
    EXPECT_EQ(stats_rows("render"), 4);
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
      if (std::string(row.name) == "draw" &&
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
  ASSERT_NO_FATAL_FAILURE(tap_stats_header());
  ASSERT_FALSE(view_->stats_panel().open());
  EXPECT_EQ(view_->player().device_timing(), DeviceTiming::kOff);
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
      gfx::app::make_shared_device(surface_, recon::device_requirements(),
                                   device_config);
  ASSERT_TRUE(shared.ok()) << shared.status().message();
  EXPECT_EQ(shared.value()->plan(), core::QueuePlan::TwoFamilies)
      << core::to_string(shared.value()->plan());
}
#endif

}  // namespace
