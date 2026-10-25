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
# is core PR #52's head (eaf142e, 0.21.0), recon's pin's base too: HostRing,
# whose slots recon's decoder and maps keep, in place where device-mapped memory
# is cached and copied elsewhere; CommandBatch::input and output; and
# DeviceCounters::dispatches and barriers, which the stamps and the measure log
# carry. Under it, 0.20.0's rows with busy, wait and device halves
# (core::DeviceTime), tiled device time, PendingBatch::device_rows and
# device_span, GpuTimer::set_max_depth and Device::counters and calibrate. Its
# iOS module, cmake/vkc_ios.cmake, links MoltenVK's iOS release (MoltenVK_ROOT)
# and answers glm. The floor below is the pin's version, so an older
# FETCHCONTENT_SOURCE_DIR override is refused.
# TODO(wave2): re-pin to core's merge commit.
set(VKC_WITH_VULKAN ON)
set(VKC_WITH_CONFIG ON)
if(VP_BUILD_TESTS)
  set(VKC_BUILD_TEST_SUPPORT ON)
endif()
FetchContent_Declare(
  volumetric_kit_core
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_core.git
  GIT_TAG eaf142ef0a685ed384563ffdcf86a544bb42eae5)
FetchContent_MakeAvailable(volumetric_kit_core)
vkc_require_core(0.21.0 VULKAN CONFIG)

# --- volumetric_kit_recon ----------------------------------------------------
# The stream reader, the codec's decode grid and its deblock, Marching Cubes,
# the mesh exchange and the live stream's client. Its tests, examples and
# -Werror stay off in a subproject, so its library's core floor is the one
# checked; its own core pin is the player's. The pin is recon PR #277's head
# (1f42ed0b): a frame decoded and meshed in the caller's batch
# (codec::record_growing, finish_growing; mesh::MarchingCubes::record_extract,
# finish, abandon), DeviceMesh::ready, Decoder::claim_input and last_rounds, and
# the reads straight into caller memory (StreamReader::read,
# networking::StreamClient::read). Under it, #274's per-block deblock fallback,
# #272's breakdown rows, #266's block ceiling for codec::make_decode_grid,
# #260's networking::StreamClient and StreamServer, #256's empty mesh payload at
# MeshExchange's commit and #255's volume::kDefaultBucketSize /
# kDefaultMaxChain.
# TODO(wave2): re-pin to recon's merge commit.
FetchContent_Declare(
  volumetric_kit_recon
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_recon.git
  GIT_TAG 1f42ed0be8b9526a50727ef16b10401a1aeeff17)
FetchContent_MakeAvailable(volumetric_kit_recon)

# --- volumetric_kit_gfx ------------------------------------------------------
# The swapchain on a shared device, the mesh pipeline, the orbit camera, the
# controls and stats panels (ImGui) and the profiler that times the draw. Its
# tests, examples and -Werror stay off in a subproject. Its GLFW tier is built
# with the desktop app alone, so not on iOS: the library links no window system
# (the top-level CMakeLists.txt checks). The pin is gfx main at #153's merge
# (123d013): ui::draw_stage_table's host, wait and GPU columns under a
# ui::StageWindow, and the frame loop's own wait, submit and present rows;
# beside them, #150's FIFO present by default, which the view takes, #148's
# ImGuiOverlayConfig::min_frame_height and ui::begin_collapsible_panel, #146's
# windowing::SurfaceSource and glfw::Library, #145's ImGuiTouch, #144's sRGB
# ImGui fix and #143's orbit drag gestures.
set(VG_WITH_GLFW ${VP_DESKTOP_APP})
FetchContent_Declare(
  volumetric_kit_gfx
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_gfx.git
  GIT_TAG 123d01326d26bc8f58b847ad8ff435ebff9e22dc)
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
