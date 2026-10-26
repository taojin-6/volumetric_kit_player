# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# A stand-in for a project that adds the player, configured and built by
# check.cmake here with -DVP_SOURCE_DIR=<player>. The player is not the
# top-level project, so its apps, tests and -Werror must default OFF, and with
# them VP_DESKTOP_APP, which leaves gfx's GLFW tier out; its library and
# player_probe, which the consumer's program links, are all it builds. With
# VP_CONSUMER_DESKTOP, the consumer turns VP_DESKTOP_APP on alone, as a project
# may, and its program links player_desktop too, which the player then builds
# with gfx's GLFW tier, and still no app. Built, the program runs.
cmake_minimum_required(VERSION 3.21)
project(vp_consumer LANGUAGES CXX)

option(VP_CONSUMER_DESKTOP "Turn the player's VP_DESKTOP_APP on" OFF)

# Cleared first, so a reconfigure checks the player's defaults, not a cached
# value.
set(_options VP_BUILD_APPS VP_BUILD_TESTS VP_WARNINGS_AS_ERRORS VP_DESKTOP_APP)
foreach(_option IN LISTS _options)
  unset(${_option} CACHE)
endforeach()
if(VP_CONSUMER_DESKTOP)
  set(VP_DESKTOP_APP ON)
endif()
add_subdirectory("${VP_SOURCE_DIR}" volumetric_kit_player)
foreach(_option IN LISTS _options)
  if(${_option} AND NOT (_option STREQUAL "VP_DESKTOP_APP"
                         AND VP_CONSUMER_DESKTOP))
    message(FATAL_ERROR "${_option} must default OFF for a player subproject")
  endif()
endforeach()
foreach(_target volumetric_kit_player vp_probe)
  if(TARGET ${_target})
    message(FATAL_ERROR "a player subproject made ${_target}")
  endif()
endforeach()
foreach(_target volumetric_kit::player_desktop volumetric_kit::gfx_glfw)
  if(VP_CONSUMER_DESKTOP AND NOT TARGET ${_target})
    message(FATAL_ERROR "VP_DESKTOP_APP on made no ${_target}")
  elseif(NOT VP_CONSUMER_DESKTOP AND TARGET ${_target})
    message(FATAL_ERROR "a player subproject made ${_target}")
  endif()
endforeach()

add_executable(vp_consumer consumer.cpp)
target_link_libraries(vp_consumer PRIVATE volumetric_kit::player
                                          volumetric_kit::player_probe)
if(VP_CONSUMER_DESKTOP)
  target_link_libraries(vp_consumer PRIVATE volumetric_kit::player_desktop)
  target_compile_definitions(vp_consumer PRIVATE VP_CONSUMER_DESKTOP)
endif()
add_custom_command(
  TARGET vp_consumer
  POST_BUILD
  COMMAND vp_consumer)
