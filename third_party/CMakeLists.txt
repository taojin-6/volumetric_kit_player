# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Third-party dependencies, fetched pinned (FetchContent): volumetric_kit_core,
# volumetric_kit_grid, volumetric_kit_gfx, and googletest when the tests are
# built. grid and gfx take Vulkan and GLM from the system, or on iOS as the core
# sets them up. Work on a sibling's checkout at once with
# -DFETCHCONTENT_SOURCE_DIR_VOLUMETRIC_KIT_<NAME>=<checkout>.

include(FetchContent)

# --- volumetric_kit_core -----------------------------------------------------
# The device, Status/Result and the GPU test fixtures, declared first, and the
# config tier: its Source, which the library's open_source takes on every
# platform, and the Player profile the desktop app loads (DECISIONS.md). The pin
# is core main at PR #70's merge (2120e62, 0.30.0), grid's own pin and the floor
# grid's library asks for. #68 to #70 add CI's trigger check, which the lint job
# takes at this pin, and FuseOrbbec's output.max_viewers; #65 to #67 add
# vulkan/fwd.hpp, test support for tests that are main()s of their own,
# vkc_export_glsl and vkc_require_sibling, which checks grid below; the player's
# code uses none of the rest. What it uses is as at #52's merge (0.21.0):
# HostRing, whose slots grid's decoder and maps keep, in place where
# device-mapped memory is cached and copied elsewhere; CommandBatch::input and
# output; and DeviceCounters::dispatches and barriers, which the stamps and the
# measure log carry. Under it, 0.20.0's rows with busy, wait and device halves
# (core::DeviceTime), tiled device time, PendingBatch::device_rows and
# device_span, GpuTimer::set_max_depth and Device::counters and calibrate. Its
# iOS module, cmake/vkc_ios.cmake, links MoltenVK's iOS release (MoltenVK_ROOT)
# and answers glm. The floor below is the pin's version, so an older
# FETCHCONTENT_SOURCE_DIR override is refused.
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
  GIT_TAG 2120e6294e3c7ead8bb168d757f940b21d21267c)
FetchContent_MakeAvailable(volumetric_kit_core)
vkc_require_core(0.30.0 VULKAN CONFIG)

# --- volumetric_kit_grid -----------------------------------------------------
# The stream reader, the codec's decode grid and its deblock, Marching Cubes,
# the mesh exchange, the live stream's client, and for vp_probe's --save-meshes
# and the tests' stream writer the PLY writer and the mesh integrator. Its tests
# and examples are off, as is its -Werror in a subproject, so its library's core
# floor is the one checked; its own core pin is the player's. The pin is grid
# main at #11's merge (cf7c99a, 0.6.0), whose StreamServer serves every track of
# a session, track 0's keys starting each GOP, so a sender serves its Audio
# track beside the geometry (live_audio.hpp); #10 (0.5.0) muxes a CodecStream's
# Audio tracks by pts, so each record rides right after the first frame at or
# past its pts. #9's merge (0.4.0) brought TrackKind::Audio and
# stream/audio.hpp, the Audio track the player plays (audio_feeder.hpp). Since
# its first commits (cbf7a6e), recon #309's grid/ under grid's names
# (DECISIONS.md, 2026-10-29), it has also brought marching-cubes vertex normals
# from the field's gradient (0.2.1), StreamServer::viewers (0.2.0), the core
# floor 0.30.0 (0.3.0) and an Encoder that holds P-frames a quantization step
# against their reference (0.3.1), which the tests' streams are written with.
# What the player uses besides is as at recon #277's merge: a frame decoded and
# meshed in the caller's batch (codec::record_growing, finish_growing;
# mesh::MarchingCubes::record_extract, finish, abandon), DeviceMesh::ready,
# Decoder::claim_input and last_rounds, and the reads straight into caller
# memory (StreamReader::read, networking::StreamClient::read); and since, recon
# #293's CommitDecision, whose kDrop a commit hook's skip takes
# (frame_hooks.hpp), #295's const volume::VoxelHashMap::record_compaction, so an
# after_mesh hook, handed the grid read-only, records work that reads it, and
# #299's Marching Cubes vertex on a corner whose distance is the iso-value
# exactly that corner. vkc_require_sibling refuses an older grid, or one without
# a tier the player links.
set(VGRID_BUILD_TESTS OFF)
set(VGRID_BUILD_EXAMPLES OFF)
FetchContent_Declare(
  volumetric_kit_grid
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_grid.git
  GIT_TAG cf7c99a0ca74839eb5c2d1afb5f58132cdf636b4)
FetchContent_MakeAvailable(volumetric_kit_grid)
vkc_require_sibling(
  volumetric_kit::grid
  0.6.0
  TARGETS
  volumetric_kit::grid_codec
  volumetric_kit::grid_mesh
  volumetric_kit::grid_mesh_io
  volumetric_kit::grid_networking
  volumetric_kit::grid_stream
  volumetric_kit::grid_voxelize)

# --- volumetric_kit_gfx ------------------------------------------------------
# The swapchain on a shared device, the mesh pipeline, the orbit camera, the
# controls and stats panels (ImGui) and the profiler that times the draw. Its
# tests, examples and -Werror stay off in a subproject. Its GLFW tier is built
# for player_desktop alone, so not on iOS: the library links no window system
# (the top-level CMakeLists.txt checks). The pin is gfx main at #168's merge
# (df3a0de), whose windows render at 4x MSAA (#167, #168): the swapchain's
# layout().samples is 4, so the view's pipelines draw multisampled, and the
# tests' offscreen targets take the view's layout with offscreen_desc. Before
# it, #166's ui::DockedPanel, which lays out a program's panel and draws its
# handle and header, on main at #165's merge (928146c), for
# HybridMeshFrame::viewport, which draws the mesh in the scene's part. What the
# player uses besides is as at #153's merge (123d013): ui::draw_stage_table's
# host, wait and GPU columns under a ui::StageWindow, and the frame loop's own
# wait, submit and present rows; beside them, #150's FIFO present by default,
# which the view takes, #148's ImGuiOverlayConfig::min_frame_height and
# ui::begin_collapsible_panel, #146's windowing::SurfaceSource and
# glfw::Library, #145's ImGuiTouch, #144's sRGB ImGui fix and #143's orbit drag
# gestures. Since #158, pipelines::HybridMeshTextureSet::create, which makes a
# set a program's before_draw binds, takes an allocator
# (HookContext::commit_allocator). The view's pipelines::HybridMeshDraw keeps
# #160's default, no second view, as the player's meshes carry none, so on Apple
# GPUs it draws at its cost before #159.
set(VG_WITH_GLFW ${VP_DESKTOP_APP})
FetchContent_Declare(
  volumetric_kit_gfx
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_gfx.git
  GIT_TAG df3a0de4184f6b989faa8d8ffbe0a338e00642a5)
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
