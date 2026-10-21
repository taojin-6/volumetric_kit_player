// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The controls on the host: status_line's wording, and draw_controls on a
// CPU-only ImGui context fed by gfx's ImGuiTouch, over a stream opened but not
// started. A tap on Play/Pause flips playing(), Step pauses, Loop toggles, a
// finger on the panel is ImGui's, and the panel keeps clear of the safe area.

#include "volumetric_kit/player/controls.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <glm/vec2.hpp>

#include "controls_layout.hpp"
#include "imgui.h"
#include "imgui_internal.h"  // FindWindowByName, to find the panel
#include "test_stream.hpp"
#include "volumetric_kit/gfx/ui/imgui_touch.hpp"
#include "volumetric_kit/player/stream_player.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace ui = volumetric_kit::gfx::ui;
using volumetric_kit::player::draw_controls;
using volumetric_kit::player::FrameStamp;
using volumetric_kit::player::kControlsWindow;
using volumetric_kit::player::status_line;
using volumetric_kit::player::StreamPlayer;

TEST(StatusLine, SaysLoadingBeforeTheFirstFrame) {
  EXPECT_EQ(status_line(std::nullopt, 150, true, true), "loading");
}

TEST(StatusLine, NamesTheFrameFromOneThePlayStateAndLoop) {
  const FrameStamp stamp{11, 440'000'000};
  EXPECT_EQ(status_line(stamp, 150, true, true),
            "frame 12/150 · playing · loop");
  EXPECT_EQ(status_line(stamp, 150, false, false), "frame 12/150 · paused");
}

// A 400 x 300-point display at 2 pixels per point, its bottom `inset` points
// kept clear.
ui::DisplayFrame display(double seconds, float inset = 0.0f) {
  ui::DisplayFrame frame;
  frame.pixels = {800, 600};
  frame.pixels_per_point = 2.0f;
  frame.safe_area.bottom = inset;
  frame.seconds = seconds;
  return frame;
}

class ControlsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const std::string path = scratch_.file("undecodable.vrst");
    const core::Status written = vp_test::write_undecodable_stream(path, 3);
    ASSERT_TRUE(written.ok()) << written.message();
    core::Result<std::unique_ptr<StreamPlayer>> opened =
        StreamPlayer::open(path);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    player_ = std::move(opened).value();

    // ImGui's own context, as the overlay's but with no renderer: its font
    // atlas built on the CPU, and no imgui.ini.
    context_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(context_);
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto touch = ui::ImGuiTouch::create(context_);
    ASSERT_TRUE(touch.ok()) << touch.status().message();
    touch_ = std::move(touch).value();
    // An auto-sized panel takes its size on its second frame.
    frame();
    frame();
    ASSERT_NE(ImGui::FindWindowByName(kControlsWindow), nullptr);
  }

  void TearDown() override {
    touch_.reset();
    if (context_ != nullptr) {
      ImGui::DestroyContext(context_);
    }
  }

  // One frame, 1/60 s after the last, with the controls in it.
  void frame(float inset = 0.0f) {
    seconds_ += 1.0 / 60.0;
    touch_->new_frame(display(seconds_, inset));
    ImGui::NewFrame();
    draw_controls(*player_, std::nullopt);
    ImGui::Render();
  }

  // The panel, as the last frame laid it out; SetUp found it.
  const ImGuiWindow& panel() const {
    return *ImGui::FindWindowByName(kControlsWindow);
  }

  // The middle of the row's `index`th control (controls_layout.hpp).
  glm::vec2 control(int index) const {
    return vp_test::control_middle(panel(), index);
  }

  // A finger lifts where it lands, and the frames ImGui takes the tap over
  // run: its position, its press, its release, and its position cleared.
  void tap(glm::vec2 at) {
    touch_->touch_began(1, at);
    touch_->touch_ended(1);
    for (int i = 0; i < 4; ++i) {
      frame();
    }
  }

  vp_test::Scratch scratch_;
  std::unique_ptr<StreamPlayer> player_;
  ImGuiContext* context_ = nullptr;
  std::unique_ptr<ui::ImGuiTouch> touch_;  // reset before the context
  double seconds_ = 0.0;
};

TEST_F(ControlsTest, ATapOnPlayPauseFlipsPlaying) {
  ASSERT_TRUE(player_->playing());
  tap(control(0));
  EXPECT_FALSE(player_->playing());
  tap(control(0));
  EXPECT_TRUE(player_->playing());
}

TEST_F(ControlsTest, StepPauses) {
  ASSERT_TRUE(player_->playing());
  tap(control(1));
  EXPECT_FALSE(player_->playing());
}

TEST_F(ControlsTest, LoopToggles) {
  ASSERT_TRUE(player_->loop());
  tap(control(2));
  EXPECT_FALSE(player_->loop());
  EXPECT_TRUE(player_->playing());
  tap(control(2));
  EXPECT_TRUE(player_->loop());
}

TEST_F(ControlsTest, AFingerOnThePanelIsImGuis) {
  const ImGuiIO& io = ImGui::GetIO();
  touch_->touch_began(1, control(1));
  frame();
  EXPECT_TRUE(io.WantCaptureMouse);
  touch_->touch_ended(1);
  frame();
  frame();
  frame();
  EXPECT_FALSE(io.WantCaptureMouse);
  touch_->touch_began(2, {10.0f, 10.0f});  // above the panel
  frame();
  EXPECT_FALSE(io.WantCaptureMouse);
  touch_->touch_ended(2);
}

// ImGui snaps a window's position to whole points.
TEST_F(ControlsTest, ThePanelSitsAtTheBottomCentreClearOfTheSafeArea) {
  const ImGuiWindow& window = panel();
  EXPECT_NEAR(window.Pos.x + 0.5f * window.Size.x, 200.0f, 1.0f);
  EXPECT_NEAR(window.Pos.y + window.Size.y, 300.0f, 1.0f);
  frame(/*inset=*/34.0f);
  EXPECT_NEAR(panel().Pos.y + panel().Size.y, 266.0f, 1.0f);
}

TEST_F(ControlsTest, TheFontDrawsTheStatusLinesDot) {
  EXPECT_TRUE(ImGui::GetFont()->IsGlyphInFont(0x00B7));
}

}  // namespace
