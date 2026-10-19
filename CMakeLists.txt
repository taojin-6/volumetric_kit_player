# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

cmake_minimum_required(VERSION 3.21)

project(
  volumetric_kit_player
  VERSION 0.0.1
  DESCRIPTION "Plays volumetric_kit .vrst streams on the GPU"
  HOMEPAGE_URL "https://github.com/taojin-6/volumetric_kit_player"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# The desktop app, a GLFW window, everywhere but iOS, which builds a never-run
# app that links the library instead (DECISIONS.md). gfx's GLFW tier is the
# desktop app's alone (third_party/).
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(VP_DESKTOP_APP OFF)
else()
  set(VP_DESKTOP_APP ON)
endif()

# An application is always the top-level project, so both default ON, but the
# tests on iOS: they run on the host, and cli_test runs the desktop app.
option(VP_BUILD_TESTS "Build tests" ${VP_DESKTOP_APP})
option(VP_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)

include(vp_warnings)
include(vp_link_closure)

add_subdirectory(third_party)
add_subdirectory(src)
if(VP_DESKTOP_APP)
  add_subdirectory(apps/volumetric_kit_player)
else()
  add_subdirectory(tests/ios_link_check)
endif()

if(VP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Last, once every directory has linked what it links: gfx's GLFW tier is built
# for the app, so keep it out of the library.
vp_check_not_linked(player glfw)
