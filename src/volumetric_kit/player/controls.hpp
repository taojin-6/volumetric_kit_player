// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file controls.hpp
/// @brief The playback controls and the stats panel, drawn on the view's
///        ImGui context. Player-internal.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "volumetric_kit/core/base/stage_metrics.hpp"
#include "volumetric_kit/player/playback.hpp"
#include "volumetric_kit/player/playback_stats.hpp"
#include "volumetric_kit/player/stream_player.hpp"

namespace volumetric_kit::player {

/// The controls' ImGui window.
inline constexpr const char* kControlsWindow = "##player_controls";
/// The stats panel's header window; its body's is derived from it
/// (gfx::ui::begin_collapsible_panel).
inline constexpr const char* kStatsWindow = "##player_stats";
/// The width of the Play/Pause and Step buttons, in font sizes, so each is a
/// stable target: "Pause" fits at any scale, and a phone's row stays inside
/// 375 pt.
inline constexpr float kButtonEms = 4.5f;

/// @brief Draw Play/Pause, Step and Loop in a row, the status line below, at
///        the bottom centre of the main viewport's work area, so they stay
///        clear of a notch and a home indicator. There is no text field, so
///        touch needs no keyboard.
///
/// @code
/// overlay.new_frame();
/// draw_controls(player, shown);
/// @endcode
/// @param player  The controls act on it; they are safe from this thread.
/// @param shown   The last stream frame drawn, for the status line.
/// @pre An ImGui frame is open on the view's context.
void draw_controls(StreamPlayer& player,
                   const std::optional<FrameStamp>& shown);

/// @brief The status line under the controls.
///
/// @code
/// // "frame 12/150 · playing · loop"
/// const std::string line = status_line(stamp, 150, true, true);
/// @endcode
/// @param shown        The last stream frame drawn; empty before the first.
/// @param frame_count  The track's frames.
/// @param playing      Whether it plays, rather than being paused.
/// @param loop         Whether it loops.
/// @return The frame drawn, counted from 1, over the track's; `playing` or
///         `paused`; and `loop` when on: "frame 12/150 · playing · loop".
///         "loading" before the first.
std::string status_line(const std::optional<FrameStamp>& shown,
                        std::uint64_t frame_count, bool playing, bool loop);

/// @brief The stats panel's lines, in Mbit/s (10^6 bit/s) and kB (1000 B).
///
/// @code
/// // {"bitrate 11.8 Mbit/s", "frame P · 41.2 kB · 182 k tris",
/// //  "rate 29.8 / 30 fps"}
/// const auto lines = stats_lines(stats, shown, triangles, now);
/// @endcode
/// @param stats      The figures.
/// @param shown      The newest frame drawn; empty before the first.
/// @param triangles  Its mesh's triangles.
/// @param now        The time, for the rate.
/// @return The track's mean bitrate (`-` when unknown); the frame's type (I
///         for a key record, else P), payload and triangles (each `-` before
///         the first); and the rate frames are taken at, over the nominal
///         when it is known.
std::vector<std::string> stats_lines(const PlaybackStats& stats,
                                     const std::optional<FrameStamp>& shown,
                                     std::uint32_t triangles,
                                     Playback::Clock::time_point now);

/// @brief The "Stats" panel, at the top left of the main viewport's work
///        area: a header (gfx::ui::begin_collapsible_panel) and below it, while
///        open, the @ref stats_lines and a stage table
///        (gfx::ui::draw_stage_table) on a 60 % background. Only the header
///        takes input: a finger or the mouse on the body reaches the camera.
///
/// It opens at launch, and a tap or click on the header closes and opens it.
/// While it is open its figures are rebuilt every @ref kRefresh, so the
/// digits stay readable; while it is closed nothing is rebuilt, but the
/// @ref PlaybackStats it reads keeps counting, so it reopens with current
/// figures. Not thread-safe: the drawing thread owns it.
///
/// @code
/// overlay.new_frame();
/// panel.draw(stats, shown, triangles, profiler.metrics().sections,
///            PlaybackStats::Clock::now());
/// @endcode
class StatsPanel {
 public:
  /// How often the figures are rebuilt while it is open.
  static constexpr std::chrono::milliseconds kRefresh{250};

  /// @brief Draw it; a tap or click on the header closes or opens it. Open
  ///        after the header, it first rebuilds the figures if @ref kRefresh
  ///        has passed since their last rebuild.
  /// @param stats      The figures.
  /// @param shown      The newest frame drawn, for @ref stats_lines.
  /// @param triangles  Its mesh's triangles.
  /// @param render     The rows after the stage means: the renderer's
  ///                   (gfx::Profiler::metrics).
  /// @param now        The time, for the rate and the rebuild.
  /// @pre An ImGui frame is open on the view's context.
  void draw(const PlaybackStats& stats, const std::optional<FrameStamp>& shown,
            std::uint32_t triangles, const std::vector<core::StageRow>& render,
            Playback::Clock::time_point now);

  /// @return Whether it is open.
  bool open() const noexcept { return open_; }
  /// @return Its lines, as of their last rebuild.
  const std::vector<std::string>& lines() const noexcept { return lines_; }
  /// @return Its table's rows, as of their last rebuild: the stage means
  ///         (PlaybackStats::stage_means), then the renderer's.
  const std::vector<core::StageRow>& rows() const noexcept { return rows_; }

 private:
  bool open_ = true;
  std::vector<std::string> lines_;
  std::vector<core::StageRow> rows_;
  std::optional<Playback::Clock::time_point> rebuilt_;
};

}  // namespace volumetric_kit::player
