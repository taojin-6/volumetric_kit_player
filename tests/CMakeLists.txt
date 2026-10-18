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

# The pacing clock, host only.
vp_add_test(playback volumetric_kit::player)
# The decode and mesh worker, on a device (the core's fixtures); its stream is
# written with recon's mesh integrator.
vp_add_test(stream_player volumetric_kit::player
            volumetric_kit::core_test_support volumetric_kit::recon_tsdf)
