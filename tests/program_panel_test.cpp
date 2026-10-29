// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// A program's panel (ProgramPanel) on CPU-only ImGui frames, the display fed
// through gfx's ImGuiTouch as the iOS shell feeds it: on the six screens of
// its design, two desktops and an iPhone and an iPad each way up, with their
// safe areas, it docks at the end of the drawable's long axis at the share
// the shell gives it, the scene up to its inner edge, its content past the
// handle's bar and below the header in the safe area, and the controls' area
// a frame height short of the scene's edge; its share is held to 25-60 % and
// to 240 points across, 60 % at most; a drag on the handle moves the edge
// with the pointer, snaps to the detents on release, and closes the panel
// below 25 %, and a click changes nothing; the header holds the title, the
// program's items and a close button; over_content is the content while
// open; and closed, the scene is the whole drawable, the controls' area the
// work area, and no chrome or item is drawn.

#include "volumetric_kit/player/program_panel.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include <glm/vec2.hpp>

#include "imgui.h"
#include "imgui_internal.h"  // FindWindowByName, for the chrome's rects
#include "volumetric_kit/core/vulkan/vulkan.hpp"
#include "volumetric_kit/gfx/ui/imgui_touch.hpp"
#include "volumetric_kit/player/controls.hpp"
#include "volumetric_kit/player/frame_hooks.hpp"

namespace {

namespace ui = volumetric_kit::gfx::ui;
using volumetric_kit::player::DockSide;
using volumetric_kit::player::kPanelHandleWindow;
using volumetric_kit::player::kPanelHeaderWindow;
using volumetric_kit::player::PanelFrame;
using volumetric_kit::player::PanelState;
using volumetric_kit::player::ProgramPanel;
using volumetric_kit::player::UiArea;
using volumetric_kit::player::work_area;

// A screen: its size in points, pixels per point, safe area (top, left,
// bottom, right, in points), the share a shell gives the panel there, and
// the side the panel docks at.
struct Screen {
  const char* name;
  glm::vec2 points;
  float scale;
  ui::EdgeInsets safe;
  float share;
  DockSide side;
  VkExtent2D pixels() const {
    return {static_cast<std::uint32_t>(points.x * scale),
            static_cast<std::uint32_t>(points.y * scale)};
  }
};

const Screen kScreens[] = {
    {"MacBook Pro 16", {1728, 1117}, 2.0f, {}, 0.4f, DockSide::kRight},
    {"Linux 4K", {3840, 2160}, 1.0f, {}, 0.4f, DockSide::kRight},
    {"iPhone portrait",
     {440, 956},
     3.0f,
     {62, 0, 34, 0},
     0.45f,
     DockSide::kBottom},
    {"iPhone landscape",
     {956, 440},
     3.0f,
     {0, 62, 21, 62},
     0.45f,
     DockSide::kRight},
    {"iPad landscape",
     {1210, 834},
     2.0f,
     {24, 0, 20, 0},
     0.4f,
     DockSide::kRight},
    {"iPad portrait",
     {834, 1210},
     2.0f,
     {24, 0, 20, 0},
     0.4f,
     DockSide::kBottom}};
const Screen& kPhonePortrait = kScreens[2];
const Screen& kPadLandscape = kScreens[4];

PanelState open_at(float share) {
  PanelState state;
  state.open = true;
  state.right_share = share;
  state.bottom_share = share;
  return state;
}

float right_of(const VkRect2D& r) {
  return static_cast<float>(r.offset.x) + static_cast<float>(r.extent.width);
}
float bottom_of(const VkRect2D& r) {
  return static_cast<float>(r.offset.y) + static_cast<float>(r.extent.height);
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

  // One frame of `panel` on `screen`: its layout and its chrome, the
  // program's items counted; with `frame` and `area`, what the layout gave
  // and the frame height and work area it was made under.
  struct Laid {
    PanelFrame frame;
    VkRect2D scene;
    UiArea area;
    UiArea work;
    float frame_height = 0.0f;
  };
  Laid frame(ProgramPanel& panel, const Screen& screen) {
    seconds_ += 1.0 / 60.0;
    touch_->new_frame({screen.pixels(), screen.scale, screen.safe, seconds_});
    ImGui::NewFrame();
    Laid laid;
    laid.frame = panel.layout(screen.pixels());
    laid.scene = panel.scene();
    laid.area = panel.scene_area();
    laid.work = work_area();
    laid.frame_height = ImGui::GetFrameHeight();
    panel.draw_chrome("Program panel", [this] {
      ++items_;
      in_header_ = ImGui::GetCurrentWindow()->Name;
      ImGui::Button("Reset");
    });
    ImGui::Render();
    return laid;
  }

  // The pointer to `from` (points), pressed, moved to `to` in two steps and
  // released there, a frame each, and a frame laid out after.
  void drag(ProgramPanel& panel, const Screen& screen, ImVec2 from, ImVec2 to) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(from.x, from.y);
    frame(panel, screen);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame(panel, screen);
    io.AddMousePosEvent(0.5f * (from.x + to.x), 0.5f * (from.y + to.y));
    frame(panel, screen);
    io.AddMousePosEvent(to.x, to.y);
    frame(panel, screen);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame(panel, screen);
    frame(panel, screen);
  }

