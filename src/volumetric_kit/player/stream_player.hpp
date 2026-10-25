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
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/gpu_timer.hpp"
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
  /// device (`core::StageRow::wait_ms`): "read" (its record, CRC included,
  /// into one of the decoder's input slots, and other tracks' records passed
  /// on the way), "decode" (parsing and recording its decode; the first
  /// frame's grid), "mesh" (recording its extract), "submit" (the commands'
  /// recording and `vkQueueSubmit`; on MoltenVK this includes encoding the
  /// Metal command buffer), "wait: GPU" (the worker blocked on its batch:
  /// about 0 while the GPU keeps up) and "finish" (judging the decode and the
  /// mesh, a rare grow, retry or refit inside). Live, there is no "read" (the
  /// wait for the network is no stage), and "mesh" is its own batch,
  /// recorded, submitted and waited on. A @ref timed frame's "decode" and
  /// "mesh" have device halves too, each over recon's breakdown rows that
  /// follow it: decode's "  ..rans decode", "  ..apply" and "  ..inverse",
  /// mesh's "  ..compact" and "  ..dispatch"; "finish" is followed by a
  /// recovery's "  ..grow", "  ..retry" or "  ..refit". A rebuild's are
  /// "decode" (the rebuild) and "mesh".
  core::StageMetrics stages;
  /// The records received since the frame before was published, this one's
  /// included: 1 for a file; live, more when only the newest was meshed.
  std::uint64_t records = 1;
  std::uint64_t record_bytes = 0;  ///< Their payload bytes.
  /// When the worker published it, so when its records had all come.
  Playback::Clock::time_point published{};
  /// Whether its device work was timed (@ref DeviceTiming); a rebuild's never
  /// is.
  bool timed = false;
  /// The pass of a file's track it was read in, from 1; live, 1.
  std::uint64_t pass = 1;
  /// What the worker's device did since the frame before was published, up
  /// to this one's publish (`core::Device::counters`): the submits, the
  /// waits that blocked and their time, the staging buffers, and the
  /// dispatches and pipeline barriers its batches recorded.
  core::DeviceCounters counters;
  /// Whether the deblock was on for its decode or rebuild
  /// (@ref StreamPlayer::set_deblock).
  bool deblock_on = false;
  /// What the deblock did to it: recon's `codec::Decoder::last_deblock`
  /// once its decode or rebuild finished: the filter, `"off"` with the
  /// deblock off or a T finer than every filter's; T; and the frame's
  /// blocks, with those recon's per-block fallback kept at their exact
  /// decode (all of them with no filter).
  recon::codec::DeblockInfo deblock{};
  /// recon's `codec::Decoder::last_rounds` once its decode finished: the most
  /// rounds its removal or allocation ran on the device. A rebuild keeps its
  /// frame's.
  std::uint32_t rounds = 0;
  /// A paused toggle's rebuild (@ref StreamPlayer::set_deblock): the frame
  /// published last, published again under the deblock as now set, so no new
  /// frame, which neither PlaybackStats nor MeasureLog counts. Its "decode"
  /// is the rebuild's.
  bool rebuild = false;
  /// When it was due: a file's, the time the clock set for it, not when the
  /// worker woke to it, or when a pause or step let it go; live, when its
  /// record arrived whole. Its latency counts from here.
  Playback::Clock::time_point due{};
  /// When the consumer took it; the consumer sets it.
  std::optional<Playback::Clock::time_point> taken;
  /// When the present call of the display frame that took it returned; set by
  /// PlayerView, none without a present.
  std::optional<Playback::Clock::time_point> presented;
  /// The worker's queue idle before its batch, in ms, on the queue's own
  /// counter: set when this and the worker's previous batch were timed and
  /// nothing was submitted between them. Not the GPU's idle: the draw's queue
  /// may run meanwhile.
  std::optional<double> device_idle_ms;
  /// Its batch's queue latency, in ms (submit call to first timestamp, on the
  /// host's clock): set only where `core::Device::calibrate` works and the
  /// batch was timed.
  std::optional<double> queue_ms;
};

/// @brief The handoff of the worker's meshes to the thread that draws them.
using FrameExchange = recon::mesh::MeshExchange<FrameStamp>;

/// The most blocks a frame may hold: the ceiling the worker passes recon's
/// `codec::make_decode_grid`, `record_growing` and `finish_growing`, which
/// bounds the decode grid (DECISIONS.md). A frame past it, or a grow past the
/// grid for it, is skipped as a damaged record is.
inline constexpr std::uint32_t kMaxFrameBlocks = 1u << 17;

