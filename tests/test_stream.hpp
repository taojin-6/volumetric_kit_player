// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file test_stream.hpp
/// The tests' stream, written with recon's MeshIntegrator, Encoder and
/// StreamWriter: kFrames frames of a sphere moving a block a frame, at 25 fps
/// with a pause before the fourth, an I-frame every third. A stream that opens
/// but never decodes. And a scratch directory of the test's own to write them
/// into.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/recon/codec/encoder.hpp"
#include "volumetric_kit/recon/core/math/vector_types.hpp"
#include "volumetric_kit/recon/stream/stream_writer.hpp"
#include "volumetric_kit/recon/tsdf/mesh_integrator.hpp"
#include "volumetric_kit/recon/volume/voxel_block_grid.hpp"

namespace vp_test {

namespace core = volumetric_kit::core;
namespace recon = volumetric_kit::recon;

constexpr std::uint64_t kFrames = 6;
constexpr std::uint32_t kIntraPeriod = 3;
constexpr std::uint64_t kPeriodNs = 40'000'000;  // 25 fps
// The pause before frame kGapAt, so a pts is not its index times the period.
constexpr std::uint64_t kGapAt = 3;
constexpr std::uint64_t kGapNs = 500'000'000;
constexpr float kVoxel = 0.01f;
constexpr float kTrunc = 0.04f;
constexpr float kRadius = 0.12f;

// Frame i's pts.
inline std::uint64_t pts_of(std::uint64_t i) {
  return i * kPeriodNs + (i >= kGapAt ? kGapNs : 0);
}

// A directory of the test's own, removed with what it holds.
class Scratch {
 public:
  Scratch() {
    const ::testing::TestInfo* test =
        ::testing::UnitTest::GetInstance()->current_test_info();
    path_ =
        std::filesystem::temp_directory_path() /
        ("vp_" + std::string(test->name()) + "_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_);
  }
  ~Scratch() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;

  std::string file(const char* name) const { return (path_ / name).string(); }

 private:
  std::filesystem::path path_;
};

struct HostMesh {
  std::vector<recon::Vec3f> vertices;
  std::vector<std::uint32_t> indices;
};

// A UV sphere of kRadius round `centre`, wound outward, its edges about 1.6
// voxels long.
inline HostMesh sphere(const recon::Vec3f& centre) {
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

// The stream: kFrames frames of the sphere moving a block (8 voxels) a frame
// along x, at pts_of, an I-frame every kIntraPeriod, each record a key
// exactly when its frame is an I-frame.
inline core::Status write_stream(core::Device& device,
                                 core::Allocator& allocator,
                                 const std::string& path) {
  namespace stream = recon::stream;
  namespace vol = recon::volume;
  constexpr std::int32_t kBuckets = 1024;
  const vol::VoxelGridParams params{kVoxel,
                                    8,
                                    512,
                                    kTrunc,
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
  config.intra_period = kIntraPeriod;
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
    const HostMesh mesh =
        sphere(recon::Vec3f(8.0f * kVoxel * static_cast<float>(i), 0, 0));
    const auto vertex_count = static_cast<std::uint32_t>(mesh.vertices.size());
    const auto triangle_count =
        static_cast<std::uint32_t>(mesh.indices.size() / 3);
    VKC_TRY(grid.clear());
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
    VKC_ASSIGN(const std::vector<std::uint8_t> frame, encoder.encode(grid));
    VKC_ASSIGN(const recon::codec::FrameInfo info,
               recon::codec::read_frame_info(frame.data(), frame.size()));
    stream::RecordInfo record;
    record.key = info.type == recon::codec::FrameType::kIntra;
    record.pts_ns = pts_of(i);
    record.seq = i;
    VKC_TRY(writer->append(record, frame.data(), frame.size()));
  }
  return writer->finish();
}

// A stream that opens, with no device, but never decodes: a 25 fps geometry
// track of `records` one-byte key records.
inline core::Status write_undecodable_stream(const std::string& path,
                                             std::uint64_t records) {
  namespace stream = recon::stream;
  stream::Session session;
  session.writer = "volumetric_kit_player tests";
  stream::Track track;
  track.kind = stream::TrackKind::Geometry;
  track.rate = {25, 1};
  session.tracks.push_back(track);
  VKC_ASSIGN(std::unique_ptr<stream::StreamWriter> writer,
             stream::StreamWriter::create(path, session));
  const std::uint8_t byte = 0;
  for (std::uint64_t i = 0; i < records; ++i) {
    stream::RecordInfo record;
    record.key = true;
    record.pts_ns = pts_of(i);
    record.seq = i;
    VKC_TRY(writer->append(record, &byte, 1));
  }
  return writer->finish();
}

}  // namespace vp_test
