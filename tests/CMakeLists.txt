# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

include(GoogleTest)

# The tests' streams (test_stream.hpp), written once a run by vp_test_streams,
# before the tests that read them: it skips with no device, as they do.
set(VP_TEST_STREAMS "${CMAKE_CURRENT_BINARY_DIR}/streams")
add_test(NAME vp_test_streams COMMAND vp_write_stream "${VP_TEST_STREAMS}")
# The skip code is the core's test::kSkipExitCode.
set_tests_properties(vp_test_streams PROPERTIES FIXTURES_SETUP vp_streams
                                                SKIP_RETURN_CODE 77 TIMEOUT 300)

# vp_<name>_test from <name>_test.cpp, linking the given targets. A case that
# hangs fails after 5 minutes rather than holding a CI job to its limit. With
# STREAMS, its cases read the tests' streams in VP_TEST_STREAMS.
function(vp_add_test name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "STREAMS" "" "")
  add_executable(vp_${name}_test ${name}_test.cpp)
  target_link_libraries(vp_${name}_test PRIVATE ${arg_UNPARSED_ARGUMENTS}
                                                GTest::gtest_main)
  vp_target_warnings(vp_${name}_test)
  set(properties TIMEOUT 300)
  if(arg_STREAMS)
    target_compile_definitions(vp_${name}_test
                               PRIVATE VP_TEST_STREAMS="${VP_TEST_STREAMS}")
    list(APPEND properties FIXTURES_REQUIRED vp_streams)
  endif()
  gtest_discover_tests(vp_${name}_test PROPERTIES ${properties})
endfunction()

# The pacing clock and the stats panel's figures, host only.
vp_add_test(playback volumetric_kit::player)
vp_add_test(playback_stats volumetric_kit::player)
# The decode and mesh worker, on a device (the core's fixtures), over the tests'
# streams.
vp_add_test(stream_player STREAMS volumetric_kit::player
            volumetric_kit::core_test_support)
# Its live park test 50 times rather than 8: a soak in the `stress` label, which
# CI leaves out (DECISIONS.md).
add_test(
  NAME vp_live_park_soak
  COMMAND vp_stream_player_test
          --gtest_filter=LivePlayerTest.AParkClosesTheConnectionWithinASlice)
set_tests_properties(
  vp_live_park_soak
  PROPERTIES ENVIRONMENT
             VP_PARK_CYCLES=50
             LABELS
             stress
             FIXTURES_REQUIRED
             vp_streams
             TIMEOUT
             300)
# The live connection and its retry, against a sender on 127.0.0.1: host only.
vp_add_test(live_connection volumetric_kit::player)
# The mesh wiring, the controls and a program's panel on a CPU ImGui context,
# and the shells' input rules: host only.
vp_add_test(mesh_wiring volumetric_kit::player)
vp_add_test(controls volumetric_kit::player)
vp_add_test(program_panel volumetric_kit::player)
vp_add_test(pointer volumetric_kit::player)
# The view, on its own device over gfx's headless surface. It skips where the
# loader has none (NVIDIA's ICD).
vp_add_test(player_view STREAMS volumetric_kit::player
            volumetric_kit::core_test_support)
target_sources(vp_player_view_test PRIVATE test_allocation_failure.cpp)
# A scenario's source played (open_source), the iOS dialog's fields parsed
# (parse_live_source) and its folder's first stream (folder_source): host only.
vp_add_test(scenario volumetric_kit::player)
# The desktop app's entry point, the app run with VP_NO_WINDOW set (AGENTS.md),
# the view's settings it plays with, and the scenarios in config/examples/; host
# only, with the app.
if(TARGET volumetric_kit_player)
  vp_add_test(cli volumetric_kit::player_desktop)
  target_compile_definitions(
    vp_cli_test
    PRIVATE VP_PLAYER_APP="$<TARGET_FILE:volumetric_kit_player>"
            VP_CONFIG_EXAMPLES="${PROJECT_SOURCE_DIR}/config/examples")
  add_dependencies(vp_cli_test volumetric_kit_player)
endif()
# run_desktop in a program's own main, with hooks of the program's that count
# their calls: CI's Xvfb step runs it as it runs the app, never ctest, as it
# opens a window (AGENTS.md).
if(TARGET volumetric_kit::player_desktop)
  add_executable(vp_desktop_hooks vp_desktop_hooks.cpp)
  target_link_libraries(vp_desktop_hooks PRIVATE volumetric_kit::player_desktop)
  vp_target_warnings(vp_desktop_hooks)
