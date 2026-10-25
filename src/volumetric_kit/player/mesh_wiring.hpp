// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file mesh_wiring.hpp
/// @brief What the renderer needs of recon's meshes: the extractor's and the
///        exchange's configs for one shared device, and whether the exchange
///        still hands over meshes it can draw. Player-internal.

#include <cstdint>
#include <string>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/recon/mesh/marching_cubes.hpp"
#include "volumetric_kit/recon/mesh/mesh_exchange.hpp"

namespace volumetric_kit::player {

/// @brief The extractor's config and the exchange's, which must agree.
struct MeshWiring {
  /// The extractor's: the draw's usage, both families, a ring two slots
  /// deeper than the renderer's frames in flight (one published and not yet
  /// taken, one meshed in the next frame's batch), and shared vertices.
  recon::mesh::MarchingCubesConfig mc;
  /// The exchange's: the renderer's frames in flight, and whether it reads
  /// from another family than the extractor writes on.
  recon::mesh::MeshExchangeConfig exchange;
};

/// @brief Wire recon's meshes to a renderer that draws them in place.
///
/// The buffers carry `VERTEX_BUFFER` and `INDEX_BUFFER` usage beside the
/// storage recon writes through. Both families are named unconditionally, so
/// the buffers are `CONCURRENT` across two families and `EXCLUSIVE` on one.
///
/// @code
/// const MeshWiring wiring = mesh_wiring(shared.graphics_family(),
///                                       shared.compute_family(), 2);
/// FrameExchange exchange(wiring.exchange);
/// VKC_TRY(player.start(device, allocator, wiring.mc, exchange));
/// @endcode
/// @param graphics_family   The renderer's queue family.
/// @param compute_family    The extractor's queue family.
/// @param frames_in_flight  The renderer's frames in flight; at least 1.
/// @return The two configs.
MeshWiring mesh_wiring(std::uint32_t graphics_family,
                       std::uint32_t compute_family,
                       std::uint32_t frames_in_flight);

/// @brief Whether the renderer can go on drawing @p exchange's meshes.
///
/// It reads the exchange's latch, not one frame's outcome: once a mesh is
/// refused the exchange takes none again, so every later frame fails too.
///
/// @code
/// exchange.begin_frame(slot, commit);
/// const core::Status drawn = drawable(exchange);
/// // ... end the frame, then VKC_TRY(drawn) ...
/// @endcode
/// @param exchange  The renderer's exchange.
/// @return OK; or `core::Status::Code::Unsupported` naming why a mesh was
///         refused.
template <typename Payload>
core::Status drawable(const recon::mesh::MeshExchange<Payload>& exchange) {
  if (exchange.refused() == nullptr) {
    return core::Status();
  }
  return core::Status::unsupported(
      std::string("PlayerView: a mesh cannot be drawn: ") + exchange.refused());
}

}  // namespace volumetric_kit::player
