# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

include(GoogleTest)

# The tests' streams (test_stream.hpp), written once a run by vp_test_streams,
# before the tests that read them: it skips with no device, as they do.
set(VP_TEST_STREAMS "${CMAKE_CURRENT_BINARY_DIR}/streams")
add_test(NAME vp_test_streams COMMAND vp_write_stream "${VP_TEST_STREAMS}")
# The skip code is the core's test::kSkipExitCode.
set_tests_properties(vp_test_streams PROPERTIES FIXTURES_SETUP vp_streams
                                                SKIP_RETURN_CODE 77)

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
# Its live park test 50 times rather than 8: a soak, on demand with `ctest -L
# stress`, which CI leaves out (DECISIONS.md).
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
# The mesh wiring, and the controls on a CPU ImGui context: host only.
vp_add_test(mesh_wiring volumetric_kit::player)
vp_add_test(controls volumetric_kit::player)
# The view, on its own device over gfx's headless surface. It skips where the
# loader has none (NVIDIA's ICD).
vp_add_test(player_view STREAMS volumetric_kit::player
            volumetric_kit::core_test_support)
# The desktop app's entry point, the app run with VP_NO_WINDOW set (AGENTS.md),
# and the scenarios in config/examples/; host only.
vp_add_test(cli volumetric_kit::player volumetric_kit::core_config)
target_compile_definitions(
  vp_cli_test
  PRIVATE VP_PLAYER_APP="$<TARGET_FILE:volumetric_kit_player>"
          VP_CONFIG_EXAMPLES="${PROJECT_SOURCE_DIR}/config/examples")
add_dependencies(vp_cli_test volumetric_kit_player)
# The tests' streams as files, for vp_test_streams above and CI's smoke run of
# the window (ci.yml): a tool, not a test.
add_executable(vp_write_stream vp_write_stream.cpp)
target_link_libraries(
  vp_write_stream
  PRIVATE volumetric_kit::recon_codec volumetric_kit::recon_stream
          volumetric_kit::recon_tsdf volumetric_kit::core_test_policy
          GTest::gtest)
vp_target_warnings(vp_write_stream)
# A sender's live stream played through the view with no window, its status and
# stats printed once a second, for agents on a Mac (DECISIONS.md): a tool, not a
# test.
if(APPLE)
  add_executable(vp_live_probe vp_live_probe.cpp)
  target_link_libraries(vp_live_probe PRIVATE volumetric_kit::player)
  vp_target_warnings(vp_live_probe)
endif()
# MoltenVK has a headless surface and several queue families, so on Apple the
# view's tests cross families rather than skip, on MoltenVK's default families
# and on its specialized ones, as the core's cross-family tests do: any skip but
# the one without a device fails.
if(APPLE)
  foreach(specialized 0 1)
    set(name vp_player_view_on_moltenvk_specialized_${specialized})
    add_test(
      NAME ${name}
      COMMAND
        "${CMAKE_COMMAND}" -E env
        MVK_CONFIG_SPECIALIZED_QUEUE_FAMILIES=${specialized}
        "$<TARGET_FILE:vp_player_view_test>" --gtest_filter=PlayerViewTest.*)
    set_tests_properties(
      ${name}
      PROPERTIES FAIL_REGULAR_EXPRESSION
                 "\\[  SKIPPED \\]"
                 SKIP_REGULAR_EXPRESSION
                 "no Vulkan device for this test"
                 FIXTURES_REQUIRED
                 vp_streams
                 TIMEOUT
                 300)
  endforeach()
endif()