endif()
# The command of vp_test_streams, above.
add_executable(vp_write_stream vp_write_stream.cpp)
target_link_libraries(
  vp_write_stream
  PRIVATE volumetric_kit::grid_codec volumetric_kit::grid_stream
          volumetric_kit::grid_voxelize volumetric_kit::core_test_policy
          GTest::gtest)
vp_target_warnings(vp_write_stream)
# The measure log, its file name and the iOS app's measure launch arguments, on
# the host, and the device line on a device (the core's fixtures).
vp_add_test(measure_log volumetric_kit::player
            volumetric_kit::core_test_support)
# A file or a sender's live stream played through the view with no window, or by
# the worker alone (--worker), its status and stats printed once a second, and
# each frame logged with --csv: the measurement runner's (tools/measure/,
# DECISIONS.md), a tool, one call of player_probe's run_probe (probe.hpp). ctest
# runs it on the tests' stream, two passes of every frame timed, with vertices
# shared and not: the worker alone everywhere, and the view where the loader has
# a headless surface, as MoltenVK does; then it checks the logs, as the
# summarizer reads them. It validates as the core's test policy asks, as the
# tests do.
add_executable(vp_probe vp_probe.cpp)
target_link_libraries(vp_probe PRIVATE volumetric_kit::player_probe)
vp_target_warnings(vp_probe)
# run_probe in a program's own main, with hooks of the program's
# (AppConfig::view.make_hooks), on the tests' stream: the worker alone
# everywhere, and the view where the loader has a headless surface, the hooks
# counting their calls or keeping a mesh more of the ring; on the host, its
# hooks' options refused with them, and the view's settings it plays with.
vp_add_test(probe STREAMS volumetric_kit::player_probe
            volumetric_kit::core_test_policy)
set(VP_PROBE_DIR "${CMAKE_CURRENT_BINARY_DIR}/probe")
file(MAKE_DIRECTORY "${VP_PROBE_DIR}")
set(probe_modes worker)
if(APPLE)
  list(APPEND probe_modes view)
endif()
set(probes "")
set(VP_PROBE_CSV "")
foreach(mode IN LISTS probe_modes)
  set(worker "")
  if(mode STREQUAL "worker")
    set(worker --worker)
  endif()
  foreach(sharing on off)
    set(csv "${VP_PROBE_DIR}/${mode}_${sharing}.csv")
    add_test(
      NAME vp_probe_${mode}_${sharing}_csv
      COMMAND
        vp_probe ${worker} --file "${VP_TEST_STREAMS}/sphere.vrst" --passes 2
        --timing all --share-vertices ${sharing} --csv "${csv}")
    list(APPEND probes vp_probe_${mode}_${sharing}_csv)
    list(APPEND VP_PROBE_CSV "${csv}")
  endforeach()
endforeach()
# The summarizer's check takes the logs joined by os.pathsep.
string(REPLACE ";" ":" VP_PROBE_CSV "${VP_PROBE_CSV}")
set_tests_properties(
  ${probes} PROPERTIES FIXTURES_REQUIRED vp_streams FIXTURES_SETUP vp_probe_csv
                       TIMEOUT 300)
# The summarizer (tools/measure/summarize.py) on its fixtures, and on the log
# above, whose check fails rather than skips without it; the runner by its
# --dry-run and command stubs, with bash and git (no device or measurement); -B
# leaves no bytecode in the source tree.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(VP_MEASURE_TOOLS "${PROJECT_SOURCE_DIR}/tools/measure")
set(VP_MEASURE_TESTS "${VP_MEASURE_TOOLS}/test_summarize.py")
add_test(NAME vp_measure_summarize COMMAND Python3::Interpreter -B
                                           "${VP_MEASURE_TESTS}" Summarize)
add_test(NAME vp_measure_dry_run
         COMMAND Python3::Interpreter -B "${VP_MEASURE_TOOLS}/test_measure.py"
                 DryRun)
add_test(NAME vp_measure_runner
         COMMAND Python3::Interpreter -B "${VP_MEASURE_TOOLS}/test_measure.py"
                 Runner)
add_test(NAME vp_probe_csv_check COMMAND Python3::Interpreter -B
                                         "${VP_MEASURE_TESTS}" ProbeLog)
