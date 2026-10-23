// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file stream_player.hpp
/// @brief Plays a `.vrst` stream's geometry, from a file or live from a
///        sender: a worker thread decodes and meshes its frames on the GPU,
///        and hands each mesh to the thread that draws it.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/stage_metrics.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/player/playback.hpp"
#include "volumetric_kit/recon/codec/decoder.hpp"
#include "volumetric_kit/recon/mesh/marching_cubes.hpp"
#include "volumetric_kit/recon/mesh/mesh_exchange.hpp"
#include "volumetric_kit/recon/networking/stream_client.hpp"
#include "volumetric_kit/recon/networking/stream_server.hpp"
#include "volumetric_kit/recon/stream/session.hpp"
#include "volumetric_kit/recon/stream/stream_reader.hpp"
#include "volumetric_kit/recon/volume/voxel_block_grid.hpp"

namespace volumetric_kit::player {

/// @brief A live stream's address: a sender's
///        (recon::networking::StreamServer).
struct LiveAddress {
  std::string host;  ///< A name or a numeric IPv4 address.
  std::uint16_t port = recon::networking::kDefaultPort;  ///< 1 to 65535.
};

/// @brief Parse a live stream's address, on the host alone.
///
/// @code
/// VKC_ASSIGN(const LiveAddress address, parse_live_address("lab:47801"));
/// @endcode
/// @param text  "host" or "host:port".
/// @return The address, on `recon::networking::kDefaultPort` when it names
///         none; `core::Status::Code::InvalidArgument` naming what is wrong
///         for an empty host or a port that is not a number from 1 to 65535.
core::Result<LiveAddress> parse_live_address(std::string_view text);

/// @return @p address as "host:port".
std::string to_string(const LiveAddress& address);

/// @brief Where a live player's connection stands.
enum class LiveState {
  kConnecting,  ///< Looking up, connecting, or waiting to retry.
  kConnected,   ///< Connected, with no record yet: the sender has no key yet.
  kLive,        ///< Records arriving.
};

/// @brief A live player's connection, for its status line.
struct LiveStatus {
  LiveState state = LiveState::kConnecting;
  std::string address;  ///< to_string() of the player's address.
  /// Why the last connection ended, or the last attempt failed
  /// (@ref live_reason). Empty until one has, and once a park has ended one.
  std::string last_error;
  std::uint64_t seq = 0;                      ///< The newest record received.
  Playback::Clock::time_point last_record{};  ///< When it arrived.
};

/// @brief Why a live connection ended, for its status line, which names the
///        address already.
///
/// @code
/// // "cannot connect: Connection refused"
/// const std::string why = live_reason(
///     core::Status::io_error(
///         "StreamClient: lab:47801: cannot connect: Connection refused"),
///     "lab:47801");
/// @endcode
/// @param ended    What ended it: recon's `networking::StreamClient` error, or
///                 the player's own.
/// @param address  to_string() of the address the client was given.
/// @return @p ended's message less the "StreamClient: <address>: " or
///         "StreamClient::connect: " it starts with (StreamClient's message
///         convention); whole with neither, as a bare "out of memory".
std::string live_reason(const core::Status& ended, const std::string& address);

/// @brief How often the worker times its frames' device work
///        (@ref StreamPlayer::set_device_timing).
enum class DeviceTiming : std::uint8_t {
  kOff,      ///< Never: recon gets no metrics, so it writes no timestamp.
  kSampled,  ///< One frame in @ref kSampleEvery: the first, then every
             ///< kSampleEvery-th.
  kAll,      ///< Every frame.
};

/// The frames @ref DeviceTiming::kSampled takes one of.
inline constexpr std::uint64_t kSampleEvery = 10;

/// @return @p timing as a measurement names it: "off", "sampled" or "all".
const char* to_string(DeviceTiming timing) noexcept;

/// @brief What the worker publishes with each frame's mesh.
struct FrameStamp {
  /// A file's frame: its place among the track's records read in this pass,
  /// from 0; a record whose header is damaged is not counted, so it shifts
  /// the frames after it one lower. A live frame: its record's seq.
  // TODO: count from the record's seq, less the track's first, so a damaged
  // header shifts nothing.
  std::uint64_t index = 0;
  std::uint64_t pts_ns = 0;  ///< Its pts, on its track's clock.
  std::uint64_t bytes = 0;   ///< Its record's payload bytes.
  bool key = false;          ///< A key record: an intra (I) frame; else P.
  /// Its stages, each with its host time and the part of it blocked on the
  /// device (`core::StageRow::wait_ms`): "read" (its record's header and
  /// payload, CRC included, and other tracks' records passed on the way),
  /// "decode" (the first frame's grid included) and "mesh"; decode and mesh
  /// each block on their fences. Live, there is no "read" (the wait for the
  /// network is no stage), and "decode" is this record's alone. A @ref timed
  /// frame's "decode" and "mesh" have device halves too, each over recon's
  /// breakdown rows that follow it: decode's "  ..rans decode", "  ..apply"
  /// and "  ..inverse", mesh's "  ..compact" (when it compacts) and
  /// "  ..dispatch".
  core::StageMetrics stages;
  /// The records received since the frame before was published, this one's
  /// included: 1 for a file; live, more when only the newest was meshed.
  std::uint64_t records = 1;
  std::uint64_t record_bytes = 0;  ///< Their payload bytes.
  /// When the worker published it, so when its records had all come.
  Playback::Clock::time_point published{};
  /// Whether its device work was timed (@ref DeviceTiming).
  bool timed = false;
  /// The pass of a file's track it was read in, from 1; live, 1.
  std::uint64_t pass = 1;
  /// What the worker's device did since the frame before was published, up
  /// to this one's publish (`core::Device::counters`): the submits, the
  /// waits that blocked and their time, and the staging buffers.
  core::DeviceCounters counters;
};

/// @brief The handoff of the worker's meshes to the thread that draws them.
using FrameExchange = recon::mesh::MeshExchange<FrameStamp>;

/// The most blocks a frame may hold: the ceiling the worker passes recon's
/// `codec::make_decode_grid` and `decode_growing`, which bounds the decode
/// grid (DECISIONS.md). A frame past it, or a grow past the grid for it, is
/// skipped as a damaged record is.
inline constexpr std::uint32_t kMaxFrameBlocks = 1u << 17;

/// @brief Plays the first geometry track of a `.vrst` file, like a video
///        player, or of a sender's live stream (@ref connect): one worker
///        thread decodes and meshes its frames on the GPU and publishes each
///        mesh to a @ref FrameExchange for the caller to draw.
///
/// **The worker** first builds the decoder and the extractor. Then, per record
/// of a file's track, it reads it, `codec::decode_growing` it into a grid
/// `codec::make_decode_grid` built from the first frame, waits until it is due
/// (@ref Playback) and until the consumer has taken the last mesh (@ref taken),
/// then extracts it with Marching Cubes and publishes it. The mesh lives in the
/// extractor's ring slot, so the next frame decodes while it is drawn. A
/// damaged record, or one this version cannot read, is skipped, and playback
/// resumes at the next key record. At the track's end it plays the track again
/// with loop on; with loop off it holds the last frame, paused, until
/// @ref play restarts it.
///
/// **Errors.** A failed build, any other failure, or a pass of the track that
/// shows no frame (its first refusal in the message), stops the worker, and
/// @ref status reports it. Live, a network or protocol failure is none
/// (@ref connect).
///
/// **Timing.** Each frame's stamp has its stages' host and wait times, and
/// the worker's device counts, always; its device times only as
/// @ref set_device_timing asks, off until then.
///
/// **Threads.** The controls (@ref play, @ref pause, @ref step,
/// @ref set_loop), their reads, @ref park, @ref unpark and @ref taken lock the
/// mutex the worker waits under, so they are safe from any thread, and each
/// change and each @ref taken wakes the worker; @ref start, @ref stop and the
/// destructor run on one thread at a time. Only the worker touches the
/// decoder, the grid and the extractor; the caller touches only the consumer
/// side of the exchange.
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
///     player->taken();
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

