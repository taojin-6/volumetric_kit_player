// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file frame_hooks.hpp
/// @brief A caller's per-frame work, which the player runs: on the worker
///        after each mesh, as the drawing thread takes a mesh, before the
///        draw, and at a park; a line for the view's stats panel; and the
///        take that the view and a consumer with no view share
///        (@ref FrameTaker).

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"
#include "volumetric_kit/player/stream_player.hpp"
#include "volumetric_kit/recon/mesh/device_mesh.hpp"
#include "volumetric_kit/recon/volume/voxel_block_grid.hpp"

namespace volumetric_kit::gfx::pipelines {
class HybridMeshPipeline;
}  // namespace volumetric_kit::gfx::pipelines

namespace volumetric_kit::player {

/// @brief What one display frame's draw uses beyond the mesh.
struct DrawBinding {
  /// HybridMeshFrame's set-0 field: a set made against the view's pipeline
  /// (gfx::pipelines::HybridMeshTextureSet); null, the pipeline's own.
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  /// gfx::pipelines::HybridMeshFlags cleared from the view's, such as
  /// kHybridMeshLit.
  std::uint32_t mesh_flags_clear = 0;
};

/// @brief What a commit hook does with a frame taken for drawing.
enum class FrameCommit {
  kDraw,  ///< It becomes the shown frame.
  kHold,  ///< Kept back, the shown frame drawn; asked again next display
          ///< frame.
  kSkip,  ///< Let go undrawn: the shown frame stays, and the next frame
          ///< published is taken at the next display frame. One frame at
          ///< most is skipped between two drawn: a kSkip with a skip and no
          ///< draw since holds the frame, as kHold does.
};

/// @brief A caller's per-frame work, which the player runs. Any member may
///        be empty.
///
/// @code
/// FrameHooks hooks;
/// hooks.after_mesh = [](core::CommandBatch&,
///                       const recon::volume::VoxelBlockGrid&,
///                       const recon::mesh::DeviceMesh&, FrameStamp& stamp) {
///   stamp.extra = std::make_shared<const Clock::time_point>(Clock::now());
///   return core::Status();
/// };
/// hooks.commit = [](const recon::mesh::DeviceMesh&, FrameStamp&) {
///   return core::Result<FrameCommit>(FrameCommit::kDraw);
/// };
/// VKC_TRY(player.start(device, allocator, mc_config, exchange, &hooks));
/// @endcode
struct FrameHooks {
  /// Worker, before each publish: a frame meshed again after its decode was
  /// redone, a paused toggle's rebuild and a live frame included, with the
  /// grid that mesh was made from, read-only. `batch` is on the worker's
  /// device; the player submits it after the hook, waiting on the mesh's
  /// ready point, and the published mesh's ready point becomes the batch's.
  /// The player finishes the batch, its readbacks written, before anything
  /// grows the grid or replaces it with a live connection's next, before
  /// the next after_mesh, before a park returns true or a stop returns, and
  /// before a frame's stamp goes unpublished at a stop. So the batch may
  /// record work that reads the grid, such as a compaction of its map
  /// (recon's volume::VoxelHashMap::record_compaction, which binds the
  /// map's own kernel sets until the batch has resolved); the next frame's
  /// batch, queued after it, is ordered after it by the core's barriers.
  /// A host wait inside stalls playback; a failure stops it with its status.
  std::function<core::Status(
      core::CommandBatch& batch, const recon::volume::VoxelBlockGrid& grid,
      const recon::mesh::DeviceMesh& mesh, FrameStamp& stamp)>
      after_mesh;
  /// The thread that takes meshes, inside its take
  /// (recon::mesh::MeshExchange::begin_frame's commit, through
  /// @ref FrameTaker), for a frame just taken or held, once its ready value
  /// is reached. Empty means kDraw. A failure stops the view with its
  /// status.
  std::function<core::Result<FrameCommit>(const recon::mesh::DeviceMesh& mesh,
                                          FrameStamp& stamp)>
      commit;
  /// Draw thread, each display frame that draws a mesh, after the take and
  /// before the render pass, with the shown frame's stamp. What it returns
  /// applies to that frame's draw; a failure stops the view with its status.
  std::function<core::Result<DrawBinding>(VkCommandBuffer cmd,
                                          const FrameStamp& stamp)>
      before_draw;
  /// In StreamPlayer::park, on its thread, once the worker is idle with
  /// nothing in flight and before a park that returns true returns.
  std::function<void()> on_park;
  /// In StreamPlayer::unpark, before the worker's next step; only after an
  /// on_park.
  std::function<void()> on_resume;
  /// Draw thread, at a rebuild of the view's stats panel while it is open,
  /// at most once a @ref StatsPanel::kProgramLineRefresh: a program's line, a
  /// short text the panel shows as its last line (@ref stats_lines) until
  /// the next call. Not called while the panel is closed or no frame is
  /// drawn, so a call may come long after the one before. Empty, or an empty
  /// text, shows nothing; with no view it is never called.
  std::function<std::string()> stats_line;
};

/// @brief What hooks are made with, valid during MakeHooks' call alone: a
///        hook may keep what it names, which outlives the hooks, but not the
///        context itself.
struct HookContext {
  core::Device& device;        ///< The worker's: after_mesh's batch's.
  core::Allocator& allocator;  ///< Its allocator.
  /// The device whose queue takes and draws meshes; the worker's with no
  /// view.
  core::Device& commit_device;
  core::Allocator& commit_allocator;  ///< Its allocator.
  /// The view's mesh pipeline; null with no view.
  const gfx::pipelines::HybridMeshPipeline* pipeline = nullptr;
  std::uint32_t compute_family = 0;   ///< The worker's queue family.
  std::uint32_t graphics_family = 0;  ///< The draw's.
};

/// Makes a run's hooks, once, before its worker starts.
using MakeHooks = std::function<core::Result<FrameHooks>(const HookContext&)>;

/// @brief The take of a player's meshes for a renderer, one per display
///        frame, with a commit hook's say (@ref FrameHooks::commit): what
///        PlayerView and a consumer with no view, vp_probe's `--worker`,
///        share.
///
/// Each take runs MeshExchange::begin_frame on the display frame's slot. A
/// mesh first taken is told to the player once (StreamPlayer::taken), held
/// or not. A mesh whose ready value the device has not reached is held
/// without asking the hook, as gfx's frame loop draws only behind values
/// reached and after_mesh's batch may still run. Otherwise the commit hook
/// decides: kDraw commits it (recon's CommitDecision::kCommit), kHold parks
/// it (kPark), and kSkip drops it (kDrop). As one frame at most is skipped
/// between two drawn (FrameCommit::kSkip), the ring needs one mesh more
/// (mesh_wiring's `holds`). Each slot keeps the extra (FrameStamp::extra) of
/// the frame it drew until its next take, so an extra lives until the GPU has
/// finished every display frame that drew its frame.
///
/// @code
/// FrameTaker taker(*player, exchange, kFramesInFlight, &hooks);
/// while (playing) {
///   // ... the slot's last frame has completed ...
///   VKC_ASSIGN(std::optional<FrameStamp> drawn, taker.take(slot));
///   if (drawn) shown = std::move(drawn);
///   // ... draw exchange.live() ...
/// }
/// @endcode
class FrameTaker {
 public:
  /// @param player            The meshes' player; it must outlive this.
  /// @param exchange          Its exchange; it must outlive this.
  /// @param frames_in_flight  The renderer's, as the exchange's config says;
  ///                          at least 1.
  /// @param hooks             Whose commit decides, or null for every frame
  ///                          drawn; it must outlive this.
  FrameTaker(StreamPlayer& player, FrameExchange& exchange,
             std::uint32_t frames_in_flight, const FrameHooks* hooks = nullptr);

