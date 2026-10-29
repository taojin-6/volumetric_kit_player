// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file program_requirements.hpp
/// A program's own device requirements (PlayerViewConfig::device_requirements)
/// for the tests: an extension no device offers, and one the GPU offers that
/// nothing the player asks for enables (grid's, gfx's or the core's own).

#include <string>

#include "volumetric_kit/core/vulkan/physical_device_info.hpp"

namespace vp_test {

/// An extension no device offers: a program's requirement the device lacks.
inline constexpr const char* kAbsentExtension = "VK_VP_test_absent_extension";

/// @return The first of a few common extensions that @p physical offers and
///         that the player's device never enables unless a program asks for
///         it; empty when it offers none of them.
inline std::string offered_extension(
    const volumetric_kit::core::PhysicalDeviceInfo& physical) {
  for (const char* name :
       {"VK_KHR_shader_non_semantic_info", "VK_KHR_driver_properties"}) {
    if (physical.supports_device_extension(name)) {
      return name;
    }
  }
  return {};
}

}  // namespace vp_test
