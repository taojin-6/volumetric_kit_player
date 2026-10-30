// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file pointer.hpp
/// @brief Which of the scene and a program's panel a press drives, a
///        desktop's double click and its wheel's zoom: the shells' input
///        rules, on the host alone. Player-internal.

#include <optional>

#include <glm/vec2.hpp>

namespace volumetric_kit::player {

/// One wheel notch's zoom of a program's panel (PanelPointer::zoom).
inline constexpr float kWheelZoom = 1.25f;

/// @brief Which of the scene and a program's panel (PlayerView::over_panel)
///        a press drives: chosen where it starts and kept until every button
///        or finger is up, so a drag that crosses the panel's edge keeps its
///        target. With nothing held, the wheel goes where the pointer is.
///
/// @code
/// PointerTarget target;
/// // each tick, before the camera or the panel moves:
/// const bool to_panel =
///     target.on_panel(left || right, view->over_panel(cursor));
/// @endcode
class PointerTarget {
 public:
  /// @brief One tick of the pointer.
  /// @param held        Whether a button or a finger is down.
  /// @param over_panel  Whether the pointer is over the panel's content.
  /// @return Whether the tick's input goes to the panel: while held, whether
  ///         the press began over it; else @p over_panel.
  bool on_panel(bool held, bool over_panel) noexcept;

 private:
  bool held_ = false;
  bool began_on_panel_ = false;
};

/// @brief A desktop's double click: a press within @ref kSeconds and
///        @ref kPoints of the press before, which ImGui's defaults set.
///
/// @code
/// DoubleClick clicks;
/// if (pressed_now && clicks.press(cursor_points, glfwGetTime())) {
///   pointer.double_tap = true;
/// }
/// @endcode
class DoubleClick {
 public:
  static constexpr double kSeconds = 0.3;  ///< ImGui's MouseDoubleClickTime.
  static constexpr float kPoints = 6.0f;   ///< Its MouseDoubleClickMaxDist.

  /// @brief A press.
  /// @param at       Where, in window points, as ImGui's distance is.
  /// @param seconds  When, on a steady clock.
  /// @return Whether it completes a double click; the press after one starts
  ///         a new pair.
  bool press(glm::vec2 at, double seconds) noexcept;

 private:
  struct Press {
    glm::vec2 at;
    double seconds;
  };
  std::optional<Press> first_;  // the pair's first press, if any
};

}  // namespace volumetric_kit::player