  static ImRect window_rect(const char* name) {
    const ImGuiWindow* window = ImGui::FindWindowByName(name);
    EXPECT_NE(window, nullptr) << name;
    return window != nullptr ? window->Rect() : ImRect();
  }

  // Whether the last frame drew window `name`.
  static bool drawn(const char* name) {
    const ImGuiWindow* window = ImGui::FindWindowByName(name);
    return window != nullptr && window->Active;
  }

  ImGuiContext* context_ = nullptr;
  std::unique_ptr<ui::ImGuiTouch> touch_;  // reset before the context
  double seconds_ = 0.0;
  int items_ = 0;               // the program's items' calls
  const char* in_header_ = "";  // the window they were drawn in
};

TEST_F(ProgramPanelTest, DocksAtTheLongAxisEndOnTheSixScreens) {
  for (const Screen& screen : kScreens) {
    SCOPED_TRACE(screen.name);
    ProgramPanel panel(open_at(screen.share));
    const Laid laid = frame(panel, screen);
    const VkExtent2D pixels = screen.pixels();
    const float s = screen.scale;
    const float safe[4] = {
        screen.safe.left * s, screen.safe.top * s,
        static_cast<float>(pixels.width) - screen.safe.right * s,
        static_cast<float>(pixels.height) - screen.safe.bottom * s};
    const float bar = std::round(ProgramPanel::kBarPoints * s);
    const float header = std::round(
        (laid.frame_height + 2.0f * ImGui::GetStyle().FramePadding.y) * s);
    const PanelFrame& f = laid.frame;
    ASSERT_EQ(f.side, screen.side);
    EXPECT_EQ(f.drawable.width, pixels.width);
    EXPECT_EQ(f.drawable.height, pixels.height);
    const bool right = f.side == DockSide::kRight;
    const float length = right ? safe[2] - safe[0] : safe[3] - safe[1];
    const float across = std::round(screen.share * length);
    ASSERT_GE(across, ProgramPanel::kMinPoints * s) << "the share binds";
    const float edge = (right ? safe[2] : safe[3]) - across;
    EXPECT_EQ(laid.scene.offset.x, 0);
    EXPECT_EQ(laid.scene.offset.y, 0);
    if (right) {
      EXPECT_EQ(right_of(laid.scene), edge);
      EXPECT_EQ(laid.scene.extent.height, pixels.height);
      EXPECT_EQ(static_cast<float>(f.content.offset.x), edge + bar);
      EXPECT_EQ(static_cast<float>(f.content.offset.y), safe[1] + header);
      EXPECT_NEAR(laid.area.pos.x + laid.area.size.x,
                  edge / s - laid.frame_height, 1e-3f);
      EXPECT_EQ(laid.area.size.y, laid.work.size.y);
    } else {
      EXPECT_EQ(bottom_of(laid.scene), edge);
      EXPECT_EQ(laid.scene.extent.width, pixels.width);
      EXPECT_EQ(static_cast<float>(f.content.offset.x), safe[0]);
      EXPECT_EQ(static_cast<float>(f.content.offset.y), edge + bar + header);
      EXPECT_NEAR(laid.area.pos.y + laid.area.size.y,
                  edge / s - laid.frame_height, 1e-3f);
      EXPECT_EQ(laid.area.size.x, laid.work.size.x);
    }
    EXPECT_EQ(right_of(f.content), safe[2]);
    EXPECT_EQ(bottom_of(f.content), safe[3]);
    EXPECT_EQ(laid.area.pos.x, laid.work.pos.x);
    EXPECT_EQ(laid.area.pos.y, laid.work.pos.y);
    // The chrome in the safe area, the header over the content: within a
    // point, as ImGui places windows on whole points.
    const ImRect header_rect = window_rect(kPanelHeaderWindow);
    const ImRect handle_rect = window_rect(kPanelHandleWindow);
    for (const ImRect& r : {header_rect, handle_rect}) {
      EXPECT_GE(r.Min.x * s, safe[0] - s);
      EXPECT_GE(r.Min.y * s, safe[1] - s);
      EXPECT_LE(r.Max.x * s, safe[2] + s);
      EXPECT_LE(r.Max.y * s, safe[3] + s);
    }
    EXPECT_NEAR(header_rect.Max.y * s, static_cast<float>(f.content.offset.y),
                s);
    EXPECT_NEAR(right ? header_rect.Min.x * s : header_rect.Min.y * s,
                edge + bar, s);
  }
}

