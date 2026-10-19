# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# An iOS app that links the library and is never run, until the iOS app takes
# its place, and links it without dead stripping (DECISIONS.md); CI builds it
# unsigned (ci.yml).
add_executable(vp_ios_link_check MACOSX_BUNDLE main.cpp)
target_link_libraries(vp_ios_link_check PRIVATE volumetric_kit::player)
vp_target_warnings(vp_ios_link_check)
set_target_properties(vp_ios_link_check
                      PROPERTIES XCODE_ATTRIBUTE_DEAD_CODE_STRIPPING NO)
