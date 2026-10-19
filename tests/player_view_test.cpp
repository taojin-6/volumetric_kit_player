// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// PlayerView on a device, over gfx's headless surface and the tests' stream
// (test_stream.hpp), with gfx's ImGuiTouch as the platform half: frames
// present and shown() advances; 0 x 0 skips without blocking; a step moves one
// frame; the worker's failure surfaces from frame(); ui_wants_pointer()
// follows a finger on the panel; and teardown mid-playback is clean. The view
// makes its own shared device, validated as the test policy asks, so the
// layer's errors fail the test. The tests skip where the loader offers no
// headless surface (NVIDIA's ICD); on Apple a skip fails (CMakeLists.txt).

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

#include "imgui.h"
#include "imgui_internal.h"  // FindWindowByName, to find the panel
#include "test_stream.hpp"
#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/gfx/ui/imgui_touch.hpp"
#include "volumetric_kit/gfx/windowing/surface_source.hpp"
#include "volumetric_kit/player/controls.hpp"
#include "volumetric_kit/recon/core/device_requirements.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace gfx = volumetric_kit::gfx;
namespace recon = volumetric_kit::recon;
using namespace std::chrono_literals;
using volumetric_kit::player::kControlsWindow;
using volumetric_kit::player::PlayerView;
using volumetric_kit::player::PlayerViewConfig;
using volumetric_kit::player::StreamPlayer;
using SteadyClock = std::chrono::steady_clock;

constexpr VkExtent2D kExtent{320, 240};
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

  // The view over the stream at `path`, paused first unless `playing`, with
  // the touch half bound to its controls; false with a failure recorded.
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
    PlayerViewConfig config;
    config.validation = active_validation() != core::test::Validation::Off;
    core::Result<std::unique_ptr<PlayerView>> view = PlayerView::create(
        surface_, kExtent, std::move(opened).value(), config);
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
    display.pixels = kExtent;
    display.seconds = seconds_;
    touch_->new_frame(display);
    return view_->frame(kExtent);
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

  vp_test::Scratch scratch_;
  const std::string path_ = scratch_.file("sphere.vrst");
  gfx::windowing::SurfaceSource surface_;
  std::unique_ptr<PlayerView> view_;
  std::unique_ptr<gfx::ui::ImGuiTouch> touch_;  // destroyed before the view
  double seconds_ = 0.0;
};

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
  for (int i = 0; i < 3; ++i) {
    const core::Result<bool> ticked = tick();
    ASSERT_TRUE(ticked.ok()) << ticked.status().message();
    presented += ticked.value() ? 1 : 0;
  }
  EXPECT_EQ(presented, 3u) << "a headless surface never settles";
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
  touch_->touch_began(2, {4.0f, 4.0f});  // the top-left corner: the scene
  ASSERT_TRUE(present());
  EXPECT_FALSE(view_->ui_wants_pointer());
  touch_->touch_ended(2);
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

}  // namespace
