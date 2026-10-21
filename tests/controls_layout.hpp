// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file controls_layout.hpp
/// Where draw_controls lays out its row, for the tests that put a finger on
/// a control.

#include <glm/vec2.hpp>

#include "imgui.h"
#include "imgui_internal.h"  // ImGuiWindow
#include "volumetric_kit/player/controls.hpp"

namespace vp_test {

// The size of the row's `index`th control, in points, as the last frame laid
// out `panel`: Play/Pause, Step, then Loop's box. Each is a frame tall; the
// buttons are kButtonEms font sizes wide, and the box is square. The panel's
// context is current. The font size is the panel's as of that frame: between
// frames ImGui's own is the unscaled one.
inline glm::vec2 control_size(const ImGuiWindow& panel, int index) {
  const float font = panel.FontRefSize;
  const float height = font + 2.0f * ImGui::GetStyle().FramePadding.y;
  const float button = volumetric_kit::player::kButtonEms * font;
  return {index < 2 ? button : height, height};
}

// Its top left, in points.
inline glm::vec2 control_top_left(const ImGuiWindow& panel, int index) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float button = control_size(panel, 0).x;
  return {panel.Pos.x + style.WindowPadding.x +
              static_cast<float>(index) * (button + style.ItemSpacing.x),
          panel.Pos.y + style.WindowPadding.y};
}

// Its middle, in points.
inline glm::vec2 control_middle(const ImGuiWindow& panel, int index) {
  return control_top_left(panel, index) + 0.5f * control_size(panel, index);
}

}  // namespace vp_test
