// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// mesh_wiring on the host: the draw's usage on the extractor's buffers, a ring
// two slots deeper than the frames in flight, one frame counted as two, and
// one more for a commit hook that may hold or skip a mesh, vertices shared
// unless asked not to be, and the families: one family is EXCLUSIVE and read
// in place, two are CONCURRENT and read across. And drawable on grid's
// exchange: a refused mesh fails every frame after it.

#include "volumetric_kit/player/mesh_wiring.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <set>
#include <string>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"
#include "volumetric_kit/grid/mesh/device_mesh.hpp"
#include "volumetric_kit/grid/mesh/mesh_exchange.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace gmesh = volumetric_kit::grid::mesh;
using volumetric_kit::player::drawable;
using volumetric_kit::player::mesh_wiring;
using volumetric_kit::player::MeshWiring;

// A handle the host never dereferences.
VkBuffer fake_buffer(std::uint64_t value) {
  static_assert(sizeof(VkBuffer) == sizeof(value), "64-bit handles");
  VkBuffer buffer = VK_NULL_HANDLE;
  std::memcpy(&buffer, &value, sizeof(buffer));
  return buffer;
}

// A drawable extract of one triangle at `generation`, shared as `sharing`.
gmesh::DeviceMesh one_triangle(std::uint64_t generation,
                               VkSharingMode sharing) {
  gmesh::DeviceMesh mesh;
  mesh.vertices = fake_buffer(4 * generation + 1);
  mesh.indices = fake_buffer(4 * generation + 2);
  mesh.indirect = fake_buffer(4 * generation + 3);
  mesh.vertex_count = 3;
  mesh.triangle_count = 1;
  mesh.vertex_usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  mesh.index_usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  mesh.indirect_usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  mesh.sharing_mode = sharing;
  mesh.generation = generation;
  return mesh;
}

// The distinct families the extractor's buffers name: one makes them
// EXCLUSIVE, more CONCURRENT (core::BufferDesc::queue_families).
std::set<std::uint32_t> families(const MeshWiring& wiring) {
  return {wiring.mc.queue_families,
          wiring.mc.queue_families + wiring.mc.queue_family_count};
}

