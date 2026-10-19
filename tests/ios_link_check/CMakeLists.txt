# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# An iOS app that links the library and is never run, until the iOS app takes
# its place (DECISIONS.md). Only an app's link shows what an iOS link misses,
# such as MoltenVK's frameworks or a macOS glm; CI builds it unsigned and checks
# that it is an iOS binary (ci.yml).
add_executable(vp_ios_link_check MACOSX_BUNDLE main.cpp)
target_link_libraries(vp_ios_link_check PRIVATE volumetric_kit::player)
vp_target_warnings(vp_ios_link_check)
