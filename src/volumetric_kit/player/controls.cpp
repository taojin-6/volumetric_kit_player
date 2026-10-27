// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/controls.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
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

// A column of the compact stats: a child window sized to what it holds, as a
// table in it would otherwise take the body's whole width, taking no input,
// as the body takes none.
void begin_column(const char* id) {
  ImGui::BeginChild(id, ImVec2(0.0f, 0.0f),
                    ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY,
                    ImGuiWindowFlags_NoInputs);
}

// A table of the compact stats under a short line stating `window`, as
// gfx::ui::draw_stage_table's own, which a phone's width cannot hold, does:
// "per stream frame · 30/30 timed".
void draw_compact_table(const char* id, const std::vector<core::StageRow>& rows,
                        const gfx::ui::StageWindow& window) {
  const std::string line = std::string("per ") + window.unit + " · " +
                           std::to_string(window.timed) + "/" +
                           std::to_string(window.frames) + " timed";
  ImGui::TextDisabled("%s", line.c_str());
  gfx::ui::draw_stage_table(id, rows);
}

// `count` with its thousands grouped: 131,072.
std::string grouped(std::uint32_t count) {
  std::string text = std::to_string(count);
  for (std::size_t at = text.size(); at > 3; at -= 3) {
    text.insert(at - 3, ",");
  }
  return text;
}

// What joins a stats line's parts.
constexpr std::string_view kSeparator = " · ";

// A stats or status line of parts joined by kSeparator: whole where it fits
// `width`; else each row as many whole parts as fit, at least one, the rows
// after the first indented. Where all but its last part fit, that is those
// and, below them, the last.
void draw_parted_line(std::string_view line, float width) {
  bool indented = false;
  do {
    const float room =
        indented ? width - ImGui::GetStyle().IndentSpacing : width;
    std::size_t end = std::min(line.find(kSeparator), line.size());
    while (end < line.size()) {
      const std::size_t next =
          std::min(line.find(kSeparator, end + kSeparator.size()), line.size());
      if (ImGui::CalcTextSize(line.data(), line.data() + next).x > room) {
        break;
      }
      end = next;
    }
    ImGui::TextUnformatted(line.data(), line.data() + end);
    line.remove_prefix(std::min(line.size(), end + kSeparator.size()));
    if (!line.empty() && !indented) {
      ImGui::Indent();
      indented = true;
    }
  } while (!line.empty());
  if (indented) {
    ImGui::Unindent();
  }
}

// The widest text a window holds inside the main viewport's work area.
float text_room() {
  const ImGuiStyle& style = ImGui::GetStyle();
  return ImGui::GetMainViewport()->WorkSize.x -
         2.0f * (style.DisplaySafeAreaPadding.x + style.WindowPadding.x);
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

// A box labelled `label` after the line's last control, or below it when the
// line would not fit the work area with it; whether a press flipped `*on`.
bool draw_box(const char* label, bool* on) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float box = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x +
                    ImGui::CalcTextSize(label).x;
  // The window's width with it on the line, against the widest ImGui lets an
  // auto-sized window grow.
  const float row = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x +
                    style.ItemSpacing.x + box + style.WindowPadding.x;
  if (row <= ImGui::GetMainViewport()->WorkSize.x -
                 2.0f * style.DisplaySafeAreaPadding.x) {
    ImGui::SameLine();
  }
  return ImGui::Checkbox(label, on);
}

// Deblock, Unlit and, given, Texture, each a box after the line's last
// control, or below it when the line would not fit the work area with it.
void draw_boxes(StreamPlayer& player, bool& unlit, bool* textured) {
  bool on = player.deblock();
  if (draw_box("Deblock", &on)) {
    player.set_deblock(on);
  }
  draw_box("Unlit", &unlit);
  if (textured != nullptr) {
    draw_box("Texture", textured);
  }
}

// A file's controls: Play/Pause, Step, Loop, Normals, Deblock, Unlit and,
// given, Texture; returns its status line.
std::string draw_file_controls(StreamPlayer& player,
                               const std::optional<FrameStamp>& shown,
                               bool& normals, bool& unlit, bool* textured) {
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
  draw_boxes(player, unlit, textured);
  return status_line(shown, player.frame_count(), player.playing(), loop);
}

}  // namespace

