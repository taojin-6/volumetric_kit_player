# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# The player's logic, a static library in the build tree only, so the tests link
# what the app runs. Headers sit beside their sources: the app exports nothing.
# It links no window system (DECISIONS.md): a shell adds GLFW or UIKit. The
# desktop app's and vp_probe's entry points are libraries of their own, below.
find_package(Threads REQUIRED)

add_library(
  player STATIC
  volumetric_kit/player/command_line.cpp
  volumetric_kit/player/controls.cpp
  volumetric_kit/player/frame_hooks.cpp
  volumetric_kit/player/live_connection.cpp
  volumetric_kit/player/measure_log.cpp
  volumetric_kit/player/mesh_wiring.cpp
  volumetric_kit/player/playback.cpp
  volumetric_kit/player/playback_stats.cpp
  volumetric_kit/player/player_view.cpp
  volumetric_kit/player/pointer.cpp
  volumetric_kit/player/program_panel.cpp
  volumetric_kit/player/scenario.cpp
  volumetric_kit/player/stream_player.cpp)
add_library(volumetric_kit::player ALIAS player)
target_include_directories(player PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  player
  PUBLIC volumetric_kit::core_config
         volumetric_kit::grid_codec
         volumetric_kit::grid_mesh
         volumetric_kit::grid_networking
         volumetric_kit::grid_stream
         volumetric_kit::gfx_app
         volumetric_kit::gfx_camera
         volumetric_kit::gfx_pipelines
         volumetric_kit::gfx_ui
         Threads::Threads)
vp_target_warnings(player)
# The commits and the configuration the measure log's header states.
vp_build_info(player volumetric_kit/player/measure_log.cpp)

# The desktop app as a library call (desktop.hpp), under VP_DESKTOP_APP: the one
# library that links gfx's GLFW tier (DECISIONS.md). The core's config tier,
# through the library, reads its command line and loads its scenario.
if(VP_DESKTOP_APP)
  if(NOT TARGET volumetric_kit::gfx_glfw)
    message(
      FATAL_ERROR
        "player_desktop needs gfx_glfw, which needs GLFW 3.3 or newer: "
        "brew install glfw, or apt-get install libglfw3-dev")
  endif()
  add_library(player_desktop STATIC volumetric_kit/player/desktop.cpp)
  add_library(volumetric_kit::player_desktop ALIAS player_desktop)
  target_link_libraries(
    player_desktop
    PUBLIC volumetric_kit::player
    PRIVATE volumetric_kit::gfx_glfw)
  vp_target_warnings(player_desktop)
endif()

# vp_probe as a library call (probe.hpp), on every desktop, built only for a
# target that links it. It validates as the core's test policy asks, which
# third_party/ builds on a desktop; a project that made the core without it gets
# no player_probe.
if(NOT VP_IOS AND TARGET volumetric_kit::core_test_policy)
  add_library(player_probe STATIC EXCLUDE_FROM_ALL
              volumetric_kit/player/probe.cpp)
  add_library(volumetric_kit::player_probe ALIAS player_probe)
  target_link_libraries(
    player_probe
    PUBLIC volumetric_kit::player
    PRIVATE volumetric_kit::grid_mesh_io volumetric_kit::core_test_policy)
  vp_target_warnings(player_probe)
endif()
