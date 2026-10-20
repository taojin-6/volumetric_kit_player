# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Third-party dependencies, fetched pinned (FetchContent): volumetric_kit_core,
# volumetric_kit_recon, volumetric_kit_gfx, and googletest when the tests are
# built. recon and gfx take Vulkan and GLM from the system, or on iOS as the
# core sets them up. Work on a sibling's checkout at once with
# -DFETCHCONTENT_SOURCE_DIR_VOLUMETRIC_KIT_<NAME>=<checkout>.

include(FetchContent)

# --- volumetric_kit_core -----------------------------------------------------
# The device, Status/Result and the GPU test fixtures, declared first
# (DECISIONS.md). The pin is core main at #42's merge (7757c13, 0.12.0): its iOS
# module, cmake/vkc_ios.cmake, links MoltenVK's iOS release (MoltenVK_ROOT) and
# answers glm. It is ahead of recon's and gfx's core pins (7a1219e, 1315d80),
# which yield to it because the player declares the core first. The floor below
# is that version, so an older FETCHCONTENT_SOURCE_DIR override is refused. The
# player installs nothing, so the core's install rules are off too.
set(VKC_WITH_VULKAN ON)
set(VKC_INSTALL OFF)
if(VP_BUILD_TESTS)
  set(VKC_BUILD_TEST_SUPPORT ON)
endif()
FetchContent_Declare(
  volumetric_kit_core
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_core.git
  GIT_TAG 7757c131d7283cacd1fb478c7b0118f7844954d6)
FetchContent_MakeAvailable(volumetric_kit_core)
vkc_require_core(0.12.0 VULKAN)

# --- volumetric_kit_recon ----------------------------------------------------
# The stream reader, the codec's decode grid, Marching Cubes and the mesh
# exchange. Its tests, examples, install and -Werror stay off in a subproject.
# The pin is recon main at #256's merge (8c8dec2): an empty mesh's payload
# reaches MeshExchange's commit, beside #255's volume::kDefaultBucketSize /
# kDefaultMaxChain and #253's codec::make_decode_grid and decode_growing.
FetchContent_Declare(
  volumetric_kit_recon
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_recon.git
  GIT_TAG 8c8dec2ece8287ac3d05ab843accfb90efa1b05e)
FetchContent_MakeAvailable(volumetric_kit_recon)

# --- volumetric_kit_gfx ------------------------------------------------------
# The swapchain on a shared device, the mesh pipeline, the orbit camera and the
# controls panel (ImGui). Its tests, examples, install and -Werror stay off in a
# subproject. Its GLFW tier is built with the desktop app alone, so not on iOS:
# the library links no window system (the top-level CMakeLists.txt checks).
# gfx's own core pin yields to the one above. The pin is gfx main at #146's
# merge (77f421e): windowing::SurfaceSource and glfw::Library, beside #143's
# orbit drag gestures, #144's sRGB ImGui fix and #145's ImGuiTouch.
# TODO(G1): PlayerViewConfig::ui_min_frame_height sets gfx's
# ImGuiOverlayConfig::min_frame_height (volumetric_kit_gfx
# feat/ui-touch-panels), which this pin lacks, so until that PR merges this
# branch builds only with -DFETCHCONTENT_SOURCE_DIR_VOLUMETRIC_KIT_GFX on its
# checkout. Once it merges, and before this branch merges, pin its merge commit
# here and wherever else the gfx pin is recorded (today nowhere else, no CI
# cache key included), restate the pin paragraph above, and in the core's
# paragraph drop gfx's core pin 1315d80, as gfx #147 pins the core at 7757c13
# like the player; then remove this block.
set(VG_WITH_GLFW ${VP_DESKTOP_APP})
FetchContent_Declare(
  volumetric_kit_gfx
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_gfx.git
  GIT_TAG 77f421e58d83f90dfc9401ed7492e88dbfe875c4)
FetchContent_MakeAvailable(volumetric_kit_gfx)

if(VP_BUILD_TESTS)
  set(INSTALL_GTEST
      OFF
      CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(googletest)
endif()
