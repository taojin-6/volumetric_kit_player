# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Third-party dependencies, fetched pinned (FetchContent): volumetric_kit_core,
# volumetric_kit_recon, volumetric_kit_gfx, and googletest when the tests are
# built. recon and gfx take Vulkan and GLM from the system. Work on a sibling's
# checkout at once with
# -DFETCHCONTENT_SOURCE_DIR_VOLUMETRIC_KIT_<NAME>=<checkout>.

include(FetchContent)

# --- volumetric_kit_core -----------------------------------------------------
# The device, Status/Result and the GPU test fixtures, declared first
# (DECISIONS.md). The pin is the core recon fa4a41e pins (7a1219e, 0.10.0, core
# #36's merge), and the floor below is that version, so an older
# FETCHCONTENT_SOURCE_DIR override is refused. The player installs nothing, so
# the core's install rules are off too.
set(VKC_WITH_VULKAN ON)
set(VKC_INSTALL OFF)
if(VP_BUILD_TESTS)
  set(VKC_BUILD_TEST_SUPPORT ON)
endif()
FetchContent_Declare(
  volumetric_kit_core
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_core.git
  GIT_TAG 7a1219e5ecaf385778acfc60f4c55eb70053cfb5)
FetchContent_MakeAvailable(volumetric_kit_core)
vkc_require_core(0.10.0 VULKAN)

# --- volumetric_kit_recon ----------------------------------------------------
# The stream reader, the codec's decode grid, Marching Cubes and the mesh
# exchange. Its tests, examples, install and -Werror stay off in a subproject.
# The pin is recon main at #255's merge (fa4a41e): volume::kDefaultBucketSize /
# kDefaultMaxChain, beside #253's codec::make_decode_grid and decode_growing.
FetchContent_Declare(
  volumetric_kit_recon
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_recon.git
  GIT_TAG fa4a41e17ffa02cab2be98805acf1fafdb83dbf2)
FetchContent_MakeAvailable(volumetric_kit_recon)

# --- volumetric_kit_gfx ------------------------------------------------------
# The swapchain on a shared device, the mesh pipeline, the orbit camera and the
# controls panel (ImGui). Its tests, examples, install and -Werror stay off in a
# subproject, and so does its GLFW tier: the library links no window system
# (DECISIONS.md). gfx's own core pin yields to the one above. The pin is gfx
# main at #146's merge (77f421e): windowing::SurfaceSource, beside #143's orbit
# drag gestures, #144's sRGB ImGui fix and #145's ImGuiTouch.
set(VG_WITH_GLFW OFF)
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
