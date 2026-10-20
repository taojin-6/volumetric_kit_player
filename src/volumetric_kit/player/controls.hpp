// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file controls.hpp
/// @brief The playback controls, drawn in raw ImGui on the view's context.
///        Player-internal.

#include <cstdint>
#include <optional>
#include <string>

#include "volumetric_kit/player/stream_player.hpp"

namespace volumetric_kit::player {

/// The controls' ImGui window.
inline constexpr const char* kControlsWindow = "##player_controls";
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
/// // "frame 12/150 · playing · loop · 4.1 ms"
/// const std::string line = status_line(stamp, 150, true, true);
/// @endcode
/// @param shown        The last stream frame drawn; empty before the first.
/// @param frame_count  The track's frames.
/// @param playing      Whether it plays, rather than being paused.
/// @param loop         Whether it loops.
/// @return The frame drawn, counted from 1, over the track's; `playing` or
///         `paused`; `loop` when on; and the frame's decode and mesh time:
///         "frame 12/150 · playing · loop · 4.1 ms". "loading" before the
///         first.
std::string status_line(const std::optional<FrameStamp>& shown,
                        std::uint64_t frame_count, bool playing, bool loop);

}  // namespace volumetric_kit::player
