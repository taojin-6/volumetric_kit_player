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
# is core main at #49's merge (ff44a98, 0.19.0), gfx's pin too; its iOS module,
# cmake/vkc_ios.cmake, links MoltenVK's iOS release (MoltenVK_ROOT) and answers
# glm. recon's pin (7bb81da, 0.16.0) is older and yields to it, as the player
# declares the core first. The floor below is the oldest core the player and its
# siblings build with: gfx's 0.18.0, for Device::pipeline_cache (the Player
# profile is 0.17.0's), so an older FETCHCONTENT_SOURCE_DIR override is refused.
set(VKC_WITH_VULKAN ON)
set(VKC_WITH_CONFIG ON)
if(VP_BUILD_TESTS)
  set(VKC_BUILD_TEST_SUPPORT ON)
endif()
FetchContent_Declare(
  volumetric_kit_core
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_core.git
  GIT_TAG ff44a981055d505590e295ff6a943bb1b6e96109)
FetchContent_MakeAvailable(volumetric_kit_core)
vkc_require_core(0.18.0 VULKAN CONFIG)

# --- volumetric_kit_recon ----------------------------------------------------
# The stream reader, the codec's decode grid, Marching Cubes, the mesh exchange
# and the live stream's client. Its tests, examples and -Werror stay off in a
# subproject, so its library's core floor (0.2.0) is the one checked. The pin is
# recon #269's head (e9424a8), StreamClient::next's progress output on main at
# #270's merge (d7577df): #266's block ceiling for codec::make_decode_grid and
# decode_growing, beside #260's networking::StreamClient and StreamServer,
# #256's empty mesh payload at MeshExchange's commit and #255's
# volume::kDefaultBucketSize / kDefaultMaxChain. Main's #271 after it moves only
# recon's own core pin and docs.
#
# TODO: pin #269's merge once it lands on recon main.
FetchContent_Declare(
  volumetric_kit_recon
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_recon.git
  GIT_TAG e9424a8b2e0dce30e19b30fc973a3ff1a8c1b7e9)
FetchContent_MakeAvailable(volumetric_kit_recon)

# --- volumetric_kit_gfx ------------------------------------------------------
# The swapchain on a shared device, the mesh pipeline, the orbit camera, the
# controls and stats panels (ImGui) and the profiler that times the draw. Its
# tests, examples and -Werror stay off in a subproject. Its GLFW tier is built
# with the desktop app alone, so not on iOS: the library links no window system
# (the top-level CMakeLists.txt checks). The pin is gfx main at #152's merge
# (f147e00): #150's FIFO present by default, which the view takes, beside #148's
# ImGuiOverlayConfig::min_frame_height, ui::begin_collapsible_panel and
# ui::draw_stage_table for the stats, #146's windowing::SurfaceSource and
# glfw::Library, #145's ImGuiTouch, #144's sRGB ImGui fix and #143's orbit drag
# gestures.
set(VG_WITH_GLFW ${VP_DESKTOP_APP})
FetchContent_Declare(
  volumetric_kit_gfx
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_gfx.git
  GIT_TAG f147e00e7fb45382da8874198684656496f09b7c)
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