  /// @brief A player of what a sender serves at @p address
  ///        (recon::networking::StreamServer). It does no I/O, so it is safe
  ///        on the main thread.
  ///
  /// Once started, the worker builds, then connects. It decodes every record
  /// of the session's first geometry track, and meshes the newest once the
  /// consumer has taken the last mesh. It drains available input before
  /// waiting for a take, which wakes it immediately. A
  /// connection that ends or cannot be made is never an error:
  /// the worker shows why (@ref live_status), and about a second later
  /// connects again, with a new grid, until stopped. Play, pause, step and
  /// loop do nothing.
  /// @param address  The sender's.
  /// @return The player; `core::Status::Code::InvalidArgument` for an empty
  ///         host or port 0.
  static core::Result<std::unique_ptr<StreamPlayer>> connect(
      LiveAddress address);

  /// @brief @ref stop, then destroy the extractor, the grid and the decoder.
  /// @pre The consumer reads no mesh of this player's any more.
  ~StreamPlayer();
  StreamPlayer(const StreamPlayer&) = delete;
  StreamPlayer& operator=(const StreamPlayer&) = delete;
  StreamPlayer(StreamPlayer&&) = delete;
  StreamPlayer& operator=(StreamPlayer&&) = delete;

  /// @return Whether it plays a live stream (@ref connect) rather than a
  ///         file.
  bool live() const noexcept { return reader_ == nullptr; }
  /// @return The geometry track's records: the frames it plays; 0 live.
  std::uint64_t frame_count() const noexcept;
  /// @return The geometry track's index in a file's session; 0 live.
  std::uint16_t track() const noexcept { return track_; }
  /// @return The geometry track's counts, from the footer or the open scan;
  ///         none live.
  const recon::stream::TrackStats& track_stats() const noexcept;
  /// @return The track's nominal rate; `num` 0 when unknown, and live.
  recon::stream::Rate rate() const noexcept;
  /// @return A live player's connection, under the mutex, so safe from any
  ///         thread; a file's is never anything but the default.
  LiveStatus live_status() const;

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

