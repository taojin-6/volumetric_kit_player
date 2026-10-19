// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// mesh_wiring on the host: the draw's usage on the extractor's buffers, a ring
// one slot deeper than the frames in flight, and the families: one family is
// EXCLUSIVE and read in place, two are CONCURRENT and read across.

#include "volumetric_kit/player/mesh_wiring.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>

#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace {

using volumetric_kit::player::mesh_wiring;
using volumetric_kit::player::MeshWiring;

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
  EXPECT_EQ(wiring.mc.slot_count, 3u);
  expect_bindable(wiring);
}

TEST(MeshWiring, TwoFamiliesAreReadAcross) {
  const MeshWiring wiring = mesh_wiring(/*graphics_family=*/0,
                                        /*compute_family=*/1,
                                        /*frames_in_flight=*/3);
  EXPECT_EQ(families(wiring), (std::set<std::uint32_t>{0, 1}));
  EXPECT_TRUE(wiring.exchange.cross_family);
  EXPECT_EQ(wiring.exchange.frames_in_flight, 3u);
  EXPECT_EQ(wiring.mc.slot_count, 4u);
  expect_bindable(wiring);
}

}  // namespace