void draw_controls(StreamPlayer& player, const std::optional<FrameStamp>& shown,
                   bool& normals, bool& unlit, bool* textured) {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(viewport->WorkPos.x + 0.5f * viewport->WorkSize.x,
             viewport->WorkPos.y + viewport->WorkSize.y),
      ImGuiCond_Always, ImVec2(0.5f, 1.0f));
  if (ImGui::Begin(kControlsWindow, nullptr, kFlags)) {
    // A file's or a live player's status line, drawn by the one call below.
    std::string status;
    if (player.live()) {
      draw_normals(normals);
      draw_boxes(player, unlit, textured);
      status = live_status_line(player.live_status(), Playback::Clock::now());
    } else {
      status = draw_file_controls(player, shown, normals, unlit, textured);
    }
    draw_parted_line(status, text_room());
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
  if (status.set != recon::stream::kNoSet) {
    line += " · set " + std::to_string(status.set);
  }
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
                     std::to_string(frame_count);
  if (shown->set != recon::stream::kNoSet) {
    line += " · set " + std::to_string(shown->set);
  }
  line += playing ? " · playing" : " · paused";
  if (loop) {
    line += " · loop";
  }
  return line;
}

std::vector<std::string> stats_lines(const PlaybackStats& stats,
                                     const std::optional<FrameStamp>& shown,
                                     std::uint32_t triangles,
                                     Playback::Clock::time_point now, bool live,
                                     bool compact) {
  const std::optional<double> mbit =
      live ? stats.rolling_mbit_per_s(now) : stats.mean_mbit_per_s();
  std::string frame = "frame - · - kB · - k tris";
  std::string deblock = "deblock - · T - mm";
  if (shown) {
    frame = std::string("frame ") + (shown->key ? "I" : "P") + " · " +
            fixed(static_cast<double>(shown->bytes) / 1e3, 1) + " kB · " +
            fixed(triangles / 1e3, 0) + " k tris";
    const recon::codec::DeblockInfo& info = shown->deblock;
    const std::string_view filter = shown->deblock_on ? info.filter : "off";
    deblock = "deblock " + std::string(filter) + " · T " +
              fixed(static_cast<double>(info.step) * 1e3, 3) + " mm";
    if (filter != "off") {
      deblock += " · " + grouped(info.exact_blocks) + " of " +
                 grouped(info.block_count) +
                 (info.block_count == 1 ? " block" : " blocks") + " exact";
    }
  }
  // The device's figures: n/t while no frame of the window was timed; the
  // GPU and the queue latency n/a where timed frames have none.
  const FrameFigures means = stats.frame_means();
  const auto figure = [](const std::optional<double>& ms, int decimals,
                         const char* unknown) {
    return ms ? fixed(*ms, decimals) + " ms" : std::string(unknown);
  };
  const char* const untimed = stats.timed_frames() > 0 ? "n/a" : "n/t";
  const std::string gpu = "GPU " + figure(means.gpu_ms, 2, untimed);
  const std::string idle = "idle " + figure(means.device_idle_ms, 1, "n/t");
  const std::string queue = "queue " + figure(means.queue_ms, 2, untimed);
  // The latency from due to the present call, over the frames presented,
  // and its legs, each over the frames that have it.
  const auto leg = [](const std::optional<double>& ms) {
    return ms ? fixed(*ms, 1) : std::string("-");
  };
  const std::string latency = "latency " + figure(means.latency_ms, 1, "-");
  std::string rate = "rate " + fixed(stats.frames_per_s(now), 1);
  if (const std::optional<double> nominal = stats.nominal_fps()) {
    rate += " / " + rate_text(*nominal);
  }
  rate += " fps";
  if (live) {
    rate += " · " + fixed(stats.records_per_s(now), 0) + " rec/s";
  }
  std::vector<std::string> lines{"bitrate " + (mbit ? fixed(*mbit, 1) : "-") +
                                     " Mbit/s" + (live ? " · last 1 s" : ""),
                                 std::move(frame), std::move(deblock)};
  if (compact) {
    lines.push_back(gpu + " · " + latency);
  } else {
    lines.push_back(gpu + " · " + idle + " · " + queue);
    lines.push_back(latency + " · publish " + leg(means.publish_ms) +
                    " · take " + leg(means.take_ms) + " · present " +
                    leg(means.present_ms));
  }
  lines.push_back(std::move(rate));
  return lines;
}

