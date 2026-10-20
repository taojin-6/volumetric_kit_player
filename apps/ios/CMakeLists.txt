# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# The iOS app: a UIKit shell over the player library, in Objective-C++ under ARC
# (DECISIONS.md). It names the frameworks the shell uses; core_vulkan brings
# MoltenVK's. With no team it builds unsigned, as CI does (ci.yml); README.md
# says how to sign it for a device.
enable_language(OBJCXX)

set(VP_DEVELOPMENT_TEAM
    ""
    CACHE STRING "Apple team ID that signs the iOS app; empty: unsigned")
set(VP_BUNDLE_ID
    "io.taojin.volumetrickit.player"
    CACHE STRING "The iOS app's bundle identifier")

set(VP_IOS_APP volumetric_kit_player)
add_executable(${VP_IOS_APP} MACOSX_BUNDLE main.mm PlayerViewController.mm)
target_link_libraries(
  ${VP_IOS_APP} PRIVATE volumetric_kit::player "-framework Foundation"
                        "-framework QuartzCore" "-framework UIKit")
vp_target_warnings(${VP_IOS_APP})

# The device family is a build setting: Xcode rewrites the plist's key from it,
# and an iPad letterboxes an iPhone-only app's layer. The entitlement raises the
# memory limit past which iOS kills the app with no Status.
configure_file(Info.plist.in "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)
set_target_properties(
  ${VP_IOS_APP}
  PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
             XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${VP_BUNDLE_ID}"
             XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
             XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
             XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS
             "${CMAKE_CURRENT_SOURCE_DIR}/volumetric_kit_player.entitlements")
if(VP_DEVELOPMENT_TEAM)
  set_target_properties(
    ${VP_IOS_APP}
    PROPERTIES XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${VP_DEVELOPMENT_TEAM}"
               XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic)
else()
  set_target_properties(${VP_IOS_APP}
                        PROPERTIES XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO)
endif()