// Held to 25-60 % of the safe length, and to 240 points across, but never
// past 60 %.
TEST_F(ProgramPanelTest, ItsShareIsHeldAndItsLeastSizeKept) {
  const Screen wide{"wide", {1000, 600}, 1.0f, {}, 0.0f, DockSide::kRight};
  const Screen narrow{"narrow", {300, 200}, 1.0f, {}, 0.0f, DockSide::kRight};
  struct Case {
    const Screen* screen;
    float share;
    float across;  // in pixels
  };
  for (const Case& c :
       {Case{&wide, 0.1f, 250.0f}, Case{&wide, 0.4f, 400.0f},
        Case{&wide, 0.9f, 600.0f}, Case{&narrow, 0.25f, 180.0f}}) {
    SCOPED_TRACE(c.share);
    SCOPED_TRACE(c.screen->name);
    ProgramPanel panel(open_at(c.share));
    const Laid laid = frame(panel, *c.screen);
    EXPECT_EQ(c.screen->points.x - right_of(laid.scene), c.across);
  }
  // 25 % of 800 is 200, under the 240-point least.
  const Screen mid{"mid", {800, 600}, 2.0f, {}, 0.0f, DockSide::kRight};
  ProgramPanel panel(open_at(0.25f));
  const Laid laid = frame(panel, mid);
  EXPECT_EQ(1600.0f - right_of(laid.scene), 480.0f);
}