  /// @brief One display frame's take: forget the extra @p slot's last frame
  ///        kept, take the newest mesh through the commit hook, and keep the
  ///        extra of the frame @p slot now draws.
  /// @pre The frame that last used @p slot has completed.
  /// @param slot  The display frame's slot, below its frames in flight.
  /// @return The stamp of the frame committed, now live, its `taken` set;
  ///         none when the live mesh stays; or the commit hook's failure, or
  ///         a failure to read a ready value, either of which leaves the mesh
  ///         held.
  core::Result<std::optional<FrameStamp>> take(std::uint32_t slot);

  /// @return The frames a commit hook skipped (FrameCommit::kSkip).
  std::uint64_t frames_skipped() const noexcept { return skipped_; }
  /// @return The frames held at least once before their commit or skip:
  ///         by the hook's kHold, a skip read as one, or a ready value not
  ///         yet reached.
  std::uint64_t frames_held() const noexcept { return held_; }

 private:
  StreamPlayer* player_;
  FrameExchange* exchange_;
  const FrameHooks* hooks_;
  std::vector<std::shared_ptr<const void>> slot_extras_;  // per slot
  std::shared_ptr<const void> live_extra_;                // the live frame's
  std::uint64_t offered_ = 0;  // the generation last handed to the commit
  bool held_offered_ = false;  // it was held once
  bool skipped_since_draw_ = false;
  std::uint64_t skipped_ = 0;
  std::uint64_t held_ = 0;
};

}  // namespace volumetric_kit::player
