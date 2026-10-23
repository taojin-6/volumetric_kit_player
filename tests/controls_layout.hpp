// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file controls_layout.hpp
/// Where draw_controls lays out its row, and draw_source its button, for the
/// tests that put a finger on a control; and the stats' tables, for those
/// that count their rows.

#include <cfloat>
#include <cmath>
#include <string>
#include <vector>

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

// The size of the row's `index`th control, in points, as the last frame laid
// out `panel`: Play/Pause, Step, Loop's box, then Normals. Each is a frame
// tall; the buttons are kButtonEms font sizes wide, and the box is square. A
// live player's row is Normals alone, laid out as Play/Pause is. The panel's
// context is current. The font size is the panel's as of that frame: between
// frames ImGui's own is the unscaled one.
inline glm::vec2 control_size(const ImGuiWindow& panel, int index) {
  const float font = panel.FontRefSize;
  const float height = font + 2.0f * ImGui::GetStyle().FramePadding.y;
  const float button = volumetric_kit::player::kButtonEms * font;
  return {index == 2 ? height : button, height};
}

// The width the row gives its `index`th control: its size's, and for Loop's
// box its label's after it.
inline float control_width(const ImGuiWindow& panel, int index) {
  if (index != 2) {
    return control_size(panel, index).x;
  }
  return control_size(panel, 2).x + ImGui::GetStyle().ItemInnerSpacing.x +
         text_width(panel, "Loop");
}

// Its top left, in points.
inline glm::vec2 control_top_left(const ImGuiWindow& panel, int index) {
  const ImGuiStyle& style = ImGui::GetStyle();
  float x = panel.Pos.x + style.WindowPadding.x;
  for (int i = 0; i < index; ++i) {
    x += control_width(panel, i) + style.ItemSpacing.x;
  }
  return {x, panel.Pos.y + style.WindowPadding.y};
}

// Its middle, in points.
inline glm::vec2 control_middle(const ImGuiWindow& panel, int index) {
  return control_top_left(panel, index) + 0.5f * control_size(panel, index);
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

// The stats' table `id`, "stages" or "render", as the last frame drew it: in
// one of the compact stats' columns, the body's child windows, or in the body
// itself; null with none drawn. The stats' context is current.
inline const ImGuiTable* stats_table(const char* id) {
  ImGuiWindow* const body = ImGui::FindWindowByName(
      (std::string(volumetric_kit::player::kStatsWindow) + "##body").c_str());
  if (body == nullptr) {
    return nullptr;
  }
  std::vector<ImGuiWindow*> windows;
  for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
    if (window->ParentWindow == body) {
      windows.push_back(window);
    }
  }
  windows.push_back(body);
  for (ImGuiWindow* window : windows) {
    const ImGuiTable* table = ImGui::TableFindByID(window->GetID(id));
    if (table != nullptr && table->LastFrameActive == ImGui::GetFrameCount()) {
      return table;
    }
  }
  return nullptr;
}

}  // namespace vp_test
