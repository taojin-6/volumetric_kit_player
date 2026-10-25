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

# The apps, tests and -Werror default ON only when the player is the top-level
# project, as in core, recon and gfx, so a project that adds the player builds
# its library alone, with no GLFW; the tests never on iOS: they run on the host.
option(VP_BUILD_APPS "Build the apps" ${PROJECT_IS_TOP_LEVEL})
# The apps: the desktop app, a GLFW window, everywhere but iOS, which builds the
# iOS app instead (DECISIONS.md). gfx's GLFW tier is the desktop app's alone
# (third_party/), so VP_DESKTOP_APP=OFF builds a desktop's library, tests and
# vp_probe with no GLFW, as the measurement runner does (tools/measure/).
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(VP_IOS ON)
  set(VP_DESKTOP_APP OFF)
else()
  set(VP_IOS OFF)
  option(VP_DESKTOP_APP "Build the desktop app, which needs GLFW" ON)
  if(NOT VP_BUILD_APPS)
    set(VP_DESKTOP_APP OFF)
  endif()
endif()

if(VP_IOS)
  set(vp_tests_default OFF)
else()
  set(vp_tests_default ${PROJECT_IS_TOP_LEVEL})
endif()
option(VP_BUILD_TESTS "Build tests" ${vp_tests_default})
option(VP_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors"
       ${PROJECT_IS_TOP_LEVEL})

include(vp_warnings)
include(vp_link_closure)
include(vp_build_info)

add_subdirectory(third_party)
add_subdirectory(src)
if(VP_IOS AND VP_BUILD_APPS)
  add_subdirectory(apps/ios)
elseif(VP_DESKTOP_APP)
  add_subdirectory(apps/volumetric_kit_player)
endif()

if(VP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Last, once every directory has linked what it links: gfx's GLFW tier is built
# for the app, so keep it out of the library.
vp_check_not_linked(player glfw)
