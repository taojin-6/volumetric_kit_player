# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Third-party dependencies, fetched pinned (FetchContent): volumetric_kit_core,
# volumetric_kit_recon, volumetric_kit_gfx, and googletest when the tests are
# built. recon and gfx take Vulkan and GLM from the system, or on iOS as the
# core sets them up. Work on a sibling's checkout at once with
# -DFETCHCONTENT_SOURCE_DIR_VOLUMETRIC_KIT_<NAME>=<checkout>.

include(FetchContent)

# --- volumetric_kit_core -----------------------------------------------------
# The device, Status/Result and the GPU test fixtures, declared first, and the
# config tier: its Source, which the library's open_source takes on every
# platform, and the Player profile the desktop app loads (DECISIONS.md). The pin
# is core #50's head (9b64ebb, 0.20.0): rows with busy, wait and device halves
# (core::DeviceTime), tiled device time and Device::counters, which the stats
# and the measure log read, and scopes that close on the thread that opened
# them; gfx and recon pin it too. Its iOS module, cmake/vkc_ios.cmake, links
# MoltenVK's iOS release (MoltenVK_ROOT) and answers glm. The floor below is the
# pin's version, which gfx and recon need too, so an older
# FETCHCONTENT_SOURCE_DIR override is refused.
#
# TODO(timing): re-pin core, recon and gfx to the merge commits of core #50,
# recon #272 and gfx #153.
set(VKC_WITH_VULKAN ON)
set(VKC_WITH_CONFIG ON)
if(VP_BUILD_TESTS)
  set(VKC_BUILD_TEST_SUPPORT ON)
endif()
FetchContent_Declare(
  volumetric_kit_core
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_core.git
  GIT_TAG 9b64ebbd109ccb6dc3d7928d1278a01d6a08084e)
FetchContent_MakeAvailable(volumetric_kit_core)
vkc_require_core(0.20.0 VULKAN CONFIG)

# --- volumetric_kit_recon ----------------------------------------------------
# The stream reader, the codec's decode grid, Marching Cubes, the mesh exchange
# and the live stream's client. Its tests, examples and -Werror stay off in a
# subproject, so its library's core floor (0.20.0) is the one checked. The pin
# is recon #272's head (75c51bb: the extract's and the decode's breakdown rows,
# each with a device half; a timer made at the first timed call), on main at
# #262's merge (ee7fa44); under it, #269's StreamClient::next progress output,
# #266's block ceiling for codec::make_decode_grid and decode_growing, #260's
# networking::StreamClient and StreamServer, #256's empty mesh payload and
# #255's volume::kDefaultBucketSize / kDefaultMaxChain.
FetchContent_Declare(
  volumetric_kit_recon
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_recon.git
  GIT_TAG 75c51bb83b1097410ffd2648a94b59a3e6cbb363)
FetchContent_MakeAvailable(volumetric_kit_recon)

# --- volumetric_kit_gfx ------------------------------------------------------
# The swapchain on a shared device, the mesh pipeline, the orbit camera, the
# controls and stats panels (ImGui) and the profiler that times the draw. Its
# tests, examples and -Werror stay off in a subproject. Its GLFW tier is built
# with the desktop app alone, so not on iOS: the library links no window system
# (the top-level CMakeLists.txt checks). The pin is gfx #153's head (a0a7a01):
# ui::draw_stage_table's host, wait and GPU columns under a ui::StageWindow, and
# the frame loop's own wait, submit and present rows, on main at #152's merge
# (f147e00): #150's FIFO present by default, which the view takes, beside #148's
# ImGuiOverlayConfig::min_frame_height and ui::begin_collapsible_panel, #146's
# windowing::SurfaceSource and glfw::Library, #145's ImGuiTouch, #144's sRGB
# ImGui fix and #143's orbit drag gestures.
set(VG_WITH_GLFW ${VP_DESKTOP_APP})
FetchContent_Declare(
  volumetric_kit_gfx
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_gfx.git
  GIT_TAG a0a7a01e26b4e7417653035edd22999341b8147f)
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
