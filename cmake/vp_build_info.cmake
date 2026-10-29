# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# `vp_build_info(<target> <source>)` gives <source>, which defines the player's
# build_info() (measure_log.hpp), the commits of the player and of its fetched
# core, grid and gfx, read now, at configure time, and <target> the
# configuration it builds. A checkout with uncommitted changes to tracked files
# reads "<commit>-dirty"; a directory that is not a checkout's top, as an
# unpacked copy is, reads "unknown", rather than the commit of a checkout around
# it.
include(FetchContent)

function(vp_commit_of dir out)
  set(commit unknown)
  find_package(Git QUIET)
  if(GIT_FOUND AND IS_DIRECTORY "${dir}")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${dir}" rev-parse --show-toplevel HEAD
      OUTPUT_VARIABLE lines
      RESULT_VARIABLE failed
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT failed)
      string(REPLACE "\n" ";" lines "${lines}")
      list(GET lines 0 top)
      list(GET lines 1 head)
      file(REAL_PATH "${dir}" real_dir)
      file(REAL_PATH "${top}" real_top)
      if(real_dir STREQUAL real_top)
        set(commit "${head}")
        execute_process(
          COMMAND "${GIT_EXECUTABLE}" -C "${dir}" status --porcelain
                  --untracked-files=no
          OUTPUT_VARIABLE changes
          RESULT_VARIABLE failed
          ERROR_QUIET)
        if(failed OR NOT changes STREQUAL "")
          string(APPEND commit "-dirty")
        endif()
      endif()
    endif()
  endif()
  set(${out}
      "${commit}"
      PARENT_SCOPE)
endfunction()

function(vp_build_info target source)
  vp_commit_of("${PROJECT_SOURCE_DIR}" player)
  foreach(sibling core grid gfx)
    FetchContent_GetProperties(volumetric_kit_${sibling})
    vp_commit_of("${volumetric_kit_${sibling}_SOURCE_DIR}" ${sibling})
  endforeach()
  set_property(
    SOURCE "${source}"
    APPEND
    PROPERTY COMPILE_DEFINITIONS
             "VP_COMMIT_PLAYER=\"${player}\";VP_COMMIT_CORE=\"${core}\""
             "VP_COMMIT_GRID=\"${grid}\";VP_COMMIT_GFX=\"${gfx}\"")
  target_compile_definitions(${target} PRIVATE VP_BUILD_TYPE="$<CONFIG>")
  message(STATUS "Build info: player ${player}, core ${core}, "
                 "grid ${grid}, gfx ${gfx}")
endfunction()