std::string display_line(const gfx::FrameMetrics& render) {
  return "display " + (render.fps > 0.0 ? fixed(render.fps, 1) : "-") + " fps";
}

void StatsPanel::draw(const PlaybackStats& stats,
                      const std::optional<FrameStamp>& shown,
                      std::uint32_t triangles, const gfx::FrameMetrics& render,
                      Playback::Clock::time_point now, bool live) {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  if (!gfx::ui::begin_collapsible_panel(kStatsWindow, "Stats",
                                        viewport->WorkPos, &open_)) {
    return;
  }
  // After the header, so the frame a tap opens it on is current.
  if (!rebuilt_ || now - *rebuilt_ >= kRefresh) {
    lines_ = stats_lines(stats, shown, triangles, now, live, compact_);
    rows_ = stats.stage_means();
    window_ = {stats.window_frames(), stats.timed_frames(), "stream frame"};
    display_ = display_line(render);
    render_rows_ = render.sections;
    const bool timed =
        std::any_of(render_rows_.begin(), render_rows_.end(),
                    [](const core::StageRow& row) {
                      return row.device == core::DeviceTime::Measured;
                    });
    render_window_ = {render_rows_.empty() ? 0u : 1u, timed ? 1u : 0u,
                      "display frame"};
    rebuilt_ = now;
  }
  // The widest text the body's window holds inside the work area.
  const ImGuiStyle& style = ImGui::GetStyle();
  const float width = text_room();
  if (!compact_) {
    for (const std::string& line : lines_) {
      draw_parted_line(line, width);
    }
    gfx::ui::draw_stage_table("stages", rows_, window_);
    ImGui::TextUnformatted(display_.c_str());
    gfx::ui::draw_stage_table("render", render_rows_, render_window_);
    gfx::ui::end_collapsible_panel();
    return;
  }
  // Compact: the worker's table holds its stages, read, decode and mesh, and
  // the renderer's its rows but its loop's submit, and a work area wider than
  // tall has room for the renderer's beside the worker's, not below. The
  // device's line and the rate, the last two, sit over the display's rate,
  // so each column holds as many rows as before they came, and a line that
  // breaks leaves the worker's column no taller; beside, a line takes the
  // width the renderer's column left as of the last frame (DECISIONS.md).
  const auto kept = [](const std::vector<core::StageRow>& rows,
                       std::initializer_list<const char*> dropped) {
    std::vector<core::StageRow> kept_rows;
    for (const core::StageRow& row : rows) {
      const bool drop =
          std::strncmp(row.name, core::StageMetrics::kBreakdownPrefix,
                       std::strlen(core::StageMetrics::kBreakdownPrefix)) ==
              0 ||
          std::any_of(dropped.begin(), dropped.end(), [&](const char* name) {
            return std::strcmp(row.name, name) == 0;
          });
      if (!drop) {
        kept_rows.push_back(row);
      }
    }
    return kept_rows;
  };
  const std::vector<core::StageRow> stages =
      kept(rows_, {"submit", "wait: GPU", "finish"});
  // The draw's row by a short name: a phone's portrait has no room for it.
  std::vector<core::StageRow> drawn = kept(render_rows_, {"submit"});
  for (core::StageRow& row : drawn) {
    if (std::strcmp(row.name, kDrawRow) == 0) {
      row.name = "draw";
    }
  }
  const bool beside = viewport->WorkSize.x > viewport->WorkSize.y;
  const float worker_width =
      beside ? width - render_width_ - style.ItemSpacing.x : width;
  const std::size_t worker_lines = lines_.size() < 2 ? 0 : lines_.size() - 2;
  begin_column("##worker");
  for (std::size_t i = 0; i < worker_lines; ++i) {
    draw_parted_line(lines_[i], worker_width);
  }
  draw_compact_table("stages", stages, window_);
  ImGui::EndChild();
  if (beside) {
    ImGui::SameLine();
  }
  begin_column("##render");
  for (std::size_t i = worker_lines; i < lines_.size(); ++i) {
    draw_parted_line(lines_[i], width);
  }
  ImGui::TextUnformatted(display_.c_str());
  draw_compact_table("render", drawn, render_window_);
  ImGui::EndChild();
  render_width_ = ImGui::GetItemRectSize().x;
  gfx::ui::end_collapsible_panel();
}

}  // namespace volumetric_kit::player
