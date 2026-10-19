// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// vp_write_stream <path>: writes the tests' stream (test_stream.hpp) on the
// first device that meets recon's requirements, for CI's smoke run of the
// window (.github/workflows/ci.yml). A tool, not a test.

#include <cstdio>
#include <string>
#include <string_view>

// test_stream.hpp reads frame info, which the tests have from
// stream_player.hpp.
#include "volumetric_kit/recon/codec/decoder.hpp"
//
#include "test_stream.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"
#include "volumetric_kit/recon/core/device_requirements.hpp"

namespace {

namespace core = volumetric_kit::core;

core::Status write(const std::string& path) {
  VKC_ASSIGN(const core::Instance instance, core::Instance::create({}));
  const core::DeviceRequirements requirements =
      volumetric_kit::recon::device_requirements();
  VKC_ASSIGN(const core::PhysicalDeviceInfo gpu,
             instance.select_physical_device(requirements));
  VKC_ASSIGN(core::Device device,
             core::Device::create(instance, gpu, requirements));
  VKC_ASSIGN(core::Allocator allocator,
             core::Allocator::create(instance.handle(), device));
  return vp_test::write_stream(device, allocator, path);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fputs("usage: vp_write_stream <path>\n", stderr);
    return 2;
  }
  const core::Status written = write(argv[1]);
  if (!written.ok()) {
    const std::string_view domain = core::to_string(written.domain());
    std::fprintf(stderr, "vp_write_stream: %.*s: %s\n",
                 static_cast<int>(domain.size()), domain.data(),
                 written.message().c_str());
    return 1;
  }
  return 0;
}
