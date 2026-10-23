// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file controls_layout.hpp
/// Where draw_controls lays out its row, Deblock on it or below it, and
/// draw_source its button, for the tests that put a finger on a control.

#include <cfloat>
#include <cmath>
#include <string>

#include <glm/vec2.hpp>

#include "imgui.h"
#include "imgui_internal.h"  // ImGuiWindow
#include "volumetric_kit/player/controls.hpp"

namespace vp_test {

// The width of `text` in `panel`'s font as of the last frame, in points,
// rounded up as ImGui rounds a line's. The panel's context is current.
inline float text_width(const ImGuiWindow& panel, const std::string& text) {
  ImFont* const font = ImGui::GetIO().Fonts->Fonts[0];
  return std::ceil(
      font->CalcTextSizeA(panel.FontRefSize, FLT_MAX, 0.0f, text.c_str()).x);
}

// The index of Deblock's box, the row's last control (control_top_left).
constexpr int kDeblock = 4;

// The size of the row's `index`th control, in points, as the last frame laid
// out `panel`: Play/Pause, Step, Loop's box, Normals, then Deblock's box. Each
// is a frame tall; the buttons are kButtonEms font sizes wide, and the boxes
// are square. A live player's row is Normals and Deblock, laid out as
// Play/Pause and Deblock are (live_deblock_top_left). The panel's context is
// current. The font size is the panel's as of that frame: between frames
// ImGui's own is the unscaled one.
inline glm::vec2 control_size(const ImGuiWindow& panel, int index) {
  const float font = panel.FontRefSize;
  const float height = font + 2.0f * ImGui::GetStyle().FramePadding.y;
  const float button = volumetric_kit::player::kButtonEms * font;
  return {index == 2 || index == kDeblock ? height : button, height};
}

// The width the row gives its `index`th control: its size's, and for a box
// its label's after it.
inline float control_width(const ImGuiWindow& panel, int index) {
  if (index != 2 && index != kDeblock) {
    return control_size(panel, index).x;
  }
  return control_size(panel, index).x + ImGui::GetStyle().ItemInnerSpacing.x +
         text_width(panel, index == 2 ? "Loop" : "Deblock");
}

// Whether Deblock is on the row, after Normals: when the panel, so widened,
// fits the widest ImGui lets an auto-sized panel grow, the work area less its
// safe-area padding on each side; else it is below the row.
inline bool deblock_on_row(const ImGuiWindow& panel) {
  const ImGuiStyle& style = ImGui::GetStyle();
  float width = 2.0f * style.WindowPadding.x;
  for (int i = 0; i <= kDeblock; ++i) {
    width += control_width(panel, i) + (i > 0 ? style.ItemSpacing.x : 0.0f);
  }
  return width <= ImGui::GetMainViewport()->WorkSize.x -
                      2.0f * style.DisplaySafeAreaPadding.x;
}

// Its top left, in points.
inline glm::vec2 control_top_left(const ImGuiWindow& panel, int index) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float left = panel.Pos.x + style.WindowPadding.x;
  const float top = panel.Pos.y + style.WindowPadding.y;
  if (index == kDeblock && !deblock_on_row(panel)) {
    return {left, top + control_size(panel, 0).y + style.ItemSpacing.y};
  }
  float x = left;
  for (int i = 0; i < index; ++i) {
    x += control_width(panel, i) + style.ItemSpacing.x;
  }
  return {x, top};
}

// A live player's Deblock's top left, in points: after Normals, which is laid
// out as Play/Pause is.
inline glm::vec2 live_deblock_top_left(const ImGuiWindow& panel) {
  return control_top_left(panel, 1);
}

// Its middle, in points.
inline glm::vec2 control_middle(const ImGuiWindow& panel, int index) {
  return control_top_left(panel, index) + 0.5f * control_size(panel, index);
}

// A live player's Deblock's middle, in points.
inline glm::vec2 live_deblock_middle(const ImGuiWindow& panel) {
  return live_deblock_top_left(panel) + 0.5f * control_size(panel, kDeblock);
}

// The Source button's top left, in points, as the last frame laid out its
// window (`source`, draw_source): the window's, as it has no padding.
inline glm::vec2 source_top_left(const ImGuiWindow& source) {
  return {source.Pos.x, source.Pos.y};
}

// Its middle, in points: it is sized as Play/Pause.
inline glm::vec2 source_middle(const ImGuiWindow& source) {
  return source_top_left(source) + 0.5f * control_size(source, 0);
}

}  // namespace vp_test
