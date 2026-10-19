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

# An application is always the top-level project, so both default ON.
option(VP_BUILD_TESTS "Build tests" ON)
option(VP_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)

include(vp_warnings)
include(vp_link_closure)

add_subdirectory(third_party)
add_subdirectory(src)
add_subdirectory(apps/volumetric_kit_player)

if(VP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
