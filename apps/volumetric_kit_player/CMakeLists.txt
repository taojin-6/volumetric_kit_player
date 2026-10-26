# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# The desktop window: one call of player_desktop's run_desktop (desktop.hpp),
# which links gfx's GLFW tier (src/).
add_executable(volumetric_kit_player main.cpp)
target_link_libraries(volumetric_kit_player
                      PRIVATE volumetric_kit::player_desktop)
vp_target_warnings(volumetric_kit_player)
