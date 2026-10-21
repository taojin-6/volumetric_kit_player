# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# The player's logic, a static library in the build tree only, so the tests link
# what the app runs. Headers sit beside their sources: the app exports nothing.
# It links no window system (DECISIONS.md): a shell adds GLFW or UIKit.
find_package(Threads REQUIRED)

add_library(
  player STATIC
  volumetric_kit/player/controls.cpp volumetric_kit/player/mesh_wiring.cpp
  volumetric_kit/player/playback.cpp volumetric_kit/player/playback_stats.cpp
  volumetric_kit/player/player_view.cpp volumetric_kit/player/stream_player.cpp)
add_library(volumetric_kit::player ALIAS player)
target_include_directories(player PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  player
  PUBLIC volumetric_kit::recon_codec
         volumetric_kit::recon_mesh
         volumetric_kit::recon_stream
         volumetric_kit::gfx_app
         volumetric_kit::gfx_camera
         volumetric_kit::gfx_pipelines
         volumetric_kit::gfx_ui
         Threads::Threads)
vp_target_warnings(player)
