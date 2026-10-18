# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# Third-party dependencies, fetched pinned (FetchContent): volumetric_kit_core,
# volumetric_kit_recon, and googletest when the tests are built. recon takes
# Vulkan and GLM from the system. Work on a sibling's checkout at once with
# -DFETCHCONTENT_SOURCE_DIR_VOLUMETRIC_KIT_<NAME>=<checkout>.

include(FetchContent)

# --- volumetric_kit_core -----------------------------------------------------
# The device, Status/Result and the GPU test fixtures. Declared first, so
# FetchContent keeps this pin for recon too (the core's DECISIONS.md, "Consumers
# pin, and an application declares the core first"). The pin is the one recon's
# own pin below names: core 0.10.0 (7a1219e, core #36's merge).
set(VKC_WITH_VULKAN ON)
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
