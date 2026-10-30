// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The scene's part of the work area beside a program's panel (scene_area),
// on CPU-only ImGui frames, the display fed through gfx's ImGuiTouch as the
// iOS shell feeds it. On six screens, two desktops and an iPhone and an iPad
// each way up, with their safe areas and gfx::ui::DockedPanel laying out the
// panel at the share the shell gives it: the area starts where the work area
// does and stops a frame height short of the scene's edge, clear of the
// panel's handle and header. Closed, it is the whole work area.

#include "volumetric_kit/player/program_panel.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>

#include <glm/vec2.hpp>

#include "imgui.h"
#include "imgui_internal.h"  // FindWindowByName, for the chrome's rects
#include "volumetric_kit/core/vulkan/vulkan.hpp"
#include "volumetric_kit/gfx/ui/docked_panel.hpp"
#include "volumetric_kit/gfx/ui/imgui_touch.hpp"
#include "volumetric_kit/player/controls.hpp"

namespace {

namespace ui = volumetric_kit::gfx::ui;
using volumetric_kit::player::scene_area;
using volumetric_kit::player::UiArea;
using volumetric_kit::player::work_area;

constexpr const char* kTitle = "Program panel";

// A screen: its size in points, pixels per point, safe area (top, left,
// bottom, right, in points), and the share a shell gives the panel there.
struct Screen {
  const char* name;
  glm::vec2 points;
  float scale;
  ui::EdgeInsets safe;
  float share;
  VkExtent2D pixels() const {
    return {static_cast<std::uint32_t>(points.x * scale),
            static_cast<std::uint32_t>(points.y * scale)};
  }
};

const Screen kScreens[] = {
    {"MacBook Pro 16", {1728, 1117}, 2.0f, {}, 0.4f},
    {"Linux 4K", {3840, 2160}, 1.0f, {}, 0.4f},
    {"iPhone portrait", {440, 956}, 3.0f, {62, 0, 34, 0}, 0.45f},
    {"iPhone landscape", {956, 440}, 3.0f, {0, 62, 21, 62}, 0.45f},
    {"iPad landscape", {1210, 834}, 2.0f, {24, 0, 20, 0}, 0.4f},
    {"iPad portrait", {834, 1210}, 2.0f, {24, 0, 20, 0}, 0.4f}};

ui::DockedPanelConfig open_at(float share) {
  ui::DockedPanelConfig config;
  config.state.open = true;
  config.state.right_share = share;
  config.state.bottom_share = share;
  return config;
}

class ProgramPanelTest : public ::testing::Test {
 protected:
  void SetUp() override {
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
  }

  void TearDown() override {
    touch_.reset();
    if (context_ != nullptr) {
      ImGui::DestroyContext(context_);
    }
  }

  // One frame of `panel` on `screen`: its layout, the scene's area, the work
  // area and the frame height it was made under, then the chrome.
  struct Laid {
    ui::PanelLayout layout;
    UiArea area;
    UiArea work;
    float frame_height = 0.0f;
  };
  Laid frame(ui::DockedPanel& panel, const Screen& screen) {
    seconds_ += 1.0 / 60.0;
    touch_->new_frame({screen.pixels(), screen.scale, screen.safe, seconds_});
    ImGui::NewFrame();
    Laid laid;
    laid.layout = panel.layout(screen.pixels());
    laid.area = scene_area(laid.layout, panel.open());
    laid.work = work_area();
    laid.frame_height = ImGui::GetFrameHeight();
    panel.draw_chrome(kTitle);
    ImGui::Render();
    return laid;
  }

  static ImRect window_rect(const std::string& name) {
    const ImGuiWindow* window = ImGui::FindWindowByName(name.c_str());
    EXPECT_NE(window, nullptr) << name;
    return window != nullptr ? window->Rect() : ImRect();
  }

  ImGuiContext* context_ = nullptr;
  std::unique_ptr<ui::ImGuiTouch> touch_;  // reset before the context
  double seconds_ = 0.0;
};

TEST_F(ProgramPanelTest, TheControlsKeepToTheScenesPartOnTheSixScreens) {
  for (const Screen& screen : kScreens) {
    SCOPED_TRACE(screen.name);
    ui::DockedPanel panel(open_at(screen.share));
    const Laid laid = frame(panel, screen);
    const float s = screen.scale;
    const UiArea& a = laid.area;
    EXPECT_EQ(a.pos.x, laid.work.pos.x);
    EXPECT_EQ(a.pos.y, laid.work.pos.y);
    const ImRect handle = window_rect(std::string(kTitle) + "##handle");
    const ImRect header = window_rect(std::string(kTitle) + "##header");
    if (laid.layout.side == ui::DockSide::Right) {
      EXPECT_NEAR(a.pos.x + a.size.x,
                  static_cast<float>(laid.layout.scene.extent.width) / s -
                      laid.frame_height,
                  1e-3f);
      EXPECT_EQ(a.size.y, laid.work.size.y);
      EXPECT_LE(a.pos.x + a.size.x, handle.Min.x + 0.5f);
      EXPECT_LE(a.pos.x + a.size.x, header.Min.x);
    } else {
      EXPECT_NEAR(a.pos.y + a.size.y,
                  static_cast<float>(laid.layout.scene.extent.height) / s -
                      laid.frame_height,
                  1e-3f);
      EXPECT_EQ(a.size.x, laid.work.size.x);
      EXPECT_LE(a.pos.y + a.size.y, handle.Min.y + 0.5f);
      EXPECT_LE(a.pos.y + a.size.y, header.Min.y);
    }
  }
}

TEST_F(ProgramPanelTest, ClosedTheControlsHaveTheWorkArea) {
  const Screen& screen = kScreens[2];
  ui::DockedPanel panel;  // closed
  const Laid laid = frame(panel, screen);
  EXPECT_EQ(laid.area.pos.x, laid.work.pos.x);
  EXPECT_EQ(laid.area.pos.y, laid.work.pos.y);
  EXPECT_EQ(laid.area.size.x, laid.work.size.x);
  EXPECT_EQ(laid.area.size.y, laid.work.size.y);
}

}  // namespace