/// @brief Plays the first geometry track of a `.vrst` file, like a video
///        player, or of a sender's live stream (@ref connect): one worker
///        thread decodes and meshes its frames on the GPU and publishes each
///        mesh to a @ref FrameExchange for the caller to draw.
///
/// **The worker** first builds the decoder and the extractor. Then, per record
/// of a file's track, it reads the record into one of the decoder's input
/// slots, records its decode (`codec::record_growing`, into a grid
/// `codec::make_decode_grid` built from the first frame, with the deblock as
/// @ref set_deblock last set it) and its mesh into one command batch, and
/// submits it without waiting. Once the frame is due (@ref Playback) and the
/// consumer has taken the mesh before it (@ref taken), the worker finishes the
/// batch, whose fence has normally signalled, and publishes the mesh. The
/// drawing frame waits for the mesh's `ready` value on the GPU, which is
/// already reached, so nothing stalls. While playing, it records each frame
/// as soon as the one before is published, so a mesh is ready when due. While
/// paused or held, it records a frame only once it is due, and a pause
/// publishes the frame already recorded, so whenever the clock is paused the
/// grid holds the frame shown, which a deblock toggle then rebuilds. A
/// damaged record, one this version cannot read, or one recon refuses is
/// skipped, the shown mesh stays, and playback resumes at the next key
/// record. At the track's end it plays the track again with loop on; with
/// loop off it holds the last frame, paused, until @ref play restarts it.
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
/// @ref set_loop, @ref set_deblock), their reads, @ref park, @ref unpark and
/// @ref taken lock the mutex the worker waits under, so they are safe from
/// any thread, and each change and each @ref taken wakes the worker;
/// @ref start, @ref stop and the destructor run on one thread at a time. Only
/// the worker touches the decoder, the grid and the extractor; the caller
/// touches only the consumer side of the exchange.
///
/// **Teardown**, on every path out, so nothing the GPU may still read is
/// freed early: @ref stop, wait until the consumer's queue is idle, and
/// destroy this (the extractor, whose timeline the drawing frames wait on,
/// the grid and the decoder), all before the exchange and the device go. A
/// caller that opens the player before it makes the device declares the
/// player first, so it tears down in a scope guard, as below.
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
  /// Once started, the worker builds, then connects. It reads every record
  /// of the session's first geometry track straight into one of the
  /// decoder's input slots and decodes it in a batch of its own, finished
  /// before the next is recorded, and meshes the newest decoded in a batch of
  /// its own once the consumer has taken the last mesh. It drains available
  /// input before waiting for a take, which wakes it immediately. A
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
  ///                   and a `slot_count` two more than its frames in flight
  ///                   (mesh_wiring), as a frame is meshed before the one
  ///                   before it is taken.
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

  /// @brief Turns the decoder-side deblock on or off (recon's
  ///        `codec::Decoder::set_deblock`), on by default: from the next
  ///        frame recorded, live too. Playing, that is the second frame after
  ///        the toggle, as the next is recorded already. While a file's
  ///        playback is paused or held at the end, the worker also rebuilds
  ///        the frame it published last under the new setting (recon's
  ///        `codec::Decoder::rebuild`), meshes it and publishes it again
  ///        (@ref FrameStamp::rebuild), so the change shows at once; parked,
  ///        it does so at @ref unpark. Safe before @ref start: the first
  ///        decode takes it.
  /// @param on  Whether the deblock runs.
  void set_deblock(bool on);
  /// @return Whether the deblock is on: true until @ref set_deblock.
  bool deblock() const;

  /// @brief Hold the worker for a suspend (iOS resign-active, Android
  ///        onPause).
  ///
  /// From this call on, even after a timed-out return, the worker starts no
  /// new step (its build, a read, recording and submitting a frame, finishing
  /// one, a rebuild, a mesh) and publishes nothing. The call waits for the
  /// step under way, which includes a frame's batch in flight: the worker
  /// waits that batch out, submitting nothing, and finishes and publishes the
  /// frame at @ref unpark, a grow, retry or refit it needs included.
  /// @ref playing does not change; a control applied meanwhile takes effect
  /// at @ref unpark. Call it after the consumer stops drawing. Live, a step
  /// never waits: the worker waits out a decode in flight, drops it and the
  /// record in hand, and closes the connection within a slice (about 10 ms).
  /// @param timeout  The longest wait for the step under way.
  /// @return true once the worker is idle with no batch in flight, or when
  ///         stopped or never started; false if @p timeout passed first: the
  ///         step under way may still submit, and the hold stays in force.
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

  /// @brief Tile the worker's timed batches only @p depth scopes deep, from
  ///        the next frame recorded (`core::GpuTimer::set_max_depth`): 0,
  ///        every scope, recon's breakdowns included; 1, "decode" and "mesh"
  ///        alone, with the fewest timestamps. Safe from any thread; for
  ///        M4's A/B.
  ///
  /// @code
  /// player.set_tile_depth(measure.tile_depth);
  /// @endcode
  /// @param depth  0 or 1.
  void set_tile_depth(std::uint32_t depth) noexcept {
    tile_depth_.store(depth, std::memory_order_relaxed);
  }
  /// @return What @ref set_tile_depth last set; 0 before.
  std::uint32_t tile_depth() const noexcept {
    return tile_depth_.load(std::memory_order_relaxed);
  }

  /// @brief Tell the worker the consumer has taken a mesh: call it from the
  ///        commit of each FrameExchange::begin_frame that takes one. The
  ///        worker sleeps until then before it publishes the next mesh,
  ///        rather than polling the exchange.
  void taken();

  /// @brief Stop the worker, playing or paused, and join it. Idempotent.
  void stop();
  /// @return OK, or the fatal error that stopped the worker.
  core::Status status() const;

 private:
  StreamPlayer(std::unique_ptr<recon::stream::StreamReader> reader,
               std::uint16_t track);
  explicit StreamPlayer(LiveAddress address);

  // A step of the worker's, as park counts them: its build, a record's read,
  // a frame from its recording to its finish (its batch in flight included),
  // a rebuild; live, a decode from its recording to its finish, or a mesh.
  // Movable, so a frame's step goes with its batch.
  class Step;
  // A pass of a file's track: the next record's place, whether a frame was
  // shown, the first record skipped, and the next frame's rows so far.
  struct Pass;
  // A file's record read into one of the decoder's input slots, stamped.
  struct Next;
  // A batch the worker submitted, with what its idle and queue latency need.
  struct Batch;
  // A frame recorded and submitted, until finished: its stamp, its pending
  // decode and extract (live, none), its step and its batch.
  struct InFlight;
  // When a frame was let go: when the clock had it due (FrameStamp::due),
  // and when the worker saw that, later by its wake-up, from which the
  // clock's late rule counts (Playback::shown).
  struct Due {
    Playback::Clock::time_point at;
    Playback::Clock::time_point seen;
  };

  // The worker: builds, then plays the track or the live stream until quit or
  // a fatal error, into status_.
  void run();
  // Build decoder_, extractor_ and timer_ on the worker.
  core::Status build();
  core::Status play_track();
  // The next record of the track read into `*next` (empty at the pass's
  // end), within a step; skippable refusals skipped: false on quit.
  core::Result<bool> read_next(recon::stream::Position* pos, Pass& pass,
                               std::optional<Next>* next);
  // Record `next`'s decode and its mesh into one batch and submit it, into
  // `*frame`, within a step that the frame keeps; a record recon refuses
  // leaves it empty, as does quit.
  core::Status record_frame(Next next, Pass& pass,
                            std::optional<InFlight>* frame);
  // Once `frame` is due, finish it and publish its mesh (a refused frame
  // publishes nothing): false on quit.
  core::Result<bool> finish_frame(InFlight frame, Pass& pass);
  // Connect, play the connection until it ends, and connect again, until
  // quit; only a GPU failure is returned.
  core::Status play_live();
  // Play `client`'s connection until it ends, into `*ended`, or quit or a
  // park ends it (`*ended` OK). Only a GPU failure is returned.
  core::Status play_connection(recon::networking::StreamClient& client,
                               core::Status* ended);
  // Live: wait for `decoding`'s batch and finish it, into `*newest`, with
  // its blocks: true once it succeeded; false, leaving them, for a skippable
  // refusal.
  core::Result<bool> finish_decode(InFlight decoding, FrameStamp* newest,
                                   std::uint32_t* blocks);
  // Record into `batch` the decode of the record of `size` bytes at the
  // start of `slot`, with the deblock as set_deblock last set it: `stamp`'s
  // "decode" row, and when it is timed recon's breakdowns under it. The
  // first frame read builds grid_ first. On a refusal the caller drops
  // `batch`.
  core::Result<recon::codec::PendingDecode> record_decode(
      core::CommandBatch& batch, const core::HostSlot& slot, std::size_t size,
      FrameStamp& stamp);
  // Wait for `frame`'s batch, as its "wait: GPU" row, then note it
  // (note_batch).
  core::Status wait_batch(InFlight& frame);
  // Build grid_ for the stream of the record of `size` bytes at the start of
  // `slot`, the first frame read.
  core::Status make_grid(const core::HostSlot& slot, std::size_t size);
  // Whether the next frame's device work is timed, as timing_ asks.
  bool time_next_frame();
  // A batch on the worker's device; when `timed`, timed in timer_ at the
  // tile depth now set.
  core::CommandBatch new_batch(bool timed);
  // Submit `batch`, setting `signal` when given, timed into `rows`' "submit"
  // row when given.
  core::Result<Batch> submit(core::CommandBatch& batch,
                             const core::TimelinePoint* signal,
                             core::StageMetrics* rows);
  // After `batch`'s wait, once `stamp`'s rows are in: the device halves of
  // its rows that `stamp` has, and for its first timed batch its idle and
  // queue latency.
  void note_batch(const Batch& batch, FrameStamp& stamp);
  // grid_ meshed in a batch of its own, `blocks` expected, submitted and
  // waited on; timed, and into a `row` of `stamp`'s, when `row` is set. None
  // when its slot came back unpublished.
  core::Result<std::optional<recon::mesh::DeviceMesh>> mesh_alone(
      FrameStamp& stamp, std::uint32_t blocks, const char* row);
  // `stamp`'s deblock fields and rounds, from the decoder's last finish or
  // rebuild.
  void stamp_deblock(FrameStamp& stamp) const;
  // Wait until the frame at pts_ns is due: when, or none on quit. Without
  // `frame`, it rebuilds last_ meanwhile when rebuild_wanted. With `frame`,
  // recorded: a pause publishes it now, and a park waits out its batch and
  // ends its step, to finish it at unpark.
  core::Result<std::optional<Due>> wait_due(std::uint64_t pts_ns,
                                            InFlight* frame);
  // Count the frame at pts_ns shown as of `seen`, when wait_due saw it due,
  // once it decoded: the clock takes a step.
  void count_shown(std::uint64_t pts_ns, Playback::Clock::time_point seen);
  // Under the lock: whether a paused toggle asks for last_ to be rebuilt.
  bool rebuild_wanted() const;
  // Rebuild last_ under the deblock as now set, untimed, mesh it and publish
  // it again (FrameStamp::rebuild): false on quit. A rebuild recon refuses as
  // skippable publishes nothing.
  core::Result<bool> rebuild();
  // Once the consumer took the last mesh and while not parked, publish
  // `mesh` with `stamp`: false on quit.
  core::Result<bool> publish(const recon::mesh::DeviceMesh& mesh,
                             FrameStamp stamp);
  // Under the lock: publish `mesh` with `stamp` now, the device's counts
  // since the last publish into its counters.
  void publish_now(const recon::mesh::DeviceMesh& mesh, FrameStamp stamp);
  // At the end of a pass that showed a frame: true to play the track again,
  // false on quit; it rebuilds last_ meanwhile when rebuild_wanted.
  core::Result<bool> wait_at_end();
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
  std::optional<core::GpuTimer> timer_;  // the worker's timed batches'
  std::thread worker_;
  std::uint64_t frames_started_ = 0;  // for kSampled
  core::DeviceCounters counted_;      // the device's, at the last publish
  // The worker's last batch waited on, if timed: its last timestamp, and the
  // device's submits once it was submitted, so the next shows its idle only
  // when nothing was submitted between them.
  std::optional<std::uint64_t> last_end_ticks_;
  std::uint64_t last_submits_ = 0;
  // A file's frame published last, its stages cleared, which grid_ holds
  // while the clock is paused; the worker's alone.
  std::optional<FrameStamp> last_;

  std::atomic<DeviceTiming> timing_{DeviceTiming::kOff};
  std::atomic<std::uint32_t> tile_depth_{0};

  // The controls, the park, the connection and the worker's outcome, under
  // mutex_; changed_ wakes the worker on each change, and a park at each
  // step's end, and taken() after each take.
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  Playback clock_;
  bool loop_ = true;
  bool deblock_ = true;  // the decoder's, from its next decode
  bool parked_ = false;  // no step starts
  int busy_ = 0;         // steps under way
  bool quit_ = false;
  std::uint64_t last_pts_ns_ = 0;  // the last frame shown
  bool last_by_step_ = false;      // it went by a step
  LiveStatus live_status_;
  core::Status status_;
};

}  // namespace volumetric_kit::player
