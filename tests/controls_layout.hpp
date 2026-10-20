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

// The middle of the row's `index`th control, in points, as the last frame
// laid out `panel`: Play/Pause, Step, then Loop's box. Each is a frame tall;
// the buttons are kButtonEms font sizes wide, and the box is square. The
// panel's context is current. The font size is the panel's as of that frame:
// between frames ImGui's own is the unscaled one.
inline glm::vec2 control_middle(const ImGuiWindow& panel, int index) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float font = panel.FontRefSize;
  const float height = font + 2.0f * style.FramePadding.y;
  const float button = volumetric_kit::player::kButtonEms * font;
  const float left = panel.Pos.x + style.WindowPadding.x +
                     static_cast<float>(index) * (button + style.ItemSpacing.x);
  const float top = panel.Pos.y + style.WindowPadding.y;
  const float width = index < 2 ? button : height;
  return {left + 0.5f * width, top + 0.5f * height};
}

}  // namespace vp_test