set_tests_properties(
  vp_probe_csv_check
  PROPERTIES ENVIRONMENT
             "VP_PROBE_CSV=${VP_PROBE_CSV}"
             FIXTURES_REQUIRED
             vp_probe_csv
             FAIL_REGULAR_EXPRESSION
             "skipped"
             TIMEOUT
             60)
# vp_probe's hooks' options on the tests' stream, each run checked by
# probe_check.py: --latency's figures, the draws' through the view, and every
# record released; --save-meshes' PLY of each of its 6 frames (test_stream.hpp's
# kFrames), as its line states.
set(VP_PROBE_CHECK "${CMAKE_CURRENT_SOURCE_DIR}/probe_check.py")
set(hooked "")
foreach(mode IN LISTS probe_modes)
  set(worker "")
  set(draws 1)
  if(mode STREQUAL "worker")
    set(worker --worker)
    set(draws 0)
  endif()
  add_test(
    NAME vp_probe_${mode}_latency
    COMMAND
      Python3::Interpreter -B "${VP_PROBE_CHECK}" latency ${draws}
      $<TARGET_FILE:vp_probe> ${worker} --latency --file
      "${VP_TEST_STREAMS}/sphere.vrst")
  set(meshes "${VP_PROBE_DIR}/meshes_${mode}")
  add_test(
    NAME vp_probe_${mode}_save_meshes
    COMMAND
      Python3::Interpreter -B "${VP_PROBE_CHECK}" save-meshes "${meshes}" 6
      $<TARGET_FILE:vp_probe> ${worker} --save-meshes "${meshes}" --file
      "${VP_TEST_STREAMS}/sphere.vrst")
  list(APPEND hooked vp_probe_${mode}_latency vp_probe_${mode}_save_meshes)
endforeach()
set_tests_properties(${hooked} PROPERTIES FIXTURES_REQUIRED vp_streams TIMEOUT
                                          300)
# MoltenVK has a headless surface and several queue families, so on Apple the
# view's tests cross families rather than skip: on MoltenVK's default families
# above, and again on its specialized ones, as the core's cross-family tests do,
# where any skip but the one without a device fails.
if(APPLE)
  add_test(
    NAME vp_player_view_on_moltenvk_specialized
    COMMAND
      "${CMAKE_COMMAND}" -E env MVK_CONFIG_SPECIALIZED_QUEUE_FAMILIES=1
      "$<TARGET_FILE:vp_player_view_test>" --gtest_filter=PlayerViewTest.*)
  set_tests_properties(
    vp_player_view_on_moltenvk_specialized
    PROPERTIES FAIL_REGULAR_EXPRESSION
               "\\[  SKIPPED \\]"
               SKIP_REGULAR_EXPRESSION
               "no Vulkan device for this test"
               FIXTURES_REQUIRED
               vp_streams
               TIMEOUT
               300)
endif()

# The player as another project adds it, and as the top-level project
# (consumer/check.cmake), each dependency this build fetched, or took from a
# local source, passed on as a local source, as grid's subproject tests do. They
# check CMake, not a device, and the first builds the library again, so CI runs
# them on one leg (label consumer).
file(
  GLOB fetched
  LIST_DIRECTORIES true
  "${FETCHCONTENT_BASE_DIR}/*-src")
set(sources "")
foreach(source IN LISTS fetched)
  get_filename_component(name "${source}" NAME)
  string(REGEX REPLACE "-src$" "" name "${name}")
  string(TOUPPER "${name}" name)
  list(APPEND sources "-DFETCHCONTENT_SOURCE_DIR_${name}=${source}")
endforeach()
get_cmake_property(cache CACHE_VARIABLES)
foreach(var IN LISTS cache)
  if(var MATCHES "^FETCHCONTENT_SOURCE_DIR_" AND NOT "${${var}}" STREQUAL "")
    list(APPEND sources "-D${var}=${${var}}")
  endif()
endforeach()
foreach(check subproject top_level)
  add_test(
    NAME vp_consumer_${check}
    COMMAND
      "${CMAKE_COMMAND}" -DCHECK=${check} "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
      "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/consumer_${check}"
      "-DGENERATOR=${CMAKE_GENERATOR}" "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DDESKTOP=${VP_DESKTOP_APP}" ${sources} -P
      "${CMAKE_CURRENT_SOURCE_DIR}/consumer/check.cmake")
  set_tests_properties(vp_consumer_${check} PROPERTIES LABELS consumer TIMEOUT
                                                       900)
endforeach()
