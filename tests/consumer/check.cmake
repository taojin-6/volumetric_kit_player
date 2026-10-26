# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# The player's build as another project takes it, run by the vp_consumer_* tests
# (tests/CMakeLists.txt) with CHECK set, in BINARY_DIR:
#
# * subproject: the consumer (CMakeLists.txt here) adds the player from
#   SOURCE_DIR and builds its program Release, which links
#   volumetric_kit::player and player_probe and runs; then, where DESKTOP is on,
#   configures it again with VP_DESKTOP_APP turned on and builds its program,
#   which links player_desktop too, and runs it.
# * top_level: the player configured alone, from a clean tree, as the top-level
#   project, where its apps, tests and -Werror default ON.
#
# GENERATOR and CXX_COMPILER repeat the calling build's, and DESKTOP is its
# VP_DESKTOP_APP, which reaches the top-level player as given, so a build
# without GLFW checks without it. Every FETCHCONTENT_SOURCE_DIR_<NAME> given is
# passed on, so it needs no network. The build takes
# CMAKE_BUILD_PARALLEL_LEVEL's jobs, or one per core.
foreach(_var CHECK SOURCE_DIR BINARY_DIR GENERATOR CXX_COMPILER DESKTOP)
  if(NOT DEFINED ${_var})
    message(FATAL_ERROR "check.cmake: ${_var} is not set")
  endif()
endforeach()

set(_given -G "${GENERATOR}" "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
get_cmake_property(_vars VARIABLES)
foreach(_var IN LISTS _vars)
  if(_var MATCHES "^FETCHCONTENT_SOURCE_DIR_.+$")
    list(APPEND _given "-D${_var}=${${_var}}")
  endif()
endforeach()

# Runs `cmake <ARGN>`, failing with its output, which names <what>, unless it
# succeeds.
function(_vp_cmake what)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" ${ARGN}
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _output)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "${what} failed:\n${_output}")
  endif()
endfunction()

if(CHECK STREQUAL "subproject")
  set(_jobs "")
  if(NOT DEFINED ENV{CMAKE_BUILD_PARALLEL_LEVEL})
    cmake_host_system_information(RESULT _cores QUERY NUMBER_OF_LOGICAL_CORES)
    set(_jobs --parallel ${_cores})
  endif()
  set(_desktops OFF)
  if(DESKTOP)
    list(APPEND _desktops ON)
  endif()
  foreach(_desktop IN LISTS _desktops)
    _vp_cmake(
      "Configuring the consumer (VP_CONSUMER_DESKTOP=${_desktop})"
      -S
      "${CMAKE_CURRENT_LIST_DIR}"
      -B
      "${BINARY_DIR}"
      ${_given}
      -DCMAKE_BUILD_TYPE=Release
      "-DVP_SOURCE_DIR=${SOURCE_DIR}"
      "-DVP_CONSUMER_DESKTOP=${_desktop}")
    _vp_cmake(
      "Building the consumer on the player (VP_CONSUMER_DESKTOP=${_desktop})"
      --build
      "${BINARY_DIR}"
      --config
      Release
      --target
      vp_consumer
      ${_jobs})
  endforeach()
elseif(CHECK STREQUAL "top_level")
  file(REMOVE_RECURSE "${BINARY_DIR}")
  _vp_cmake(
    "Configuring the player"
    -S
    "${SOURCE_DIR}"
    -B
    "${BINARY_DIR}"
    ${_given}
    "-DVP_DESKTOP_APP=${DESKTOP}")
  foreach(_option VP_BUILD_APPS VP_BUILD_TESTS VP_WARNINGS_AS_ERRORS)
    file(STRINGS "${BINARY_DIR}/CMakeCache.txt" _entry
         REGEX "^${_option}:BOOL=")
    if(NOT _entry STREQUAL "${_option}:BOOL=ON")
      message(FATAL_ERROR "${_option} must default ON for the top-level "
                          "player, not '${_entry}'")
    endif()
  endforeach()
else()
  message(FATAL_ERROR "check.cmake: CHECK is '${CHECK}', not subproject or "
                      "top_level")
endif()