// On an iPad, free: the edge follows the pointer. On an iPhone, with its
// detents, a release snaps to the nearest; one below 25 % closes the panel
// and keeps the share it had; a click on the handle changes nothing.
TEST_F(ProgramPanelTest, ADragOnTheHandleResizesSnapsAndCloses) {
  {
    ProgramPanel panel(open_at(0.4f));
    frame(panel, kPadLandscape);
    const ImRect handle = window_rect(kPanelHandleWindow);
    const ImVec2 grab = handle.GetCenter();
    drag(panel, kPadLandscape, grab, ImVec2(grab.x - 121.0f, grab.y));
    EXPECT_NEAR(panel.state().right_share, 0.4f + 121.0f / 1210.0f, 1e-4f);
    EXPECT_TRUE(panel.open());
    drag(panel, kPadLandscape, ImVec2(grab.x - 121.0f, grab.y),
         ImVec2(grab.x - 117.0f, grab.y));
    EXPECT_NEAR(panel.state().right_share, 0.4f + 121.0f / 1210.0f, 1e-4f)
        << "within the drag threshold: a click";
  }
  ProgramPanel panel(open_at(0.45f), {0.45f, 0.6f});
  frame(panel, kPhonePortrait);
  const float length = 956.0f - 62.0f - 34.0f;  // the safe height, in points
  const ImVec2 grab = window_rect(kPanelHandleWindow).GetCenter();
  drag(panel, kPhonePortrait, grab, ImVec2(grab.x, grab.y - 0.1f * length));
  EXPECT_EQ(panel.state().bottom_share, 0.6f) << "0.55 snaps to 0.6";
  const ImVec2 moved = window_rect(kPanelHandleWindow).GetCenter();
  drag(panel, kPhonePortrait, moved, ImVec2(moved.x, moved.y + 0.2f * length));
  EXPECT_EQ(panel.state().bottom_share, 0.45f) << "0.4 snaps to 0.45";
  EXPECT_TRUE(panel.open());
  const ImVec2 again = window_rect(kPanelHandleWindow).GetCenter();
  drag(panel, kPhonePortrait, again, ImVec2(again.x, again.y + 0.3f * length));
  EXPECT_FALSE(panel.open()) << "released at 15 %";
  EXPECT_EQ(panel.state().bottom_share, 0.45f);
}

// The header: the title, then the program's items in its window, then the
// close button, which closes the panel; closed, the scene is the whole
// drawable, the content empty, the controls' area the work area, and neither
// the chrome nor the program's items are drawn.
TEST_F(ProgramPanelTest, CloseClosesAndClosedTheSceneIsTheWholeDrawable) {
  ProgramPanel panel(open_at(0.4f));
  frame(panel, kPadLandscape);
  EXPECT_EQ(items_, 1);
  EXPECT_STREQ(in_header_, kPanelHeaderWindow);
  const ImRect header = window_rect(kPanelHeaderWindow);
  const float frame_height = ImGui::GetFrameHeight();
  const ImVec2 close(
      header.Max.x - ImGui::GetStyle().WindowPadding.x - 0.5f * frame_height,
      header.GetCenter().y);
  ImGuiIO& io = ImGui::GetIO();
  io.AddMousePosEvent(close.x, close.y);
  frame(panel, kPadLandscape);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
  frame(panel, kPadLandscape);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
  frame(panel, kPadLandscape);
  EXPECT_FALSE(panel.open());
  const int items = items_;
  const Laid laid = frame(panel, kPadLandscape);
  EXPECT_EQ(items_, items) << "closed, the program's items are not drawn";
  EXPECT_FALSE(drawn(kPanelHeaderWindow));
  EXPECT_FALSE(drawn(kPanelHandleWindow));
  EXPECT_EQ(laid.scene.extent.width, kPadLandscape.pixels().width);
  EXPECT_EQ(laid.scene.extent.height, kPadLandscape.pixels().height);
  EXPECT_EQ(laid.frame.content.extent.width, 0u);
  EXPECT_EQ(laid.area.pos, laid.work.pos);
  EXPECT_EQ(laid.area.size, laid.work.size);
  EXPECT_FALSE(panel.laid_out_open());
}

// Over the content while open, not over the header, the scene or a closed
// panel.
TEST_F(ProgramPanelTest, OverContentIsTheContentWhileOpen) {
  ProgramPanel panel(open_at(0.4f));
  const Laid laid = frame(panel, kPadLandscape);
  const VkRect2D& c = laid.frame.content;
  const glm::vec2 inside(static_cast<float>(c.offset.x) + 1.0f,
                         static_cast<float>(c.offset.y) + 1.0f);
  EXPECT_TRUE(panel.over_content(inside));
  EXPECT_FALSE(panel.over_content(inside - glm::vec2(0.0f, 2.0f)))
      << "the header";
  EXPECT_FALSE(panel.over_content(inside - glm::vec2(2.0f, 0.0f)))
      << "the handle's bar";
  EXPECT_FALSE(panel.over_content({right_of(c), inside.y}));
  panel.set_open(false);
  EXPECT_FALSE(panel.over_content(inside));
  frame(panel, kPadLandscape);
  EXPECT_FALSE(panel.over_content(inside));
}

}  // namespace