  /// @brief Hold the worker for a suspend (iOS resign-active, Android
  ///        onPause). From this call on, the worker starts no new step (its
  ///        kernel build, a read, a decode or an extract), even after a
  ///        timed-out return; the call waits for the step under way.
  ///        @ref playing does not change; a control applied meanwhile takes
  ///        effect at @ref unpark. Call it after the consumer stops drawing.
  ///        Live, a step is GPU work alone and never waits: the worker drops
  ///        the record in hand and closes the connection within a slice
  ///        (about 10 ms).
  /// @param timeout  The longest wait for the step under way.
  /// @return true once the worker is idle, or when stopped or never started;
  ///         false if @p timeout passed first: the step under way may still
  ///         submit, and the hold stays in force.
  [[nodiscard]] bool park(std::chrono::milliseconds timeout);
  /// @brief End a @ref park. The worker carries on from the same frame,
  ///        playing or paused as before; live, it connects again, with a new
  ///        grid. A no-op when not parked.
  void unpark();

  /// @brief Time the device work of the frames the worker starts from now on
  ///        as @p timing says; safe from any thread. A timed frame's recon
  ///        writes timestamps, which cost device time (DECISIONS.md).
  ///
  /// @code
  /// player.set_device_timing(panel.open() ? DeviceTiming::kAll
  ///                                       : DeviceTiming::kOff);
  /// @endcode
  /// @param timing  How often.
  void set_device_timing(DeviceTiming timing) noexcept {
    timing_.store(timing, std::memory_order_relaxed);
  }
  /// @return What @ref set_device_timing last set; kOff before.
  DeviceTiming device_timing() const noexcept {
    return timing_.load(std::memory_order_relaxed);
  }

