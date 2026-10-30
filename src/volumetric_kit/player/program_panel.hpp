// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file program_panel.hpp
/// @brief The player's part of a program's panel (PanelHooks), which gfx's
///        ui::DockedPanel lays out beside the scene: where the controls, the
///        stats and Source go. Player-internal.

#include "volumetric_kit/gfx/ui/docked_panel.hpp"
#include "volumetric_kit/player/controls.hpp"

namespace volumetric_kit::player {

/// @brief The scene's part of ImGui's work area, in points, for the
///        controls, the stats and Source.
/// @param laid  The panel's layout this frame (gfx::ui::DockedPanel::layout).
/// @param open  Whether that layout found the panel open.
/// @return All of the work area while closed; otherwise its part over the
///         scene, less a frame height on the panel's side, which the
///         panel's handle may reach over.
/// @pre An ImGui frame is open, with its context current.
///
/// @code
/// const gfx::ui::PanelLayout& laid = panel.layout(drawable);
/// draw_controls(player, shown, normals, unlit, nullptr,
///               scene_area(laid, panel.open()));
/// @endcode
UiArea scene_area(const gfx::ui::PanelLayout& laid, bool open);

}  // namespace volumetric_kit::player
