# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Third-party dependencies, fetched pinned (FetchContent): volumetric_kit_core,
# volumetric_kit_recon, volumetric_kit_gfx, and googletest when the tests are
# built. recon and gfx take Vulkan and GLM from the system, or on iOS as the
# core sets them up. Work on a sibling's checkout at once with
# -DFETCHCONTENT_SOURCE_DIR_VOLUMETRIC_KIT_<NAME>=<checkout>.

include(FetchContent)

# --- volumetric_kit_core -----------------------------------------------------
# The device, Status/Result and the GPU test fixtures, declared first, and for
# the desktop app the config tier, whose Player profile loads its scenario
# (DECISIONS.md). The pin is core main at #47's merge (f133d41, 0.17.0), which
# adds that profile; its iOS module, cmake/vkc_ios.cmake, links MoltenVK's iOS
# release (MoltenVK_ROOT) and answers glm. gfx's pin (7757c13, 0.12.0) and
# recon's (b992f12, 0.15.0) are older and yield to it, as the player declares
# the core first. The floor below is the pin's version (0.17.0), so an older
# FETCHCONTENT_SOURCE_DIR override is refused. The player installs nothing, so
# the core's install rules are off too.
set(VKC_WITH_VULKAN ON)
set(VKC_WITH_CONFIG ${VP_DESKTOP_APP})
set(VKC_INSTALL OFF)
if(VP_BUILD_TESTS)
  set(VKC_BUILD_TEST_SUPPORT ON)
endif()
FetchContent_Declare(
  volumetric_kit_core
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_core.git
  GIT_TAG f133d418dce30159bd002dc360c152bbdec4759a)
FetchContent_MakeAvailable(volumetric_kit_core)
set(_vp_core_tiers VULKAN)
if(VP_DESKTOP_APP)
  list(APPEND _vp_core_tiers CONFIG)
endif()
vkc_require_core(0.17.0 ${_vp_core_tiers})

# --- volumetric_kit_recon ----------------------------------------------------
# The stream reader, the codec's decode grid, Marching Cubes, the mesh exchange
# and the live stream's client. Its tests, examples, install and -Werror stay
# off in a subproject, so its library's core floor (0.2.0) is the one checked.
# The pin adds StreamClient::next's progress output (51ef686) to #260's merge
# (b6fa928): networking::StreamClient and StreamServer, beside #256's empty mesh
# payload at MeshExchange's commit, #255's volume::kDefaultBucketSize /
# kDefaultMaxChain and #253's codec::make_decode_grid and decode_growing.
FetchContent_Declare(
  volumetric_kit_recon
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_recon.git
  GIT_TAG 51ef686e72b0a0793c6bc23100e37267ee41b6a1)
FetchContent_MakeAvailable(volumetric_kit_recon)

# --- volumetric_kit_gfx ------------------------------------------------------
# The swapchain on a shared device, the mesh pipeline, the orbit camera, the
# controls and stats panels (ImGui) and the profiler that times the draw. Its
# tests, examples, install and -Werror stay off in a subproject. Its GLFW tier
# is built with the desktop app alone, so not on iOS: the library links no
# window system (the top-level CMakeLists.txt checks). The pin is gfx main at
# #148's merge (b35520f): ImGuiOverlayConfig's min_frame_height, and
# ui::begin_collapsible_panel and ui::draw_stage_table for the stats, beside
# #146's windowing::SurfaceSource and glfw::Library, #145's ImGuiTouch, #144's
# sRGB ImGui fix and #143's orbit drag gestures.
set(VG_WITH_GLFW ${VP_DESKTOP_APP})
FetchContent_Declare(
  volumetric_kit_gfx
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_gfx.git
  GIT_TAG b35520f5c6068e93bef0dccadd3ef1fa10fa9c6b)
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
