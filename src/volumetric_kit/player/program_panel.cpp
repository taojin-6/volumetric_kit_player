// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/program_panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include "imgui.h"

namespace volumetric_kit::player {

// TODO: a docked panel's layout and chrome get one home in gfx_ui with
// ui::ImagePanel's, which adds its own content to the same rules (a gfx
// task); this is a knowing copy of their generic part until then
// (DECISIONS.md).

namespace {

constexpr float kBackgroundAlpha = 0.6f;

// A rectangle of whole pixels from [x0, x1) x [y0, y1), rounded; empty where
// it is inverted.
VkRect2D pixel_rect(float x0, float y0, float x1, float y1) {
  const float left = std::round(x0);
  const float top = std::round(y0);
  const float right = std::max(std::round(x1), left);
  const float bottom = std::max(std::round(y1), top);
  VkRect2D rect{};
  rect.offset = {static_cast<std::int32_t>(left),
                 static_cast<std::int32_t>(top)};
  rect.extent = {static_cast<std::uint32_t>(right - left),
                 static_cast<std::uint32_t>(bottom - top)};
  return rect;
}

// The panel's size across an axis `axis` pixels long, at `share`, in whole
// pixels: the share held to [kMinShare, kMaxShare], then at least `least`,
// then at most kMaxShare of the axis.
float across(float share, float axis, float least) {
  const float held =
      std::clamp(share, ProgramPanel::kMinShare, ProgramPanel::kMaxShare) *
      axis;
  return std::round(
      std::min(std::max(held, least), ProgramPanel::kMaxShare * axis));
}

}  // namespace

ProgramPanel::ProgramPanel(const PanelState& state, std::vector<float> detents)
    : detents_(std::move(detents)), state_(state) {}

float& ProgramPanel::share(DockSide side) noexcept {
  return side == DockSide::kRight ? state_.right_share : state_.bottom_share;
}

const PanelFrame& ProgramPanel::layout(VkExtent2D drawable) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 s = ImGui::GetIO().DisplayFramebufferScale;
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  const auto width = static_cast<float>(drawable.width);
  const auto height = static_cast<float>(drawable.height);
  safe_[0] = std::clamp(std::round(viewport->WorkPos.x * s.x), 0.0f, width);
  safe_[1] = std::clamp(std::round(viewport->WorkPos.y * s.y), 0.0f, height);
  safe_[2] =
      std::clamp(std::round((viewport->WorkPos.x + viewport->WorkSize.x) * s.x),
                 safe_[0], width);
  safe_[3] =
      std::clamp(std::round((viewport->WorkPos.y + viewport->WorkSize.y) * s.y),
                 safe_[1], height);
  header_ =
      std::round((ImGui::GetFrameHeight() + 2.0f * style.FramePadding.y) * s.y);
  frame_.drawable = drawable;
  frame_.side = width >= height ? DockSide::kRight : DockSide::kBottom;
  scene_area_ = work_area();
  laid_out_open_ = state_.open;
  if (!state_.open) {
    frame_.content = {};
    scene_ = {{0, 0}, drawable};
    return frame_;
  }
  // The scene up to the panel's inner edge; the content past the handle's
  // bar and below the header, in the safe area. The controls stop a frame
  // height short of the scene's edge, which the handle may reach over.
  const float frame = ImGui::GetFrameHeight();
  if (frame_.side == DockSide::kRight) {
    edge_ = safe_[2] -
            across(share(frame_.side), safe_[2] - safe_[0], kMinPoints * s.x);
    scene_ = pixel_rect(0.0f, 0.0f, edge_, height);
    frame_.content = pixel_rect(edge_ + std::round(kBarPoints * s.x),
                                safe_[1] + header_, safe_[2], safe_[3]);
    scene_area_.size.x = std::max(
        0.0f,
        std::min(scene_area_.pos.x + scene_area_.size.x, edge_ / s.x - frame) -
            scene_area_.pos.x);
  } else {
    edge_ = safe_[3] -
            across(share(frame_.side), safe_[3] - safe_[1], kMinPoints * s.y);
    scene_ = pixel_rect(0.0f, 0.0f, width, edge_);
    frame_.content =
        pixel_rect(safe_[0], edge_ + std::round(kBarPoints * s.y) + header_,
                   safe_[2], safe_[3]);
    scene_area_.size.y = std::max(
        0.0f,
        std::min(scene_area_.pos.y + scene_area_.size.y, edge_ / s.y - frame) -
            scene_area_.pos.y);
  }
  return frame_;
}

