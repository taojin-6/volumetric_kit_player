// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// vp_write_stream <dir>: writes the tests' streams (test_stream.hpp) into
// <dir>, with recon's MeshIntegrator, Encoder and StreamWriter, on the first
// device that meets recon's requirements: the ctest vp_test_streams, once a
// run (CMakeLists.txt). It applies the core's test policy as a main() of its
// own (vulkan_policy.hpp): with no device a skip, or a failure under
// VKC_REQUIRE_VULKAN_DEVICE, and under VKC_TEST_VALIDATION any error the
// layer reports fails it.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "test_stream.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/testing/vulkan_policy.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"
#include "volumetric_kit/recon/codec/decoder.hpp"
#include "volumetric_kit/recon/codec/encoder.hpp"
#include "volumetric_kit/recon/core/device_requirements.hpp"
#include "volumetric_kit/recon/core/math/vector_types.hpp"
#include "volumetric_kit/recon/stream/stream_writer.hpp"
#include "volumetric_kit/recon/tsdf/mesh_integrator.hpp"
#include "volumetric_kit/recon/volume/voxel_block_grid.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace recon = volumetric_kit::recon;
using vp_test::kFrames;

constexpr float kVoxel = 0.01f;
constexpr float kTrunc = 0.04f;
constexpr float kRadius = 0.12f;
// The true-SDF stream's truncation: 16 voxels, past recon's
// kTrueSdfBandVoxels (15), so every coded voxel is observed.
constexpr float kTrueSdfTrunc = 0.16f;

struct HostMesh {
  std::vector<recon::Vec3f> vertices;
  std::vector<std::uint32_t> indices;
};

// A UV sphere of kRadius round `centre`, wound outward, its edges about 1.6
// voxels long.
HostMesh sphere(const recon::Vec3f& centre) {
  constexpr std::uint32_t kStacks = 24;
  constexpr std::uint32_t kSlices = 48;
  constexpr float kPi = 3.14159265f;
  HostMesh mesh;
  for (std::uint32_t i = 0; i <= kStacks; ++i) {
    const float phi = kPi * static_cast<float>(i) / kStacks;
    for (std::uint32_t j = 0; j < kSlices; ++j) {
      const float theta = 2.0f * kPi * static_cast<float>(j) / kSlices;
      mesh.vertices.push_back(
          centre + kRadius * recon::Vec3f(std::sin(phi) * std::cos(theta),
                                          std::cos(phi),
                                          std::sin(phi) * std::sin(theta)));
    }
  }
  for (std::uint32_t i = 0; i < kStacks; ++i) {
    for (std::uint32_t j = 0; j < kSlices; ++j) {
      const std::uint32_t a = i * kSlices + j;
      const std::uint32_t b = i * kSlices + (j + 1) % kSlices;
      mesh.indices.insert(mesh.indices.end(),
                          {a, b, a + kSlices, b, b + kSlices, a + kSlices});
    }
  }
  return mesh;
}

