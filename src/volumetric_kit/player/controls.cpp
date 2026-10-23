// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/controls.hpp"

#include <cstdio>
#include <string_view>
#include <utility>

#include "imgui.h"
#include "volumetric_kit/gfx/ui/collapsible_panel.hpp"
#include "volumetric_kit/gfx/ui/metrics_panel.hpp"

namespace volumetric_kit::player {
namespace {

// The controls' and Source's windows: sized to what they hold, where they are
// put, and never taking focus.
constexpr ImGuiWindowFlags kFlags =
    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
    ImGuiWindowFlags_NoFocusOnAppearing;

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

// A button's size: kButtonEms font sizes wide, a frame tall.
ImVec2 button_size() { return {kButtonEms * ImGui::GetFontSize(), 0.0f}; }

// Normals, a button drawn pressed while on, hovered or not, as a mouse stays
// on it after a click (DECISIONS.md).
void draw_normals(bool& normals) {
  const bool on = normals;
  if (on) {
    const ImVec4 pressed = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
    ImGui::PushStyleColor(ImGuiCol_Button, pressed);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pressed);
  }
  if (ImGui::Button("Normals", button_size())) {
    normals = !normals;
  }
  if (on) {
    ImGui::PopStyleColor(2);
  }
}

// Deblock, a box, after the row's last control, or below the row when the
// row would not fit the work area with it.
void draw_deblock(StreamPlayer& player) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float box = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x +
                    ImGui::CalcTextSize("Deblock").x;
  // The window's width with it on the row, against the widest ImGui lets an
  // auto-sized window grow.
  const float row = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x +
                    style.ItemSpacing.x + box + style.WindowPadding.x;
  if (row <= ImGui::GetMainViewport()->WorkSize.x -
                 2.0f * style.DisplaySafeAreaPadding.x) {
    ImGui::SameLine();
  }
  bool on = player.deblock();
  if (ImGui::Checkbox("Deblock", &on)) {
    player.set_deblock(on);
  }
}

// A file's controls: Play/Pause, Step, Loop, Normals and Deblock, then the
// status line.
void draw_file_controls(StreamPlayer& player,
                        const std::optional<FrameStamp>& shown, bool& normals) {
  const ImVec2 button = button_size();
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
  ImGui::SameLine();
  draw_normals(normals);
  draw_deblock(player);
  ImGui::TextUnformatted(
      status_line(shown, player.frame_count(), player.playing(), loop).c_str());
}

}  // namespace

void draw_controls(StreamPlayer& player, const std::optional<FrameStamp>& shown,
                   bool& normals) {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(viewport->WorkPos.x + 0.5f * viewport->WorkSize.x,
             viewport->WorkPos.y + viewport->WorkSize.y),
      ImGuiCond_Always, ImVec2(0.5f, 1.0f));
  if (ImGui::Begin(kControlsWindow, nullptr, kFlags)) {
    if (player.live()) {
      draw_normals(normals);
      draw_deblock(player);
      ImGui::TextUnformatted(
          live_status_line(player.live_status(), Playback::Clock::now())
              .c_str());
    } else {
      draw_file_controls(player, shown, normals);
    }
  }
  ImGui::End();
}

bool draw_source() {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y),
      ImGuiCond_Always, ImVec2(1.0f, 0.0f));
  // No padding, so the button is all of the window, and no border, which
  // would clip a point off each side of the button's target.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  const bool drawn = ImGui::Begin(kSourceWindow, nullptr, kFlags);
  ImGui::PopStyleVar(2);
  const bool pressed = drawn && ImGui::Button("Source", button_size());
  ImGui::End();
  return pressed;
}

std::string live_status_line(const LiveStatus& status,
                             Playback::Clock::time_point now) {
  switch (status.state) {
    case LiveState::kConnecting:
      if (status.last_error.empty()) {
        return "connecting to " + status.address;
      }
      return "reconnecting to " + status.address + ": " + status.last_error;
    case LiveState::kConnected:
      return "connected · the sender has no I-frame yet";
    case LiveState::kLive:
      break;
  }
  std::string line = "live · seq " + std::to_string(status.seq);
  const auto quiet = std::chrono::duration_cast<std::chrono::seconds>(
      now - status.last_record);
  if (now - status.last_record > std::chrono::seconds(1)) {
    line += " · nothing for " + std::to_string(quiet.count()) + " s";
  }
  return line;
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
                                     Playback::Clock::time_point now,
                                     bool live) {
  const std::optional<double> mbit =
      live ? stats.rolling_mbit_per_s(now) : stats.mean_mbit_per_s();
  std::string frame = "frame - · - kB · - k tris";
  std::string deblock = "deblock - · T - mm";
  if (shown) {
    frame = std::string("frame ") + (shown->key ? "I" : "P") + " · " +
            fixed(static_cast<double>(shown->bytes) / 1e3, 1) + " kB · " +
            fixed(triangles / 1e3, 0) + " k tris";
    const char* filter = "off";
    if (shown->deblock_on) {
      filter = std::string_view(shown->deblock.filter) == "off"
                   ? "skipped"
                   : shown->deblock.filter;
    }
    deblock = std::string("deblock ") + filter + " · T " +
              fixed(static_cast<double>(shown->deblock.step) * 1e3, 3) + " mm";
  }
  std::string rate = "rate " + fixed(stats.frames_per_s(now), 1);
  if (const std::optional<double> nominal = stats.nominal_fps()) {
    rate += " / " + rate_text(*nominal);
  }
  rate += " fps";
  if (live) {
    rate += " · " + fixed(stats.records_per_s(now), 0) + " rec/s";
  }
  return {"bitrate " + (mbit ? fixed(*mbit, 1) : "-") + " Mbit/s" +
              (live ? " · last 1 s" : ""),
          std::move(frame), std::move(deblock), std::move(rate)};
}

void StatsPanel::draw(const PlaybackStats& stats,
                      const std::optional<FrameStamp>& shown,
                      std::uint32_t triangles,
                      const std::vector<core::StageRow>& render,
                      Playback::Clock::time_point now, bool live) {
  if (gfx::ui::begin_collapsible_panel(
          kStatsWindow, "Stats", ImGui::GetMainViewport()->WorkPos, &open_)) {
    // After the header, so the frame a tap opens it on is current.
    if (!rebuilt_ || now - *rebuilt_ >= kRefresh) {
      lines_ = stats_lines(stats, shown, triangles, now, live);
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
