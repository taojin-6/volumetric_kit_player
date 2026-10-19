// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/controls.hpp"

#include <cstdio>

#include "imgui.h"

namespace volumetric_kit::player {

void draw_controls(StreamPlayer& player,
                   const std::optional<FrameStamp>& shown) {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(viewport->WorkPos.x + 0.5f * viewport->WorkSize.x,
             viewport->WorkPos.y + viewport->WorkSize.y),
      ImGuiCond_Always, ImVec2(0.5f, 1.0f));
  constexpr ImGuiWindowFlags kFlags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoFocusOnAppearing;
  if (ImGui::Begin(kControlsWindow, nullptr, kFlags)) {
    const ImVec2 button(kButtonFrames * ImGui::GetFrameHeight(), 0.0f);
    // One ID for both labels, so a press that flips the label completes.
    if (ImGui::Button(player.playing() ? "Pause###play" : "Play###play",
                      button)) {
      if (player.playing()) {
        player.pause();
      } else {
        player.play();
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Step", button)) {
      player.step();
    }
    ImGui::SameLine();
    bool loop = player.loop();
    if (ImGui::Checkbox("Loop", &loop)) {
      player.set_loop(loop);
    }
    ImGui::TextUnformatted(
        status_line(shown, player.frame_count(), player.playing(), loop)
            .c_str());
  }
  ImGui::End();
}

std::string status_line(const std::optional<FrameStamp>& shown,
                        std::uint64_t frame_count, bool playing, bool loop) {
  if (!shown) {
    return "loading";
  }
  char ms[32];
  std::snprintf(ms, sizeof ms, "%.1f ms", shown->decode_mesh_ms);
  std::string line = "frame " + std::to_string(shown->index + 1) + "/" +
                     std::to_string(frame_count) + " · " +
                     (playing ? "playing" : "paused");
  if (loop) {
    line += " · loop";
  }
  return line + " · " + ms;
}

}  // namespace volumetric_kit::player
