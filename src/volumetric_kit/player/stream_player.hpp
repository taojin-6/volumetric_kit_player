// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file stream_player.hpp
/// @brief Plays a `.vrst` stream's geometry: a worker thread decodes and
///        meshes each frame on the GPU when its pts is due, and hands the
///        mesh to the thread that draws it.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/player/playback.hpp"
#include "volumetric_kit/recon/codec/decoder.hpp"
#include "volumetric_kit/recon/mesh/marching_cubes.hpp"
#include "volumetric_kit/recon/mesh/mesh_exchange.hpp"
#include "volumetric_kit/recon/stream/stream_reader.hpp"
#include "volumetric_kit/recon/volume/voxel_block_grid.hpp"

namespace volumetric_kit::player {

/// @brief What the worker publishes with each frame's mesh.
struct FrameStamp {
  /// The frame's place among the track's records read in this pass, from 0:
  /// a record whose header is damaged is not counted, so it shifts the
  /// frames after it one lower.
  // TODO: count from the record's seq, less the track's first, so a damaged
  // header shifts nothing.
  std::uint64_t index = 0;
  std::uint64_t pts_ns = 0;     ///< Its pts, on its track's clock.
  double decode_mesh_ms = 0.0;  ///< The wall time decoding and meshing took.
};

/// @brief The handoff of the worker's meshes to the thread that draws them.
using FrameExchange = recon::mesh::MeshExchange<FrameStamp>;

/// @brief Plays the first geometry track of a `.vrst` file, like a video
///        player: one worker thread decodes and meshes each frame on the
///        GPU, paced by its pts, and publishes the mesh to a
///        @ref FrameExchange for the caller to draw.
///
/// **The worker** first builds the decoder and the extractor. Then, per record
/// of the track, it reads it, `codec::decode_growing` it into a grid
/// `codec::make_decode_grid` built from the first frame, waits until it is due
/// (@ref Playback) and until the consumer has taken the last mesh, then
/// extracts it with Marching Cubes and publishes it. The mesh lives in the
/// extractor's ring slot, so the next frame decodes while it is drawn. A
/// damaged record, or one this version cannot read, is skipped, and playback
/// resumes at the next key record. At the track's end it plays the track again
/// with loop on; with loop off it holds the last frame, paused, until
/// @ref play restarts it.
///
/// **Errors.** A failed build, any other failure, or a pass of the track that
/// shows no frame (its first refusal in the message), stops the worker, and
/// @ref status reports it.
///
/// **Threads.** The controls (@ref play, @ref pause, @ref step,
/// @ref set_loop) and their reads lock the mutex the worker waits under, so
/// they are safe from any thread and each change wakes the worker; @ref start,
/// @ref stop and the destructor run on one thread at a time. Only the worker
/// touches the decoder, the grid and the extractor; the caller touches only
/// the consumer side of the exchange.
///
/// **Teardown**, on every path out, so nothing the GPU may still read is
/// freed early: @ref stop, wait until the consumer's queue is idle, and
/// destroy this (the extractor, the grid and the decoder), all before the
/// exchange and the device go. A caller that opens the player before it makes
/// the device declares the player first, so it tears down in a scope guard,
/// as below.
///
/// @code
/// VKC_ASSIGN(std::unique_ptr<StreamPlayer> player, StreamPlayer::open(path));
/// // ... make the device, its allocator and the exchange ...
/// const ScopeGuard teardown([&] {  // the app's guard: runs on every return
///   player->stop();
///   app.wait_idle();
///   player.reset();
/// });
/// VKC_TRY(player->start(device, allocator, mc_config, exchange));
/// while (player->status().ok() && !closed) {
///   exchange.begin_frame(slot, [&](const auto&, FrameStamp& stamp) {
///     shown = stamp;
///     return true;
///   });
///   // ... draw exchange.live() ...
/// }
/// return player->status();
/// @endcode
class StreamPlayer {
 public:
  /// @brief Open @p path and find what to play, on the host alone.
  /// @param path  A `.vrst` file.
  /// @return The player, playing with loop on once started;
  ///         `StreamReader::open`'s refusal, such as
  ///         `Status::Code::NotFound` for a missing file and
  ///         `Status::Code::InvalidArgument` for one that is not a `.vrst`;
  ///         or `Status::Code::NotFound` for a stream with no geometry
  ///         track, or none with a record.
  static core::Result<std::unique_ptr<StreamPlayer>> open(
      const std::string& path);