// A stream: kFrames frames of the sphere moving a block (8 voxels) a frame
// along x, at pts_of, an I-frame every kIntraPeriod, each record a key
// exactly when its frame is an I-frame, at `voxel` metres a voxel and
// `trunc` metres of truncation, coded as a true SDF when `true_sdf`
// (recon's EncoderConfig::true_sdf). Frame `empty`, if given, holds no
// block, so it meshes nothing: an I-frame of no blocks. It replaces a file
// at `path`, such as the last ctest run's, which StreamWriter would refuse.
core::Status write_stream(core::Device& device, core::Allocator& allocator,
                          const std::string& path,
                          std::optional<std::uint64_t> empty, float voxel,
                          float trunc = kTrunc, bool true_sdf = false) {
  std::error_code ec;
  std::filesystem::remove(path, ec);  // StreamWriter says why if it is left
  namespace stream = recon::stream;
  namespace vol = recon::volume;
  constexpr std::int32_t kBuckets = 1024;
  const vol::VoxelGridParams params{voxel,
                                    8,
                                    512,
                                    trunc,
                                    vol::kDefaultBucketSize,
                                    kBuckets,
                                    vol::kDefaultBucketSize * kBuckets,
                                    vol::kDefaultMaxChain};
  const vol::AttributeSpec attrs[] = {{"tsdf", sizeof(float)},
                                      {"weight", sizeof(float)}};
  VKC_ASSIGN(vol::VoxelBlockGrid grid,
             vol::VoxelBlockGrid::create(device, allocator, params, attrs, 2));
  VKC_ASSIGN(recon::tsdf::MeshIntegrator integrator,
             recon::tsdf::MeshIntegrator::create(device, allocator));
  recon::codec::EncoderConfig config;
  config.intra_period = vp_test::kIntraPeriod;
  config.true_sdf = true_sdf;
  VKC_ASSIGN(recon::codec::Encoder encoder,
             recon::codec::Encoder::create(device, allocator, config));
  stream::Session session;
  session.writer = "volumetric_kit_player tests";
  stream::Track track;
  track.kind = stream::TrackKind::Geometry;
  track.rate = {25, 1};
  session.tracks.push_back(track);
  VKC_ASSIGN(std::unique_ptr<stream::StreamWriter> writer,
             stream::StreamWriter::create(path, session));
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    VKC_TRY(grid.clear());
    if (i != empty) {
      const HostMesh mesh =
          sphere(recon::Vec3f(8.0f * voxel * static_cast<float>(i), 0, 0));
      const auto vertex_count =
          static_cast<std::uint32_t>(mesh.vertices.size());
      const auto triangle_count =
          static_cast<std::uint32_t>(mesh.indices.size() / 3);
      VKC_ASSIGN(const std::uint32_t failed,
                 grid.map().allocate_from_triangles(
                     mesh.vertices.data(), vertex_count, mesh.indices.data(),
                     triangle_count));
      if (failed != 0) {
        return core::Status::out_of_memory("the sphere outgrew the grid");
      }
      VKC_TRY(integrator
                  .integrate(grid, mesh.vertices.data(), vertex_count,
                             mesh.indices.data(), triangle_count)
                  .status());
    }
    VKC_ASSIGN(const std::vector<std::uint8_t> frame, encoder.encode(grid));
    VKC_ASSIGN(const recon::codec::FrameInfo info,
               recon::codec::read_frame_info(frame.data(), frame.size()));
    stream::RecordInfo record;
    record.key = info.type == recon::codec::FrameType::kIntra;
    record.pts_ns = vp_test::pts_of(i);
    record.seq = i;
    VKC_TRY(writer->append(record, frame.data(), frame.size()));
  }
  return writer->finish();
}

// The streams into `dir`, on a device of `gpu`'s.
core::Status write(const core::Instance& instance,
                   const core::PhysicalDeviceInfo& gpu,
                   const std::filesystem::path& dir) {
  VKC_TRY(core::test::check_layer_loaded(instance));
  VKC_ASSIGN(core::Device device,
             core::Device::create(instance, gpu, recon::device_requirements()));
  VKC_ASSIGN(core::Allocator allocator,
             core::Allocator::create(instance.handle(), device));
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return core::Status::io_error("cannot make " + dir.string() + ": " +
                                  ec.message());
  }
  VKC_TRY(write_stream(device, allocator, (dir / vp_test::kStream).string(),
                       std::nullopt, kVoxel));
  VKC_TRY(write_stream(device, allocator,
                       (dir / vp_test::kEmptyFrameStream).string(),
                       vp_test::kEmptyFrame, kVoxel));
  VKC_TRY(write_stream(device, allocator,
                       (dir / vp_test::kTrueSdfStream).string(), std::nullopt,
                       kVoxel, kTrueSdfTrunc, /*true_sdf=*/true));
  return write_stream(device, allocator,
                      (dir / vp_test::kCoarseStream).string(), std::nullopt,
                      2 * kVoxel);
}

// The streams into `dir`, everything made on the device destroyed before this
// returns, while main's capture still counts what the layer reports.
int run(const char* dir) {
  const core::Result<core::Instance> instance =
      core::Instance::create(core::test::instance_config());
  if (!instance.ok()) {
    return core::test::no_device_exit_code(instance.status().message());
  }
  const core::Result<core::PhysicalDeviceInfo> gpu =
      instance->select_physical_device(recon::device_requirements());
  if (!gpu.ok()) {
    return core::test::no_device_exit_code(gpu.status().message());
  }
  const core::Status written = write(*instance, *gpu, dir);
  if (!written.ok()) {
    const std::string_view domain = core::to_string(written.domain());
    std::fprintf(stderr, "vp_write_stream: %.*s: %s\n",
                 static_cast<int>(domain.size()), domain.data(),
                 written.message().c_str());
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fputs("usage: vp_write_stream <dir>\n", stderr);
    return 2;
  }
  const core::test::ValidationSession validation;
  const core::test::LogCapture log;
  return log.exit_code(run(argv[1]));
}
