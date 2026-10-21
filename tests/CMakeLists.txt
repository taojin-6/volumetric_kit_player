# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

include(GoogleTest)

# vp_<name>_test from <name>_test.cpp, linking the given targets. A case that
# hangs fails after 5 minutes rather than holding a CI job to its limit.
function(vp_add_test name)
  add_executable(vp_${name}_test ${name}_test.cpp)
  target_link_libraries(vp_${name}_test PRIVATE ${ARGN} GTest::gtest_main)
  vp_target_warnings(vp_${name}_test)
  gtest_discover_tests(
    vp_${name}_test
    PROPERTIES
    TIMEOUT 300)
endfunction()

# The pacing clock and the stats panel's figures, host only.
vp_add_test(playback volumetric_kit::player)
vp_add_test(playback_stats volumetric_kit::player)
# The decode and mesh worker, on a device (the core's fixtures), over the tests'
# stream (test_stream.hpp), which recon's mesh integrator writes.
vp_add_test(stream_player volumetric_kit::player
            volumetric_kit::core_test_support volumetric_kit::recon_tsdf)
# The mesh wiring, and the controls on a CPU ImGui context: host only.
vp_add_test(mesh_wiring volumetric_kit::player)
vp_add_test(controls volumetric_kit::player volumetric_kit::recon_tsdf)
# The view, on its own device over gfx's headless surface. It skips where the
# loader has none (NVIDIA's ICD).
vp_add_test(player_view volumetric_kit::player
            volumetric_kit::core_test_support volumetric_kit::recon_tsdf)
# The desktop app's command line, the app run on input it refuses before GLFW
# (AGENTS.md); host only.
vp_add_test(cli volumetric_kit::player volumetric_kit::recon_tsdf)
target_compile_definitions(
  vp_cli_test PRIVATE VP_PLAYER_APP="$<TARGET_FILE:volumetric_kit_player>")
add_dependencies(vp_cli_test volumetric_kit_player)
# The tests' stream as a file, for CI's smoke run of the window (ci.yml): a
# tool, not a test.
add_executable(vp_write_stream vp_write_stream.cpp)
target_link_libraries(
  vp_write_stream
  PRIVATE volumetric_kit::recon_codec volumetric_kit::recon_stream
          volumetric_kit::recon_tsdf GTest::gtest)
vp_target_warnings(vp_write_stream)
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
      PROPERTIES FAIL_REGULAR_EXPRESSION "\\[  SKIPPED \\]"
                 SKIP_REGULAR_EXPRESSION "no Vulkan device for this test"
                 TIMEOUT 300)
  endforeach()
endif()
