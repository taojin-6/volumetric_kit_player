# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# The desktop window: a GLFW shell over the player library (DECISIONS.md), the
# only target that links gfx's GLFW tier.
if(NOT TARGET volumetric_kit::gfx_glfw)
  message(
    FATAL_ERROR
      "volumetric_kit_player needs gfx_glfw, which needs GLFW 3.3 or newer: "
      "brew install glfw, or apt-get install libglfw3-dev")
endif()

add_executable(volumetric_kit_player main.cpp)
target_link_libraries(volumetric_kit_player PRIVATE volumetric_kit::player
                                                    volumetric_kit::gfx_glfw)
vp_target_warnings(volumetric_kit_player)
