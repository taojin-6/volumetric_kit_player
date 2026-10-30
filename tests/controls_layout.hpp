// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file controls_layout.hpp
/// Where draw_controls lays out its row, Deblock, Unlit, Texture and a
/// program's panel's box on it or on lines below it, and draw_source its
/// button, for the tests that put a finger on a control; and the stats'
/// tables, for those that count their rows.

#include <cfloat>
#include <cmath>
#include <cstddef>
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

// The indices of Deblock's, Unlit's, Texture's and the panel's boxes, the
// row's last controls (control_top_left); Texture's and the panel's are drawn
// only when given, the panel's labelled kPanelLabel.
constexpr int kDeblock = 4;
constexpr int kUnlit = 5;
constexpr int kTexture = 6;
constexpr int kPanel = 7;
constexpr const char* kPanelLabel = "Panel";
// Mute's, the last box, drawn for a file with audio alone (mute_top_left).
constexpr int kMute = 8;

// Whether the row's `index`th control is a box: Loop, Deblock, Unlit,
// Texture, the panel's or Mute.
inline bool is_box(int index) {
  return index == 2 || index == kDeblock || index == kUnlit ||
         index == kTexture || index == kPanel || index == kMute;
}

// The size of the row's `index`th control, in points, as the last frame laid
// out `panel`: Play/Pause, Step, Loop's box, Normals, then Deblock's,
// Unlit's, Texture's and the panel's boxes. Each is a frame tall; the buttons
// are kButtonEms font sizes wide, and the boxes are square. A live player's row
// is Normals, Deblock, Unlit and Texture, laid out as Play/Pause, Deblock,
// Unlit and Texture are (live_top_left). The panel's context is current. The
// font size is the panel's as of that frame: between frames ImGui's own is the
// unscaled one.
inline glm::vec2 control_size(const ImGuiWindow& panel, int index) {
  const float font = panel.FontRefSize;
  const float height = font + 2.0f * ImGui::GetStyle().FramePadding.y;
  const float button = volumetric_kit::player::kButtonEms * font;
  return {is_box(index) ? height : button, height};
}

// The width the row gives its `index`th control: its size's, and for a box
// its label's after it.
inline float control_width(const ImGuiWindow& panel, int index) {
  if (!is_box(index)) {
    return control_size(panel, index).x;
  }
  const char* label = index == 2          ? "Loop"
                      : index == kDeblock ? "Deblock"
                      : index == kUnlit   ? "Unlit"
                      : index == kTexture ? "Texture"
                      : index == kMute    ? "Mute"
                                          : kPanelLabel;
  return control_size(panel, index).x + ImGui::GetStyle().ItemInnerSpacing.x +
         text_width(panel, label);
}

// The top left, in points, of each of the controls of `indices`, laid out as
// draw_controls lays them: the first `fixed` in a row, then each box after
// its line's last control when the panel, so widened, fits the widest ImGui
// lets an auto-sized panel grow (the work area less its safe-area padding on
// each side), else at the left of a line below.
inline std::vector<glm::vec2> lay_out(const ImGuiWindow& panel,
                                      const std::vector<int>& indices,
                                      std::size_t fixed) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float left = panel.Pos.x + style.WindowPadding.x;
  const float widest = ImGui::GetMainViewport()->WorkSize.x -
                       2.0f * style.DisplaySafeAreaPadding.x;
  const float line = control_size(panel, 0).y + style.ItemSpacing.y;
  std::vector<glm::vec2> places;
  glm::vec2 at{left, panel.Pos.y + style.WindowPadding.y};
  for (std::size_t i = 0; i < indices.size(); ++i) {
    const float width = control_width(panel, indices[i]);
    if (i >= fixed &&
        at.x - panel.Pos.x + width + style.WindowPadding.x > widest) {
      at = {left, at.y + line};
    }
    places.push_back(at);
    at.x += width + style.ItemSpacing.x;
  }
  return places;
}

// Its top left, in points. A control's place depends only on those before
// it, so Texture's and the panel's, laid out last, leave the others' as they
// are without them; the panel's is after Texture's, given with it here.
inline glm::vec2 control_top_left(const ImGuiWindow& panel, int index) {
  return lay_out(panel, {0, 1, 2, 3, kDeblock, kUnlit, kTexture, kPanel},
                 4)[index];
}

// Mute's top left, in points, for a file with audio and neither Texture nor
// the panel's box: after Unlit.
inline glm::vec2 mute_top_left(const ImGuiWindow& panel) {
  return lay_out(panel, {0, 1, 2, 3, kDeblock, kUnlit, kMute}, 4).back();
}

// Whether Deblock is on the row, after Normals; else it is below the row.
inline bool deblock_on_row(const ImGuiWindow& panel) {
  return control_top_left(panel, kDeblock).y == control_top_left(panel, 0).y;
}

// A live player's Deblock's (`index` kDeblock), Unlit's (kUnlit) or
// Texture's (kTexture) top left, in points: after Normals, which is laid out
// as Play/Pause is.
inline glm::vec2 live_top_left(const ImGuiWindow& panel, int index) {
  return lay_out(panel, {3, kDeblock, kUnlit, kTexture},
                 1)[static_cast<std::size_t>(index - kDeblock + 1)];
}
inline glm::vec2 live_deblock_top_left(const ImGuiWindow& panel) {
  return live_top_left(panel, kDeblock);
}

// Its middle, in points.
inline glm::vec2 control_middle(const ImGuiWindow& panel, int index) {
  return control_top_left(panel, index) + 0.5f * control_size(panel, index);
}

// A live player's Deblock's, Unlit's or Texture's middle, in points.
inline glm::vec2 live_middle(const ImGuiWindow& panel, int index) {
  return live_top_left(panel, index) + 0.5f * control_size(panel, index);
}
inline glm::vec2 live_deblock_middle(const ImGuiWindow& panel) {
  return live_middle(panel, kDeblock);
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
