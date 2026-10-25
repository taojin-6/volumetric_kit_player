# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# A stand-in for a project that adds the player, configured and built by
# check.cmake here with -DVP_SOURCE_DIR=<player>. The player is not the
# top-level project, so its apps, tests and -Werror must default OFF, which
# leaves gfx's GLFW tier out; its library, which the consumer's program links,
# is all it builds. Built, the program runs.
cmake_minimum_required(VERSION 3.21)
project(vp_consumer LANGUAGES CXX)

# Cleared first, so a reconfigure checks the player's defaults, not a cached
# value.
set(_options VP_BUILD_APPS VP_BUILD_TESTS VP_WARNINGS_AS_ERRORS)
foreach(_option IN LISTS _options)
  unset(${_option} CACHE)
endforeach()
add_subdirectory("${VP_SOURCE_DIR}" volumetric_kit_player)
foreach(_option IN LISTS _options)
  if(${_option})
    message(FATAL_ERROR "${_option} must default OFF for a player subproject")
  endif()
endforeach()
foreach(_target volumetric_kit_player vp_probe volumetric_kit::gfx_glfw)
  if(TARGET ${_target})
    message(FATAL_ERROR "a player subproject made ${_target}")
  endif()
endforeach()

add_executable(vp_consumer consumer.cpp)
target_link_libraries(vp_consumer PRIVATE volumetric_kit::player)
add_custom_command(
  TARGET vp_consumer
  POST_BUILD
  COMMAND vp_consumer)