// The extractor's buffers carry every usage the exchange binds them with.
void expect_bindable(const MeshWiring& wiring) {
  EXPECT_EQ(wiring.mc.extra_vertex_usage, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  EXPECT_EQ(wiring.mc.extra_index_usage, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
  EXPECT_EQ(wiring.mc.extra_vertex_usage & wiring.exchange.vertex_usage,
            wiring.exchange.vertex_usage);
  EXPECT_EQ(wiring.mc.extra_index_usage & wiring.exchange.index_usage,
            wiring.exchange.index_usage);
  // The command carries INDIRECT_BUFFER unconditionally.
  EXPECT_EQ(wiring.exchange.indirect_usage,
            VkBufferUsageFlags{VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT});
}

TEST(MeshWiring, OneFamilyIsReadInPlace) {
  const MeshWiring wiring = mesh_wiring(/*graphics_family=*/3,
                                        /*compute_family=*/3,
                                        /*frames_in_flight=*/2);
  EXPECT_EQ(families(wiring), std::set<std::uint32_t>{3});
  EXPECT_FALSE(wiring.exchange.cross_family);
  EXPECT_EQ(wiring.exchange.frames_in_flight, 2u);
  EXPECT_EQ(wiring.mc.slot_count, 4u);
  expect_bindable(wiring);
}

TEST(MeshWiring, TwoFamiliesAreReadAcross) {
  const MeshWiring wiring = mesh_wiring(/*graphics_family=*/0,
                                        /*compute_family=*/1,
                                        /*frames_in_flight=*/3);
  EXPECT_EQ(families(wiring), (std::set<std::uint32_t>{0, 1}));
  EXPECT_TRUE(wiring.exchange.cross_family);
  EXPECT_EQ(wiring.exchange.frames_in_flight, 3u);
  EXPECT_EQ(wiring.mc.slot_count, 5u);
  expect_bindable(wiring);
}

// grid's ring rule: the frames in flight, one counted as two, two more for a
// producer that meshes the next frame before the take, and with `holds` one
// more for the mesh a commit hook may skip; the rest is as without.
TEST(MeshWiring, HoldsAddOneMeshAndOneFrameCountsAsTwo) {
  EXPECT_EQ(mesh_wiring(0, 0, 1).mc.slot_count, 4u);
  EXPECT_EQ(mesh_wiring(0, 0, 2).mc.slot_count, 4u);
  for (const std::uint32_t frames : {1u, 2u, 3u}) {
    const MeshWiring plain = mesh_wiring(0, 1, frames);
    const MeshWiring holding =
        mesh_wiring(0, 1, frames, /*share_vertices=*/true, /*holds=*/true);
    EXPECT_EQ(holding.mc.slot_count, plain.mc.slot_count + 1) << frames;
    EXPECT_EQ(families(holding), families(plain));
    EXPECT_EQ(holding.exchange.frames_in_flight, frames);
    EXPECT_TRUE(holding.exchange.cross_family);
    expect_bindable(holding);
  }
}

// The draw is indexed, so triangles share their vertices by default: about a
// third of the vertices to hold, write and fetch. Asked, they share none, and
// the rest is as shared.
TEST(MeshWiring, VerticesAreSharedUnlessAskedNotTo) {
  EXPECT_TRUE(mesh_wiring(0, 0, 2).mc.share_vertices);
  EXPECT_TRUE(mesh_wiring(0, 1, 2).mc.share_vertices);
  EXPECT_TRUE(mesh_wiring(0, 1, 2, /*share_vertices=*/true).mc.share_vertices);
  for (const std::uint32_t compute : {0u, 1u}) {
    const MeshWiring shared = mesh_wiring(0, compute, 2);
    const MeshWiring own = mesh_wiring(0, compute, 2, /*share_vertices=*/false);
    EXPECT_FALSE(own.mc.share_vertices);
    EXPECT_EQ(families(own), families(shared));
    EXPECT_EQ(own.mc.slot_count, shared.mc.slot_count);
    EXPECT_EQ(own.exchange.cross_family, shared.exchange.cross_family);
    EXPECT_EQ(own.exchange.frames_in_flight, shared.exchange.frames_in_flight);
    expect_bindable(own);
  }
}

TEST(MeshWiring, ARefusedMeshFailsEveryLaterFrame) {
  const MeshWiring wiring = mesh_wiring(/*graphics_family=*/0,
                                        /*compute_family=*/1,
                                        /*frames_in_flight=*/2);
  gmesh::MeshExchange<int> exchange(wiring.exchange);
  const auto commit = [](const gmesh::DeviceMesh&, int&) { return true; };
  EXPECT_TRUE(drawable(exchange).ok());
  // EXCLUSIVE buffers cannot be read across families.
  exchange.publish(one_triangle(1, VK_SHARING_MODE_EXCLUSIVE), 1);
  ASSERT_EQ(exchange.begin_frame(0, commit), gmesh::ExchangeOutcome::kRefused);
  const core::Status refused = drawable(exchange);
  EXPECT_EQ(refused.domain(), core::Status::Code::Unsupported);
  EXPECT_NE(refused.message().find("EXCLUSIVE"), std::string::npos)
      << refused.message();
  // The next frame takes nothing, even a drawable mesh, and still fails.
  exchange.publish(one_triangle(2, VK_SHARING_MODE_CONCURRENT), 2);
  for (std::uint32_t slot : {1u, 0u}) {
    ASSERT_EQ(exchange.begin_frame(slot, commit),
              gmesh::ExchangeOutcome::kNone);
    const core::Status later = drawable(exchange);
    EXPECT_EQ(later.domain(), core::Status::Code::Unsupported);
    EXPECT_EQ(later.message(), refused.message());
  }
}

}  // namespace
