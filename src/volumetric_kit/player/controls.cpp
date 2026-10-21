// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/controls.hpp"

#include <cstdio>
#include <utility>

#include "imgui.h"
#include "volumetric_kit/gfx/ui/collapsible_panel.hpp"
#include "volumetric_kit/gfx/ui/metrics_panel.hpp"

namespace volumetric_kit::player {
namespace {

// `value` to `decimals` places.
std::string fixed(double value, int decimals) {
  char text[32];
  std::snprintf(text, sizeof text, "%.*f", decimals, value);
  return text;
}

// A nominal rate: whole as 30, fractional to 2 places as 29.97.
std::string rate_text(double fps) {
  std::string text = fixed(fps, 2);
  text.erase(text.find_last_not_of('0') + 1);
  if (text.back() == '.') {
    text.pop_back();
  }
  return text;
}

}  // namespace

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
    const ImVec2 button(kButtonEms * ImGui::GetFontSize(), 0.0f);
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
  std::string line = "frame " + std::to_string(shown->index + 1) + "/" +
                     std::to_string(frame_count) + " · " +
                     (playing ? "playing" : "paused");
  if (loop) {
    line += " · loop";
  }
  return line;
}

std::vector<std::string> stats_lines(const PlaybackStats& stats,
                                     const std::optional<FrameStamp>& shown,
                                     std::uint32_t triangles,
                                     Playback::Clock::time_point now) {
  const std::optional<double> mbit = stats.mean_mbit_per_s();
  std::string frame = "frame - · - kB · - k tris";
  if (shown) {
    frame = std::string("frame ") + (shown->key ? "I" : "P") + " · " +
            fixed(static_cast<double>(shown->bytes) / 1e3, 1) + " kB · " +
            fixed(triangles / 1e3, 0) + " k tris";
  }
  std::string rate = "rate " + fixed(stats.frames_per_s(now), 1);
  if (const std::optional<double> nominal = stats.nominal_fps()) {
    rate += " / " + rate_text(*nominal);
  }
  return {"bitrate " + (mbit ? fixed(*mbit, 1) : "-") + " Mbit/s",
          std::move(frame), rate + " fps"};
}

void StatsPanel::draw(const PlaybackStats& stats,
                      const std::optional<FrameStamp>& shown,
                      std::uint32_t triangles,
                      const std::vector<core::StageRow>& render,
                      Playback::Clock::time_point now) {
  if (gfx::ui::begin_collapsible_panel(
          kStatsWindow, "Stats", ImGui::GetMainViewport()->WorkPos, &open_)) {
    // After the header, so the frame a tap opens it on is current.
    if (!rebuilt_ || now - *rebuilt_ >= kRefresh) {
      lines_ = stats_lines(stats, shown, triangles, now);
      rows_ = stats.stage_means();
      rows_.insert(rows_.end(), render.begin(), render.end());
      rebuilt_ = now;
    }
    for (const std::string& line : lines_) {
      ImGui::TextUnformatted(line.c_str());
    }
    gfx::ui::draw_stage_table("stages", rows_);
    gfx::ui::end_collapsible_panel();
  }
}

}  // namespace volumetric_kit::player