  /// @brief @ref stop, then destroy the extractor, the grid and the decoder.
  /// @pre The consumer reads no mesh of this player's any more.
  ~StreamPlayer();
  StreamPlayer(const StreamPlayer&) = delete;
  StreamPlayer& operator=(const StreamPlayer&) = delete;
  StreamPlayer(StreamPlayer&&) = delete;
  StreamPlayer& operator=(StreamPlayer&&) = delete;

  /// @return The geometry track's records: the frames it plays.
  std::uint64_t frame_count() const noexcept;

  /// @brief Start the worker on @p device; it builds first (above).
  /// @param device     The compute device; it must outlive this.
  /// @param allocator  Its allocator; it must outlive this.
  /// @param config     The extractor's: the consumer's usage, queue families
  ///                   and a `slot_count` one more than its frames in flight.
  /// @param exchange   Where the meshes go; it must outlive the worker.
  /// @return OK; `Status::Code::InvalidArgument` once started or stopped; or
  ///         the thread's failure.
  core::Status start(core::Device& device, core::Allocator& allocator,
                     const recon::mesh::MarchingCubesConfig& config,
                     FrameExchange& exchange);

  /// @brief Plays, or resumes; held at the end, restarts from the first
  ///        frame.
  void play();
  /// @brief Pauses: no frame goes until @ref play or @ref step.
  void pause();
  /// @brief Pauses, and lets one more frame through.
  void step();
  /// @brief Turns looping on or off.
  void set_loop(bool on);
  /// @return Whether it plays, rather than being paused or held at the end.
  bool playing() const;
  /// @return Whether it loops.
  bool loop() const;

  /// @brief Stop the worker, playing or paused, and join it. Idempotent.
  void stop();
  /// @return OK, or the fatal error that stopped the worker.
  core::Status status() const;

 private:
  StreamPlayer(std::unique_ptr<recon::stream::StreamReader> reader,
               std::uint16_t track);

  // The worker: builds, then plays the track until quit or a fatal error,
  // into status_.
  void run();
  // Build decoder_ and extractor_ on the worker.
  core::Status build();
  core::Status play_track();
  // Build grid_ for the stream of `payload`, the first frame read.
  core::Status make_grid(const std::vector<std::uint8_t>& payload);
  // Wait until the frame at pts_ns is due, and count it shown: false on quit.
  bool wait_due(std::uint64_t pts_ns);
  // Once the consumer took the last mesh, mesh the grid and publish it with
  // @p stamp, adding the extract's time to its decode's: false on quit.
  core::Result<bool> publish(const FrameStamp& stamp);
  // At the end of a pass that showed a frame: true to play the track again,
  // false on quit.
  bool wait_at_end();
  // Apply a change to the controls under the lock, then wake the worker.
  template <typename Change>
  void control(Change&& change);

  std::unique_ptr<recon::stream::StreamReader> reader_;
  std::uint16_t track_ = 0;

  // The worker's: start sets what it builds on, and the worker builds the
  // rest; destroyed extractor first, after the join.
  core::Device* device_ = nullptr;
  core::Allocator* allocator_ = nullptr;
  recon::mesh::MarchingCubesConfig config_;
  FrameExchange* exchange_ = nullptr;
  std::optional<recon::codec::Decoder> decoder_;
  std::optional<recon::volume::VoxelBlockGrid> grid_;
  std::optional<recon::mesh::MarchingCubes> extractor_;
  std::thread worker_;

  // The controls and the worker's outcome, under mutex_; changed_ wakes the
  // worker on each change. quit_ is written under it too, and also read
  // without it by the exchange's wait.
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  Playback clock_;
  bool loop_ = true;
  std::atomic<bool> quit_{false};
  std::uint64_t last_pts_ns_ = 0;  // the last frame shown
  bool last_by_step_ = false;      // it went by a step
  core::Status status_;
};

}  // namespace volumetric_kit::player
