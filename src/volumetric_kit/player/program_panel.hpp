// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file program_panel.hpp
/// @brief A program's panel docked beside the scene (PanelHooks): its
///        layout, its handle and header, drawn on the view's ImGui context.
///        Player-internal.

#include <functional>
#include <vector>

#include <glm/vec2.hpp>

#include "volumetric_kit/core/vulkan/vulkan.hpp"
#include "volumetric_kit/player/controls.hpp"
#include "volumetric_kit/player/frame_hooks.hpp"

namespace volumetric_kit::player {

/// The panel's handle's ImGui window (ProgramPanel::draw_chrome).
inline constexpr const char* kPanelHandleWindow = "##player_panel_handle";
/// Its header's.
inline constexpr const char* kPanelHeaderWindow = "##player_panel_header";

/// @brief What a shell keeps of a program's panel across a rebuild of its
///        view (PlayerViewConfig::panel, PlayerView::panel_state).
struct PanelState {
  bool open = false;          ///< Whether it shows.
  float right_share = 0.4f;   ///< Its share of the safe width, docked right.
  float bottom_share = 0.4f;  ///< Of the safe height, docked at the bottom.
};

/// @brief A program's panel docked beside the scene: the two split the
///        drawable, so nothing covers the scene.
///
/// - **Side.** The end of the drawable's long axis: right, or at the bottom
///   when it is taller than wide (DockSide).
/// - **Share** of the safe area along that axis, held to @ref kMinShare to
///   @ref kMaxShare and to at least @ref kMinPoints across (@ref kMaxShare at
///   most). The panel reaches the drawable's edge; its handle, header and
///   content stay in ImGui's work area, the safe area that `ImGuiTouch`
///   sets.
/// - **Chrome** (@ref draw_chrome), ImGui windows that take the pointer: the
///   handle, a bar @ref kBarPoints wide on the panel's inner edge that takes
///   the pointer over a frame height (44 points on a touch overlay); and the
///   header across the panel's top: the title, the program's items, and a
///   close button. A drag on the handle resizes, snapping to the detents on
///   release; a release below @ref kMinShare closes the panel.
///
/// Rectangles are in drawable pixels; the chrome is in ImGui points, which
/// `DisplayFramebufferScale` converts.
///
/// @code
/// ProgramPanel panel({/*open=*/true, 0.45f, 0.45f}, {0.45f, 0.6f});
/// // each frame, after ImGuiOverlay::new_frame:
/// const PanelFrame& laid = panel.layout(drawable);
/// draw_controls(player, shown, normals, unlit, nullptr, panel.scene_area(),
///               "Inspect", &open);
/// panel.draw_chrome("Inspector", [] { ImGui::Button("Reset"); });
/// // the mesh in panel.scene(), the program's drawing in laid.content
/// @endcode
class ProgramPanel {
 public:
  /// The least and the most share of the safe area along the dock axis.
  static constexpr float kMinShare = 0.25f;
  static constexpr float kMaxShare = 0.6f;
  /// The panel's least size across, in points.
  static constexpr float kMinPoints = 240.0f;
  /// The handle's bar, in points.
  static constexpr float kBarPoints = 6.0f;

  /// @param state    The state it starts in.
  /// @param detents  The shares a drag on the handle snaps to on release,
  ///                 such as `{0.45f, 0.6f}` on a phone; empty, a drag
  ///                 resizes freely.
  explicit ProgramPanel(const PanelState& state = {},
                        std::vector<float> detents = {});

  /// @return Whether it shows.
  bool open() const noexcept { return state_.open; }
  /// @brief Show or hide it, from the next @ref layout.
  void set_open(bool open) noexcept { state_.open = open; }
  /// @return Its state, for a shell to carry across a rebuild.
  PanelState state() const noexcept { return state_; }

  /// @brief Lay out @p drawable for this frame, inside ImGui's work area.
  /// @param drawable  The drawable's size, in pixels.
  /// @return The program's rectangles, valid until the next call; an empty
  ///         content while closed.
  /// @pre An ImGui frame is open, with its context current.
  const PanelFrame& layout(VkExtent2D drawable);
  /// @return The program's rectangles as of the last @ref layout.
  const PanelFrame& laid_out() const noexcept { return frame_; }
  /// @return Whether the last @ref layout found it open.
  bool laid_out_open() const noexcept { return laid_out_open_; }
  /// @return The scene's rectangle as of the last @ref layout, in pixels:
  ///         the whole drawable while closed, otherwise the drawable up to
  ///         the panel's inner edge.
  const VkRect2D& scene() const noexcept { return scene_; }
  /// @return The scene's part of the work area as of the last @ref layout,
  ///         in points, for the controls, the stats and Source: all of it
  ///         while closed; otherwise the part over the scene, less a frame
  ///         height on the panel's side, which the handle may take.
  const UiArea& scene_area() const noexcept { return scene_area_; }

  /// @brief Draw the handle and the header: the title, then @p items, then
  ///        a close button at the header's right end. Closed, it draws
  ///        nothing.
  /// @param title  The header's text.
  /// @param items  The program's items, inside the header's window after
  ///               the title, on its line; empty, none.
  /// @pre @ref layout ran this frame.
  void draw_chrome(const char* title, const std::function<void()>& items);

  /// @return Whether @p point, in drawable pixels, is in the content of the
  ///         last layout, with the panel open.
  bool over_content(glm::vec2 point) const noexcept;

 private:
  // The share the panel takes on `side`.
  float& share(DockSide side) noexcept;

  std::vector<float> detents_;
  PanelState state_;
  PanelFrame frame_;
  VkRect2D scene_{};
  UiArea scene_area_;
  bool laid_out_open_ = false;
  // This frame's chrome, in drawable pixels: the safe area, the panel's inner
  // edge along the dock axis, and the header's height.
  float safe_[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // x0, y0, x1, y1
  float edge_ = 0.0f;
  float header_ = 0.0f;
  // A drag on the handle: the share it started from, and that share moved
  // by the pointer.
  float drag_from_ = 0.0f;
  float drag_share_ = 0.0f;
};

}  // namespace volumetric_kit::player
