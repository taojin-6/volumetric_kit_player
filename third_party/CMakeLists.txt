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
# is core main at PR #63's merge (c529072, 0.26.0); recon's library asks for
# 0.25.0 (#62's merge, recon's own pin). #62 and #63 change only profiles the
# player does not load; the Player profile resolves as before. What the player
# uses is as at #52's merge (0.21.0): HostRing, whose slots recon's decoder and
# maps keep, in place where device-mapped memory is cached and copied elsewhere;
# CommandBatch::input and output; and DeviceCounters::dispatches and barriers,
# which the stamps and the measure log carry. Under it, 0.20.0's rows with busy,
# wait and device halves (core::DeviceTime), tiled device time,
# PendingBatch::device_rows and device_span, GpuTimer::set_max_depth and
# Device::counters and calibrate. Its iOS module, cmake/vkc_ios.cmake, links
# MoltenVK's iOS release (MoltenVK_ROOT) and answers glm. The floor below is the
# pin's version, so an older FETCHCONTENT_SOURCE_DIR override is refused.
set(VKC_WITH_VULKAN ON)
set(VKC_WITH_CONFIG ON)
# The test policy, which vp_probe's library validates by on every desktop, and
# the fixtures for the tests: each built only for a target that links it.
if(VP_BUILD_TESTS OR NOT VP_IOS)
  set(VKC_BUILD_TEST_SUPPORT ON)
endif()
FetchContent_Declare(
  volumetric_kit_core
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_core.git
  GIT_TAG c5290721b915c7e3fbc4585c451cb40941aa50ef)
FetchContent_MakeAvailable(volumetric_kit_core)
vkc_require_core(0.26.0 VULKAN CONFIG)

# --- volumetric_kit_recon ----------------------------------------------------
# The stream reader, the codec's decode grid and its deblock, Marching Cubes,
# the mesh exchange and the live stream's client. Its tests, examples and
# -Werror stay off in a subproject, so its library's core floor is the one
# checked; its own core pin is the player's. The pin is recon main at #299's
# merge (d8b8bda). The tiers the player links are as at #277's merge (a23f7f5):
# a frame decoded and meshed in the caller's batch (codec::record_growing,
# finish_growing; mesh::MarchingCubes::record_extract, finish, abandon),
# DeviceMesh::ready, Decoder::claim_input and last_rounds, and the reads
# straight into caller memory (StreamReader::read,
# networking::StreamClient::read). Under it, #274's per-block deblock fallback,
# #272's breakdown rows, #266's block ceiling for codec::make_decode_grid,
# #260's networking::StreamClient and StreamServer, #256's empty mesh payload at
# MeshExchange's commit and #255's volume::kDefaultBucketSize /
# kDefaultMaxChain.
#
# #293 adds MeshExchange's CommitDecision, whose kDrop a commit hook's skip
# takes (frame_hooks.hpp). #294 changes none of the tiers the player links; its
# library asks for core 0.24.0. #295 makes volume::VoxelHashMap's
# record_compaction const, so an after_mesh hook, handed the grid read-only,
# records work that reads it; the worker finishes that batch before the grid
# grows (DECISIONS.md). #296 to #298 change only recon's camera and sensor tiers
# and its examples, which the player does not link; #298 raises recon's core
# floor to 0.25.0. #299 makes a Marching Cubes vertex on a corner whose distance
# is the iso-value exactly that corner, selected rather than interpolated (the
# division could leave it just short, or up to a voxel away under the
# near-tangent guard), and rounds each corner before the interpolation
# (`precise`).
FetchContent_Declare(
  volumetric_kit_recon
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_recon.git
  GIT_TAG d8b8bda181caf25619b086a36cd587a9c5fa0432)
FetchContent_MakeAvailable(volumetric_kit_recon)

# --- volumetric_kit_gfx ------------------------------------------------------
# The swapchain on a shared device, the mesh pipeline, the orbit camera, the
# controls and stats panels (ImGui) and the profiler that times the draw. Its
# tests, examples and -Werror stay off in a subproject. Its GLFW tier is built
# for player_desktop alone, so not on iOS: the library links no window system
# (the top-level CMakeLists.txt checks). The pin is gfx main at #160's merge
# (17b3192). What the player uses is as at #153's merge (123d013):
# ui::draw_stage_table's host, wait and GPU columns under a ui::StageWindow, and
# the frame loop's own wait, submit and present rows; beside them, #150's FIFO
# present by default, which the view takes, #148's
# ImGuiOverlayConfig::min_frame_height and ui::begin_collapsible_panel, #146's
# windowing::SurfaceSource and glfw::Library, #145's ImGuiTouch, #144's sRGB
# ImGui fix and #143's orbit drag gestures. Since #158,
# pipelines::HybridMeshTextureSet::create, which makes a set a program's
# before_draw binds, takes an allocator (HookContext::commit_allocator). The
# view's pipelines::HybridMeshDraw keeps #160's default, no second view, as the
# player's meshes carry none, so on Apple GPUs it draws at its cost before #159.
set(VG_WITH_GLFW ${VP_DESKTOP_APP})
FetchContent_Declare(
  volumetric_kit_gfx
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_gfx.git
  GIT_TAG 17b3192d17021d5085d2201c76c840c453d2a174)
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