  /// @brief Tell the worker the consumer has taken a mesh: call it from the
  ///        commit of each FrameExchange::begin_frame that takes one. The
  ///        worker sleeps until then before it meshes the next frame, rather
  ///        than polling the exchange.
  void taken();

  /// @brief Stop the worker, playing or paused, and join it. Idempotent.
  void stop();
  /// @return OK, or the fatal error that stopped the worker.
  core::Status status() const;

 private:
  StreamPlayer(std::unique_ptr<recon::stream::StreamReader> reader,
               std::uint16_t track);
  explicit StreamPlayer(LiveAddress address);

  // A step of the worker's, as park counts them: its kernel build, a record's
  // read and decode, or an extract; live, a decode or an extract.
  class Step;

  // The worker: builds, then plays the track or the live stream until quit or
  // a fatal error, into status_.
  void run();
  // Build decoder_ and extractor_ on the worker.
  core::Status build();
  core::Status play_track();
  // Connect, play the connection until it ends, and connect again, until
  // quit; only a GPU failure is returned.
  core::Status play_live();
  // Play `client`'s connection until it ends, into `*ended`, or quit or a
  // park ends it (`*ended` OK). `payload` is the worker's buffer. Only a GPU
  // failure is returned.
  core::Status play_connection(recon::networking::StreamClient& client,
                               std::vector<std::uint8_t>& payload,
                               core::Status* ended);
  // Build grid_ for the stream of `payload`, the first frame read.
  core::Status make_grid(const std::vector<std::uint8_t>& payload);
  // Decode `payload` into grid_, built from it if there is none, timed into
  // `stages`' "decode" row, and with `timed` its device half and recon's
  // breakdowns.
  core::Status decode(const std::vector<std::uint8_t>& payload,
                      core::StageMetrics& stages, bool timed);
  // Whether the next frame's device work is timed, as timing_ asks.
  bool time_next_frame();
  // Wait until the frame at pts_ns is due, and count it shown: false on quit.
  bool wait_due(std::uint64_t pts_ns);
  // Once the consumer took the last mesh, mesh the grid and publish it with
  // @p stamp, the extract timed into its "mesh" row: false on quit.
  core::Result<bool> publish(FrameStamp stamp);
  // Mesh grid_ and publish it with `stamp`, the extract timed into its "mesh"
  // row (its device half too when `stamp.timed`) and the device's counts
  // since the last publish into its counters, within a step, once the
  // consumer took the last mesh.
  core::Status mesh_and_publish(FrameStamp stamp);
  // At the end of a pass that showed a frame: true to play the track again,
  // false on quit.
  bool wait_at_end();
  // Live, outside any step: wait while parked; false on quit.
  bool wait_unparked();
  // Live, as a connection ends (`ended` OK when quit or a park ended it):
  // show why, or nothing for OK, and after a failure wait about a second for
  // the retry, or until quit or a park; false on quit.
  bool wait_to_retry(const core::Status& ended);
  // Apply a change to the controls under the lock, then wake the worker.
  template <typename Change>
  void control(Change&& change);

  std::unique_ptr<recon::stream::StreamReader> reader_;  // null live
  std::uint16_t track_ = 0;
  LiveAddress address_;  // live

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
  std::uint64_t frames_started_ = 0;  // for kSampled
  core::DeviceCounters counted_;      // the device's, at the last publish

  std::atomic<DeviceTiming> timing_{DeviceTiming::kOff};

  // The controls, the park, the connection and the worker's outcome, under
  // mutex_; changed_ wakes the worker on each change, and a park at each
  // step's end, and taken() after each take.
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  Playback clock_;
  bool loop_ = true;
  bool parked_ = false;  // no step starts
  bool busy_ = false;    // a step is under way
  bool quit_ = false;
  std::uint64_t last_pts_ns_ = 0;  // the last frame shown
  bool last_by_step_ = false;      // it went by a step
  LiveStatus live_status_;
  core::Status status_;
};

}  // namespace volumetric_kit::player