void ProgramPanel::draw_chrome(const char* title,
                               const std::function<void()>& items) {
  if (!state_.open || !laid_out_open_) {
    return;
  }
  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 s = ImGui::GetIO().DisplayFramebufferScale;
  const bool right = frame_.side == DockSide::kRight;
  constexpr ImGuiWindowFlags kFlags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse;

  // The handle: a window a frame height across the bar, along the panel's
  // safe length. Into the panel it reaches no further than the header's
  // padding, so it never covers the header's items; the rest of it lies
  // over the scene.
  const float bar =
      right ? std::round(kBarPoints * s.x) : std::round(kBarPoints * s.y);
  const float hit = std::max(ImGui::GetFrameHeight(), kBarPoints);
  const float padding = right ? style.WindowPadding.x : style.FramePadding.y;
  const float inward = std::min(0.5f * (hit - kBarPoints), padding);
  const float start = (edge_ + bar) / (right ? s.x : s.y) + inward - hit;
  const ImVec2 handle_pos =
      right ? ImVec2(start, safe_[1] / s.y) : ImVec2(safe_[0] / s.x, start);
  const ImVec2 handle_size = right ? ImVec2(hit, (safe_[3] - safe_[1]) / s.y)
                                   : ImVec2((safe_[2] - safe_[0]) / s.x, hit);
  ImGui::SetNextWindowPos(handle_pos);
  ImGui::SetNextWindowSize(handle_size);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
  const bool handle_drawn = ImGui::Begin(
      kPanelHandleWindow, nullptr, kFlags | ImGuiWindowFlags_NoBackground);
  ImGui::PopStyleVar(3);
  if (handle_drawn) {
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    ImGui::InvisibleButton("##drag", handle_size);
    float& current = share(frame_.side);
    if (ImGui::IsItemActivated()) {
      drag_from_ = current;
      drag_share_ = current;
    }
    const float length = right ? safe_[2] - safe_[0] : safe_[3] - safe_[1];
    if (ImGui::IsItemActive() && length > 0.0f) {
      // The share the press found, moved by the pointer since, of the safe
      // length along the dock axis: the edge keeps its place under the
      // pointer.
      const ImVec2 travel =
          ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
      drag_share_ =
          drag_from_ - (right ? travel.x * s.x : travel.y * s.y) / length;
      current = std::clamp(drag_share_, kMinShare, kMaxShare);
    }
    if (ImGui::IsItemDeactivated()) {
      // Zero within ImGui's drag threshold: a click or a tap, which changes
      // nothing.
      const ImVec2 dragged = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
      if (dragged.x == 0.0f && dragged.y == 0.0f) {
        current = drag_from_;
      } else if (drag_share_ < kMinShare) {
        state_.open = false;
        current = drag_from_;
      } else if (!detents_.empty()) {
        current = *std::min_element(
            detents_.begin(), detents_.end(), [&](float a, float b) {
              return std::abs(a - drag_share_) < std::abs(b - drag_share_);
            });
      }
    }
    const ImGuiCol colour = ImGui::IsItemActive()    ? ImGuiCol_SeparatorActive
                            : ImGui::IsItemHovered() ? ImGuiCol_SeparatorHovered
                                                     : ImGuiCol_Separator;
    const ImVec2 bar_min = right ? ImVec2(edge_ / s.x, handle_pos.y)
                                 : ImVec2(handle_pos.x, edge_ / s.y);
    const ImVec2 bar_max =
        right ? ImVec2((edge_ + bar) / s.x, handle_pos.y + handle_size.y)
              : ImVec2(handle_pos.x + handle_size.x, (edge_ + bar) / s.y);
    ImGui::GetWindowDrawList()->AddRectFilled(bar_min, bar_max,
                                              ImGui::GetColorU32(colour));
  }
  ImGui::End();

  // The header, across the panel's safe top: the title, the program's
  // items, and a close button at the right.
  const ImVec2 header_pos = right ? ImVec2((edge_ + bar) / s.x, safe_[1] / s.y)
                                  : ImVec2(safe_[0] / s.x, (edge_ + bar) / s.y);
  const float header_width =
      right ? (safe_[2] - edge_ - bar) / s.x : (safe_[2] - safe_[0]) / s.x;
  ImGui::SetNextWindowPos(header_pos);
  ImGui::SetNextWindowSize(ImVec2(header_width, header_ / s.y));
  ImGui::SetNextWindowBgAlpha(kBackgroundAlpha);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                      ImVec2(style.WindowPadding.x, style.FramePadding.y));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
  // Kept behind the handle: a press on the header never raises it over the
  // handle's reach.
  const bool header_drawn =
      ImGui::Begin(kPanelHeaderWindow, nullptr,
                   kFlags | ImGuiWindowFlags_NoScrollbar |
                       ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::PopStyleVar(2);
  if (header_drawn) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(title);
    if (items) {
      ImGui::SameLine();
      items();
    }
    const float frame = ImGui::GetFrameHeight();
    ImGui::SameLine();
    const float room = ImGui::GetContentRegionAvail().x - frame;
    if (room > 0.0f) {
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room);
    }
    if (ImGui::Button("##close", ImVec2(frame, frame))) {
      state_.open = false;
    }
    // A cross, which the default font has no glyph for.
    const ImVec2 lo = ImGui::GetItemRectMin();
    const ImVec2 hi = ImGui::GetItemRectMax();
    const float inset = 0.3f * frame;
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
    list->AddLine(ImVec2(lo.x + inset, lo.y + inset),
                  ImVec2(hi.x - inset, hi.y - inset), ink, 1.5f);
    list->AddLine(ImVec2(lo.x + inset, hi.y - inset),
                  ImVec2(hi.x - inset, lo.y + inset), ink, 1.5f);
  }
  ImGui::End();
}

bool ProgramPanel::over_content(glm::vec2 point) const noexcept {
  if (!state_.open || !laid_out_open_) {
    return false;
  }
  const VkRect2D& r = frame_.content;
  const auto x0 = static_cast<float>(r.offset.x);
  const auto y0 = static_cast<float>(r.offset.y);
  return point.x >= x0 && point.y >= y0 &&
         point.x < x0 + static_cast<float>(r.extent.width) &&
         point.y < y0 + static_cast<float>(r.extent.height);
}

}  // namespace volumetric_kit::player
