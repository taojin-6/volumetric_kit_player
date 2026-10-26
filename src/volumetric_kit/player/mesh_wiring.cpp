// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/mesh_wiring.hpp"

#include <algorithm>

#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::player {

MeshWiring mesh_wiring(std::uint32_t graphics_family,
                       std::uint32_t compute_family,
                       std::uint32_t frames_in_flight, bool share_vertices,
                       bool holds) {
  MeshWiring wiring;
  recon::mesh::MarchingCubesConfig& mc = wiring.mc;
  mc.extra_vertex_usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  mc.extra_index_usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  mc.queue_families[0] = compute_family;
  mc.queue_families[1] = graphics_family;
  mc.queue_family_count = 2;
  // Each frame in flight holds a generation, one published may wait to be
  // taken, and the next is meshed in its frame's batch before that take. One
  // frame counts as two: the mesh a commit replaces keeps its slot until the
  // committing frame retires (recon's slot_count). A skipped mesh keeps its
  // slot until the next is committed, and FrameTaker skips one at most.
  mc.slot_count = std::max(frames_in_flight, 2u) + 2 + (holds ? 1 : 0);
  // Shared, about a third of the vertices to hold, write and fetch; unshared,
  // a cheaper extraction (DECISIONS.md). The draw is indexed, so it takes
  // either.
  mc.share_vertices = share_vertices;
  wiring.exchange.frames_in_flight = frames_in_flight;
  wiring.exchange.cross_family = graphics_family != compute_family;
  return wiring;
}

}  // namespace volumetric_kit::player
