// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/program_panel.hpp"

#include <algorithm>

#include "imgui.h"

namespace volumetric_kit::player {

UiArea scene_area(const gfx::ui::PanelLayout& laid, bool open) {
  UiArea area = work_area();
  if (!open) {
    return area;
  }
  // The scene runs from the drawable's origin to the panel's inner edge.
  const ImVec2 s = ImGui::GetIO().DisplayFramebufferScale;
  const float frame = ImGui::GetFrameHeight();
  if (laid.side == gfx::ui::DockSide::Right) {
    const float edge = static_cast<float>(laid.scene.extent.width) / s.x;
    area.size.x = std::max(
        0.0f, std::min(area.pos.x + area.size.x, edge - frame) - area.pos.x);
  } else {
    const float edge = static_cast<float>(laid.scene.extent.height) / s.y;
    area.size.y = std::max(
        0.0f, std::min(area.pos.y + area.size.y, edge - frame) - area.pos.y);
  }
  return area;
}

}  // namespace volumetric_kit::player
