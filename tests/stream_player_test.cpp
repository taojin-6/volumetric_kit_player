// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// StreamPlayer on a device, over the tests' stream (test_stream.hpp). Its
// track's counts and rate are what was written. Paused and stepped, so each
// frame goes when the test says: frames arrive in order with their index,
// pts, payload bytes, key, set (kNoSet for a record that names none), stage
// times (read, decode, mesh, submit, the wait
// for its batch and finish, untimed with no device half) and the worker's
// device counts over the frame alone; timed, decode and mesh have device
// halves over grid's breakdowns, every frame or one in kSampleEvery, and
// from the second batch on the queue's idle before it, and its queue latency
// where the device calibrates; each pass of the track is counted; each mesh
// is non-empty on the device, carries a reached ready value, and a frame that
// meshes nothing still reaches the commit with its stamp. Played, a steady
// frame is one batch, one submit, no staging buffer, at most one wait that
// blocked, and the dispatches and barriers its kind records; its rounds and
// its due time are stamped. Paused, nothing is recorded ahead; a pause
// publishes the frame recorded ahead at once, and the grid then holds it.
// With loop on a step past the end wraps, with loop off the last frame holds
// until play restarts; a corrupt P-frame, a damaged record header or a record
// this version cannot read resyncs at the next key, as does one grid refuses
// on the device, whose ring slot comes back over more passes than the ring
// has; a stream with no key left stops the worker with the reason, one of
// keys past kMaxFrameBlocks blocks with grid's refusal naming that ceiling;
// quit is prompt, playing, paused or waiting for a consumer that takes
// nothing; the worker sleeps until taken() rather than polling for the take; a
// fatal error stops the worker, a failed build on it included, as does a ring
// one short; start is refused once started or stopped. With hooks
// (frame_hooks.hpp): after_mesh runs once before each publish, a redone
// frame's and a rebuild's included, with the grid its mesh was made from;
// the mesh published is ready once after_mesh's batch is, a steady frame
// then two submits with no added blocking wait, and the batch finished, its
// readback written, by the next after_mesh, a park, before its on_park, a
// stop, and before an unpublished frame's extra goes, though a park waits
// for it no longer than its timeout, returning false with no on_park and the
// hold kept; an after_mesh failure stops playback; and a batch that reads the
// grid, a compaction of its map, finishes before a frame grows the grid, at
// its finish or, outgrowing it, at its record, stepped, played or after a
// rebuild, so playback goes on and each counts its frame's blocks.
// vp_probe --worker's
// consumer (FrameTaker): a held frame keeps the one before, the file at most
// two frames ahead; a skipped frame is never drawn and the ring one mesh
// deeper never runs out; a second skip before a draw holds; a commit failure
// is the take's and holds the frame; and each slot keeps the extra of the
// frame it drew until its next take.
// Playing, no frame arrives before its pts says. The deblock is on by
// default, and each frame says what it did, its filter and its blocks, the
// exact ones among them: the true-SDF stream's, none exact, each mesh unlike
// its rebuild with the deblock off; the masked sphere's, its blocks holding
// an unobserved voxel exact, each mesh like that rebuild exactly when all
// are; set before start, the first decode takes it; toggled while paused, the
// frame shown, not the next, is rebuilt, meshed and published again as a
// rebuild with its set, never timed, with nothing else going, at the end with
// loop off
// too, and parked only at unpark; toggled while playing, the frames after
// take it with no rebuild. A park holds the worker:
// nothing is published while parked, a mesh waiting for the consumer
// included; a batch in flight is waited out, nothing is submitted, and its
// frame is published at unpark, a grow and a decode again it needs included;
// a control applied meanwhile (a step, a play and a pause) takes
// effect at unpark; playing() does not change; unpark carries on from the
// same frame; a park returns as soon as the step under way ends; one that
// times out mid-decode still lets nothing start after it; one before start
// holds the build; one before start or after stop returns at once; and a stop
// while parked is prompt, at a step's start too.
//
// Live, from a grid StreamServer on 127.0.0.1 publishing the stream (looped
// and restamped for a steady flow), or a raw peer: frames arrive with their
// seq as their index and their set, the status's too, a mesh of their own
// decode, decode, submit, wait,
// finish and mesh rows alone and the deblock on, which a toggle turns off and
// on from the next record; each record is a decode batch and each mesh one,
// with no staging buffer once the slots have grown; a join after a P-frame
// shows that GOP from its key; P-frames before a connection's first key are
// skipped, though counted, with status() OK; every record published while a
// mesh waits for the consumer is decoded, and the newest is shown with the
// records since; a take with a frame in hand has it meshed at once; records
// slower than their decode are meshed once nothing newer has come, and
// records faster still show a newer frame every 100 ms or so; a consumer
// leaving a mesh untaken does not delay draining buffered small records or
// a record spanning many receives; taking nothing for 1.5 s leaves status()
// OK, and the next take is followed by the newest frame, while PlaybackStats
// fed as the view feeds it reads no spike in the records a second; a server
// going away is a retry with its reason, a new one on its port, at another
// voxel size, gives frames again, and a park then leaves no reason; a park
// closes the connection within kClosed, submitting nothing after it, 8 times
// at spread points as records flow, and once with none coming; an unpark
// joins at the newest key; bytes
// that are no stream, a session with no geometry track and a name that does not
// resolve show why, with status() OK; a reason of StreamClient's shows less
// its prefix, which names the address; a park ends the wait to retry, so
// the unpark connects at once; a stop is prompt while a refused connect
// waits to retry, connected and idle, and parked; and with hooks, after_mesh
// runs before each publish with its grid, a park runs on_park and the unpark
// on_resume, a held frame keeps the one before while records keep decoding,
// a new connection's grid waits for the last after_mesh batch, and a record
// that grows the grid, at its decode's finish or its record, for the batch
// of one that read it. Live, the playout clock (live_clock.hpp), its
// records' pts a second apart so the tests' own paces add no delay: a
// program's need is asked once a record and sets D, each frame taken no
// earlier than its present time and released no earlier than due; its
// data_ready's never to 30 frames in a row publishes none, the next ready
// one drawn for all their records; its not yet holds a frame, the one before
// shown and nothing released, then publishes it once; a need raised after a
// frame's release delays its take, which counts as no hold; at a need of
// 150 ms each record is a frame of its own, in order, and as D falls the
// order holds; a need past a second is clamped; and a park drops the records
// held as bytes, the unpark joining at the key. A file asks no live hook and
// is released when due, with no playout. On the
// host: open refuses
// what it cannot play, and connect an empty host or port 0. The device tests
// skip with no device. A stop that no control can wake fails only on ctest's
// timeout.

#include "volumetric_kit/player/stream_player.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>  // IWYU pragma: keep (ssize_t on Linux)
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <future>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "test_stream.hpp"
#include "volumetric_kit/core/base/unique_fd.hpp"
#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/gpu_timer.hpp"
#include "volumetric_kit/core/vulkan/sync.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"
#include "volumetric_kit/grid/base/device_requirements.hpp"
#include "volumetric_kit/grid/mesh/mesh.hpp"
#include "volumetric_kit/grid/networking/stream_server.hpp"
#include "volumetric_kit/grid/stream/chunks.hpp"
#include "volumetric_kit/grid/stream/stream_writer.hpp"
#include "volumetric_kit/grid/volume/voxel_block_grid.hpp"
#include "volumetric_kit/grid/volume/voxel_hash_map.hpp"
#include "volumetric_kit/player/frame_hooks.hpp"
#include "volumetric_kit/player/live_clock.hpp"
#include "volumetric_kit/player/playback_stats.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace grid = volumetric_kit::grid;
namespace networking = grid::networking;
namespace stream = grid::stream;
using namespace std::chrono_literals;
using volumetric_kit::player::DelayNeed;
using volumetric_kit::player::DeviceTiming;
using volumetric_kit::player::FrameCommit;
using volumetric_kit::player::FrameExchange;
using volumetric_kit::player::FrameHooks;
using volumetric_kit::player::FrameStamp;
using volumetric_kit::player::FrameTaker;
using volumetric_kit::player::kMaxLiveDelay;
using volumetric_kit::player::LiveState;
using volumetric_kit::player::LiveStatus;
using volumetric_kit::player::PlaybackStats;
using volumetric_kit::player::Playout;
using volumetric_kit::player::present;
using volumetric_kit::player::StreamPlayer;
using vp_test::kFrames;
using vp_test::kIntraPeriod;
using vp_test::kPeriodNs;
using vp_test::pts_of;
using vp_test::Scratch;
using Bytes = std::vector<std::uint8_t>;
using Code = core::Status::Code;
using SteadyClock = std::chrono::steady_clock;

// The default exchange's.
constexpr std::uint32_t kFramesInFlight = 2;
// How long a frame the test let go may take to arrive; how long one it did
// not is waited for; and the longest a prompt stop or park takes.
constexpr auto kArrival = 10s;
constexpr auto kQuiet = 300ms;
constexpr auto kPrompt = 1s;
// A RECD chunk's header (session.hpp): its payload's CRC, its flags, its pts,
// and its CRC of the bytes before it.
constexpr std::size_t kRecordHeader = 56;
constexpr std::size_t kBodyCrcAt = 16;
constexpr std::size_t kFlagsAt = 22;
constexpr std::size_t kPtsAt = 24;
constexpr std::size_t kHeaderCrcAt = 52;
// A codec frame's block count, in its header (grid's codec/bitstream.hpp).
constexpr std::size_t kBlockCountAt = 32;
// The streams' T (grid's DeblockInfo::step), in metres: the default scale,
// 0.2, times the largest sample of the (0,0,1) basis, cos(pi/16) / 16, times
// trunc_dist, the sphere's 0.04 m and the true-SDF stream's 0.16 m
// (vp_write_stream); and how far grid's float arithmetic may land from it.
constexpr float kSphereStep = 0.00049039264f;
constexpr float kTrueSdfStep = 0.0019615706f;
constexpr float kStepError = 1e-9f;
// A frame's sphere's centre moves this far along x a frame (vp_write_stream).
constexpr double kMetresAFrame = 0.08;

// zlib's CRC-32, which a chunk's header carries (session.hpp).
std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

// Writes `value` little-endian at `bytes[at]`, as the stream and the codec
// lay out every field.
void put_u32(std::vector<std::uint8_t>& bytes, std::size_t at,
             std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) {
    bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
  }
}

// How long `player.stop()` takes. One that is not back within kPrompt fails
// the test; controls then wake the worker, parked or not, so the test still
// ends.
SteadyClock::duration stop_time(StreamPlayer& player) {
  const SteadyClock::time_point start = SteadyClock::now();
  std::promise<void> done;
  std::future<void> stopped = done.get_future();
  std::thread stopper([&] {
    player.stop();
    done.set_value();
  });
  if (stopped.wait_for(kPrompt) != std::future_status::ready) {
    ADD_FAILURE() << "stop did not return within " << kPrompt.count() << " s";
    player.unpark();
    player.play();
  }
  stopper.join();
  return SteadyClock::now() - start;
}

// Parks once the worker is inside a step that cannot end: the caller holds
// the device's submit mutex, which every step but a record's read takes (the
// build, a decode, a rebuild, an extract). A park that finds the worker
// between steps holds it there, and one that finds it reading returns once
// the read ends, within kRead, so this unparks and lets it go on until a park
// times out on a step. Whether one did within kArrival; the hold then stays
// in force.
bool park_mid_step(StreamPlayer& player) {
  constexpr auto kRead = 50ms;
  const SteadyClock::time_point deadline = SteadyClock::now() + kArrival;
  while (player.park(kRead)) {
    player.unpark();
    if (SteadyClock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

// What a test's after_mesh saw of a mesh: its frame, the mesh, and the
// grid's blocks.
struct Meshed {
  std::uint64_t index = 0;
  std::uint64_t pass = 0;
  bool rebuild = false;
  std::uint64_t generation = 0;
  std::uint32_t blocks = 0;  // the grid's allocated blocks
  std::uint32_t triangles = 0;
  const core::TimelineSemaphore* ready = nullptr;  // the mesh's, as it saw it
};

// A test's hooks' record: written by the worker, read by the test.
class HookLog {
 public:
  void add(const Meshed& meshed) {
    std::lock_guard<std::mutex> lock(mutex_);
    meshed_.push_back(meshed);
  }
  std::vector<Meshed> meshed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return meshed_;
  }
  // The highest frame index meshed in pass `pass`; none before one.
  std::optional<std::uint64_t> newest(std::uint64_t pass = 1) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<std::uint64_t> newest;
    for (const Meshed& meshed : meshed_) {
      if (meshed.pass == pass && (!newest || meshed.index > *newest)) {
        newest = meshed.index;
      }
    }
    return newest;
  }

  std::atomic<int> parks{0};    // on_park's calls
  std::atomic<int> resumes{0};  // on_resume's

 private:
  mutable std::mutex mutex_;
  std::vector<Meshed> meshed_;
};

// What a compacting after_mesh (StreamPlayerTest::compacting_hooks) saw of
// its frame's grid, and what its batch counted there.
struct Compacted {
  std::uint64_t index = 0;
  std::uint32_t allocated = 0;  // the grid's blocks, at the hook
  std::int32_t num_blocks = 0;  // its heap, at the hook
  // The active blocks the batch's compaction counted on the device, read
  // back: written once the batch has finished.
  std::uint32_t counted = ~0u;
};

// What a program's live hooks saw, shared with the worker that calls them,
// so it outlives the test's body: its need, how often it was asked for it,
// and data_ready's frames in the order asked.
struct LiveAsks {
  std::atomic<std::int64_t> need_ns{0};
  std::atomic<int> needs{0};
  std::mutex mutex;
  std::vector<std::uint64_t> ready;  // under mutex

  void asked(std::uint64_t index) {
    const std::lock_guard<std::mutex> lock(mutex);
    ready.push_back(index);
  }
  std::size_t count(std::uint64_t index) {
    const std::lock_guard<std::mutex> lock(mutex);
    return static_cast<std::size_t>(
        std::count(ready.begin(), ready.end(), index));
  }
  std::vector<std::uint64_t> all() {
    const std::lock_guard<std::mutex> lock(mutex);
    return ready;
  }
};

// Each frame taken (FrameTaker), live: at or after its present time on the
// clock as it was taken, released no earlier than due, its playout's need
// a program's.
void expect_taken_when_due(const FrameStamp& frame) {
  ASSERT_TRUE(frame.playout.has_value()) << "frame " << frame.index;
  ASSERT_TRUE(frame.taken.has_value());
  EXPECT_GE(*frame.taken, present(frame.playout->point, frame.pts_ns))
      << "frame " << frame.index << " taken before its present time";
  EXPECT_GE(frame.released, frame.due) << "frame " << frame.index;
  EXPECT_GE(frame.published, frame.released) << "frame " << frame.index;
}

class StreamPlayerTest : public core::test::VulkanDeviceTest {
 protected:
  core::DeviceRequirements requirements() const override {
    return grid::device_requirements();
  }

  void SetUp() override {
    VulkanDeviceTest::SetUp();
    if (base_setup_incomplete()) {
      return;
    }
    path_ = scratch_.copy(VP_TEST_STREAMS, vp_test::kStream);
    ASSERT_FALSE(path_.empty());
  }

  // Every test's player stops here too, so one that fails early is still
  // stopped within kPrompt.
  void TearDown() override {
    if (player_ != nullptr) {
      stop_time(*player_);
    }
    VulkanDeviceTest::TearDown();
  }

  // The extractor's, for a renderer of kFramesInFlight frames into
  // `slot_count` ring slots: mesh_wiring's two more by default.
  static grid::mesh::MarchingCubesConfig extractor_config(
      std::uint32_t slot_count = kFramesInFlight + 2) {
    grid::mesh::MarchingCubesConfig config;
    config.extra_vertex_usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    config.extra_index_usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    config.slot_count = slot_count;
    return config;
  }

  // The stream opened and started on the test's device, paused unless
  // `playing`, into `slot_count` ring slots, with hooks_ when set; the
  // fixture owns it.
  StreamPlayer* start(bool playing,
                      std::uint32_t slot_count = kFramesInFlight + 2) {
    core::Result<std::unique_ptr<StreamPlayer>> opened =
        StreamPlayer::open(path_);
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    if (!opened.ok()) {
      return nullptr;
    }
    player_ = std::move(opened).value();
    if (!playing) {
      player_->pause();
    }
    player_->set_device_timing(timing_);
    const core::Status started =
        player_->start(device(), allocator(), extractor_config(slot_count),
                       exchange_, hooks_ ? &*hooks_ : nullptr);
    EXPECT_TRUE(started.ok()) << started.message();
    return started.ok() ? player_.get() : nullptr;
  }

  // The next frame `taker` commits, taking a display frame each millisecond
  // on alternate slots, as vp_probe's --worker does: its stamp, or empty
  // when none comes within `wait`, or with a failure recorded when the take
  // fails. Its mesh's ready value has been reached (FrameTaker).
  std::optional<FrameStamp> take_with(FrameTaker& taker,
                                      SteadyClock::duration wait = kArrival) {
    const SteadyClock::time_point deadline = SteadyClock::now() + wait;
    while (SteadyClock::now() < deadline) {
      core::Result<std::optional<FrameStamp>> taken = taker.take(slot_);
      slot_ = (slot_ + 1) % kFramesInFlight;
      if (!taken.ok()) {
        ADD_FAILURE() << taken.status().message();
        return std::nullopt;
      }
      if (taken->has_value()) {
        if (!exchange_.live().empty()) {
          expect_ready(exchange_.live());
        }
        return std::move(*taken);
      }
      std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
  }

  // hooks_ with an after_mesh that notes each mesh into log_, as it saw it,
  // and an on_park and an on_resume that count into it.
  FrameHooks& logging_hooks() {
    hooks_.emplace();
    hooks_->after_mesh =
        [this](core::CommandBatch&, const grid::volume::VoxelBlockGrid& grid,
               const grid::mesh::DeviceMesh& mesh, FrameStamp& stamp) {
          log_.add({stamp.index, stamp.pass, stamp.rebuild, mesh.generation,
                    grid.map().allocated_blocks(), mesh.triangle_count,
                    mesh.ready.semaphore});
          return core::Status();
        };
    hooks_->on_park = [this] { ++log_.parks; };
    hooks_->on_resume = [this] { ++log_.resumes; };
    return *hooks_;
  }

  // hooks_ as logging_hooks makes them, with after_mesh also recording work
  // that reads the grid it is handed read-only: a compaction of its map's
  // active blocks (grid's VoxelHashMap::record_compaction), whose batch
  // binds the map's own kernel sets until it has resolved, their count read
  // back into compacted_.
  FrameHooks& compacting_hooks() {
    logging_hooks();
    const auto log = hooks_->after_mesh;
    hooks_->after_mesh = [this, log](core::CommandBatch& batch,
                                     const grid::volume::VoxelBlockGrid& grid,
                                     const grid::mesh::DeviceMesh& mesh,
                                     FrameStamp& stamp) -> core::Status {
      VKC_TRY(log(batch, grid, mesh, stamp));
      VKC_ASSIGN(const grid::volume::RecordedBlockList active,
                 grid.map().record_compaction(batch, nullptr));
      std::lock_guard<std::mutex> lock(compacted_mutex_);
      Compacted& read = compacted_.emplace_back();
      read.index = stamp.index;
      read.allocated = grid.map().allocated_blocks();
      read.num_blocks = grid.grid().num_blocks;
      return batch.readback(*active.count, 0, sizeof read.counted,
                            &read.counted);
    };
    return *hooks_;
  }

  // hooks_ with a live_delay that answers `asks`' need, counting its calls,
  // and, when `answer` is given, a data_ready that notes each frame and
  // answers it.
  FrameHooks& live_hooks(
      const std::shared_ptr<LiveAsks>& asks,
      std::function<FrameCommit(const FrameStamp&)> answer = {}) {
    hooks_.emplace();
    hooks_->live_delay = [asks] {
      ++asks->needs;
      return std::chrono::nanoseconds(asks->need_ns.load());
    };
    if (answer) {
      hooks_->data_ready = [asks, answer](const FrameStamp& stamp) {
        asks->asked(stamp.index);
        return answer(stamp);
      };
    }
    return *hooks_;
  }

  // Once the player has stopped: each of compacting_hooks' batches counted
  // on the device the blocks its frame's grid held, `blocks`' for its frame.
  // The grid's heap each saw, in order.
  std::vector<std::int32_t> expect_compacted(
      const std::vector<std::uint32_t>& blocks) {
    std::vector<std::int32_t> heaps;
    std::lock_guard<std::mutex> lock(compacted_mutex_);
    for (const Compacted& read : compacted_) {
      SCOPED_TRACE(::testing::Message() << "frame " << read.index);
      EXPECT_EQ(read.counted, read.allocated) << "its batch's count";
      if (read.index < blocks.size()) {
        EXPECT_EQ(read.allocated, blocks[read.index]);
      }
      heaps.push_back(read.num_blocks);
    }
    return heaps;
  }

  // The tests' stream `name` is the test's (test_stream.hpp).
  void use_stream(const char* name) {
    path_ = scratch_.copy(VP_TEST_STREAMS, name);
    ASSERT_FALSE(path_.empty());
  }

  // Frame kGrownFrame's blocks, in `blocks`, grow the decode grid the first
  // frame sizes: at the frame's record when they pass its slots, as the
  // outgrow stream's do; otherwise at its finish, as the grow stream's,
  // which fill it past what its table can place, do.
  static void expect_grown_at(const std::vector<std::uint32_t>& blocks,
                              bool at_record) {
    ASSERT_EQ(blocks.size(), kFrames);
    ASSERT_LE(2 * blocks[0], vp_test::kFirstGridSlots)
        << "a grid of more slots";
    const std::uint32_t grown = blocks[vp_test::kGrownFrame];
    if (at_record) {
      ASSERT_GT(grown, vp_test::kFirstGridSlots);
    } else {
      ASSERT_GT(grown, vp_test::kFirstGridSlots - vp_test::kTableFloor);
      ASSERT_LE(grown, vp_test::kFirstGridSlots);
    }
  }

  // The heaps compacting_hooks' after_mesh saw, by frame, `heaps`: the first
  // grid's until frame kGrownFrame, then a grown one's.
  static void expect_grew_at_grown_frame(
      const std::vector<std::int32_t>& heaps) {
    ASSERT_EQ(heaps.size(), kFrames);
    for (std::uint64_t i = 0; i < kFrames; ++i) {
      if (i < vp_test::kGrownFrame) {
        EXPECT_EQ(heaps[i], static_cast<std::int32_t>(vp_test::kFirstGridSlots))
            << "frame " << i;
      } else {
        EXPECT_GT(heaps[i], static_cast<std::int32_t>(vp_test::kFirstGridSlots))
            << "frame " << i << ": no grow";
      }
    }
  }

  // Stepped through a pass of the test's stream, a grow stream, with
  // compacting_hooks: every frame comes, and each after_mesh's batch counted
  // its frame's blocks on the grid as it saw it, the first until frame
  // kGrownFrame and a grown one from it.
  void step_through_compacting(bool grown_at_record) {
    const std::vector<std::uint32_t> blocks = block_counts();
    ASSERT_NO_FATAL_FAILURE(expect_grown_at(blocks, grown_at_record));
    compacting_hooks();
    StreamPlayer* player = start(/*playing=*/false);
    ASSERT_NE(player, nullptr);
    FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
    for (std::uint64_t i = 0; i < kFrames; ++i) {
      player->step();
      const std::optional<FrameStamp> frame = take_with(taker);
      ASSERT_TRUE(frame.has_value())
          << "no frame " << i << ": " << player->status().message();
      EXPECT_EQ(frame->index, i);
    }
    EXPECT_LT(stop_time(*player), kPrompt);
    EXPECT_TRUE(player->status().ok()) << player->status().message();
    expect_grew_at_grown_frame(expect_compacted(blocks));
  }

  // As step_through_compacting, played with loop off: each frame is
  // recorded as soon as the one before is published, whose after_mesh batch
  // may still run. Frame kGrownFrame's stamp in `grown`.
  void play_through_compacting(bool grown_at_record,
                               std::optional<FrameStamp>* grown) {
    const std::vector<std::uint32_t> blocks = block_counts();
    ASSERT_NO_FATAL_FAILURE(expect_grown_at(blocks, grown_at_record));
    compacting_hooks();
    StreamPlayer* player = start(/*playing=*/true);
    ASSERT_NE(player, nullptr);
    player->set_loop(false);  // one pass, then the last frame holds
    FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
    for (std::uint64_t i = 0; i < kFrames; ++i) {
      const std::optional<FrameStamp> frame = take_with(taker);
      ASSERT_TRUE(frame.has_value())
          << "no frame " << i << ": " << player->status().message();
      EXPECT_EQ(frame->index, i);
      if (i == vp_test::kGrownFrame) {
        *grown = frame;
      }
    }
    EXPECT_LT(stop_time(*player), kPrompt);
    EXPECT_TRUE(player->status().ok()) << player->status().message();
    expect_grew_at_grown_frame(expect_compacted(blocks));
  }

  // As step_through_compacting, with frame kGrownFrame - 1 rebuilt by a
  // deblock toggle while paused there, with no frame in flight: the
  // rebuild's after_mesh batch is the one in flight when frame kGrownFrame,
  // stepped to next, grows the grid. The rebuilt frame's batch counts its
  // blocks too, on the first grid.
  void rebuild_then_grow_compacting(bool grown_at_record) {
    const std::vector<std::uint32_t> blocks = block_counts();
    ASSERT_NO_FATAL_FAILURE(expect_grown_at(blocks, grown_at_record));
    compacting_hooks();
    StreamPlayer* player = start(/*playing=*/false);
    ASSERT_NE(player, nullptr);
    FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
    for (std::uint64_t i = 0; i < vp_test::kGrownFrame; ++i) {
      player->step();
      const std::optional<FrameStamp> frame = take_with(taker);
      ASSERT_TRUE(frame.has_value())
          << "no frame " << i << ": " << player->status().message();
      EXPECT_EQ(frame->index, i);
    }
    std::this_thread::sleep_for(kQuiet);  // the worker waits for the next due
    player->set_deblock(!player->deblock());
    const std::optional<FrameStamp> rebuilt = take_with(taker);
    ASSERT_TRUE(rebuilt.has_value())
        << "no rebuild: " << player->status().message();
    EXPECT_TRUE(rebuilt->rebuild);
    EXPECT_EQ(rebuilt->index, vp_test::kGrownFrame - 1);
    for (std::uint64_t i = vp_test::kGrownFrame; i < kFrames; ++i) {
      player->step();
      const std::optional<FrameStamp> frame = take_with(taker);
      ASSERT_TRUE(frame.has_value())
          << "no frame " << i << ": " << player->status().message();
      EXPECT_EQ(frame->index, i);
      EXPECT_FALSE(frame->rebuild);
    }
    EXPECT_LT(stop_time(*player), kPrompt);
    EXPECT_TRUE(player->status().ok()) << player->status().message();
    std::vector<std::int32_t> heaps = expect_compacted(blocks);
    ASSERT_EQ(heaps.size(), kFrames + 1);
    EXPECT_EQ(heaps[vp_test::kGrownFrame], heaps[vp_test::kGrownFrame - 1])
        << "the rebuild's grid";
    // The frames' heaps, without the rebuild's.
    heaps.erase(heaps.begin() +
                static_cast<std::ptrdiff_t>(vp_test::kGrownFrame));
    expect_grew_at_grown_frame(heaps);
  }

  // The next mesh the worker publishes, taken as a renderer's frame takes
  // it, through the commit, and checked on the device: non-empty but for
  // the stream's empty_frame_. Its stamp, or empty when none comes within
  // `wait`.
  std::optional<FrameStamp> next_frame(
      std::chrono::milliseconds wait = kArrival) {
    const SteadyClock::time_point deadline = SteadyClock::now() + wait;
    while (SteadyClock::now() < deadline) {
      std::optional<FrameStamp> taken;
      const grid::mesh::ExchangeOutcome outcome = exchange_.begin_frame(
          slot_, [&](const grid::mesh::DeviceMesh&, FrameStamp& stamp) {
            taken = stamp;
            player_->taken();
            return true;
          });
      slot_ = (slot_ + 1) % kFramesInFlight;
      if (outcome == grid::mesh::ExchangeOutcome::kRefused) {
        ADD_FAILURE() << "refused: " << exchange_.refused();
        return std::nullopt;
      }
      if (outcome == grid::mesh::ExchangeOutcome::kCommitted) {
        // Every mesh committed, an empty one included, came through the
        // commit with its stamp.
        EXPECT_TRUE(taken.has_value()) << "a mesh committed without its stamp";
        const grid::mesh::DeviceMesh& mesh = exchange_.live();
        if (taken) {
          EXPECT_EQ(mesh.empty(), taken->index == empty_frame_)
              << "frame " << taken->index;
        }
        // An empty mesh may name null buffers: no draw command to read.
        if (mesh.valid()) {
          EXPECT_EQ(index_count(mesh),
                    grid::mesh::kIndicesPerTriangle * mesh.triangle_count);
        }
        if (!mesh.empty()) {
          expect_ready(mesh);
        }
        return taken;
      }
      std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
  }

  // The next mesh published, taken as next_frame takes it but with no check
  // on the device, so at once and with no work of the test's on it; its
  // stamp, or empty when none comes within `wait`. Each committed mesh's
  // ready value has been reached.
  std::optional<FrameStamp> take(SteadyClock::duration wait = kArrival) {
    const SteadyClock::time_point deadline = SteadyClock::now() + wait;
    while (SteadyClock::now() < deadline) {
      std::optional<FrameStamp> taken;
      const grid::mesh::ExchangeOutcome outcome = exchange_.begin_frame(
          slot_, [&](const grid::mesh::DeviceMesh&, FrameStamp& stamp) {
            taken = stamp;
            player_->taken();
            return true;
          });
      slot_ = (slot_ + 1) % kFramesInFlight;
      if (outcome == grid::mesh::ExchangeOutcome::kCommitted) {
        if (!exchange_.live().empty()) {
          expect_ready(exchange_.live());
        }
        return taken;
      }
      std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
  }

  // A mesh carries the value on its extractor's timeline that the batch
  // which wrote it set, reached by its hand-over (DeviceMesh::ready), which a
  // drawing frame waits on with no stall.
  static void expect_ready(const grid::mesh::DeviceMesh& mesh) {
    ASSERT_NE(mesh.ready.semaphore, nullptr)
        << "generation " << mesh.generation;
    EXPECT_GT(mesh.ready.value, 0u);
    const core::Result<std::uint64_t> reached = mesh.ready.semaphore->value();
    ASSERT_TRUE(reached.ok()) << reached.status().message();
    EXPECT_GE(*reached, mesh.ready.value) << "generation " << mesh.generation;
  }

  // Whether `done()` holds within `wait`, asked every millisecond.
  template <typename Done>
  static bool eventually(Done&& done, SteadyClock::duration wait = kArrival) {
    const SteadyClock::time_point deadline = SteadyClock::now() + wait;
    while (!done()) {
      if (SteadyClock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(1ms);
    }
    return true;
  }

  // Once the worker submits a batch past the device's `submits`, within
  // kArrival: a frame recorded ahead.
  bool submitted_after(std::uint64_t submits) {
    return eventually([&] { return device().counters().submits > submits; });
  }

  // Once the batch of the frame after `taken`, the frame taken last, has run,
  // within kArrival: recorded ahead while playing, it sets the value after
  // `taken`'s mesh's on the extractor's timeline, and its recording, a grow
  // included, has submitted all it will. Whether that frame is still kAhead
  // or more from due, so a control applied now finds its batch in flight: it
  // is due no earlier than its pts step after `taken` was (FrameStamp::due).
  // A test skips without, as a device too slow for this window cannot show
  // what the test checks.
  bool next_batch_in_flight(const FrameStamp& taken) {
    constexpr auto kAhead = 100ms;
    const core::TimelinePoint ready = exchange_.live().ready;
    EXPECT_NE(ready.semaphore, nullptr);
    if (ready.semaphore == nullptr) {
      return false;
    }
    const core::Status ran = ready.semaphore->wait(
        ready.value + 1,
        std::chrono::duration_cast<std::chrono::nanoseconds>(kArrival).count());
    EXPECT_TRUE(ran.ok()) << "frame " << taken.index + 1
                          << "'s batch: " << ran.message();
    const auto step =
        std::chrono::nanoseconds(pts_of(taken.index + 1) - pts_of(taken.index));
    return ran.ok() && SteadyClock::now() + kAhead < taken.due + step;
  }

  // The index count the mesh's indirect draw command holds on the device.
  std::uint32_t index_count(const grid::mesh::DeviceMesh& mesh) {
    // A view of the extractor's buffer: its deleter frees nothing.
    const core::Buffer command(
        mesh.indirect, sizeof(VkDrawIndexedIndirectCommand),
        mesh.indirect_usage, mesh.sharing_mode, nullptr, [] {}, std::nullopt);
    VkDrawIndexedIndirectCommand read{};
    core::CommandBatch batch(device(), allocator());
    core::Status status = batch.readback(command, 0, sizeof read, &read);
    if (status.ok()) {
      status = batch.submit();
    }
    EXPECT_TRUE(status.ok()) << status.message();
    return read.indexCount;
  }

  // The mesh's vertices, read back from the device; none, with a failure
  // recorded, for an empty mesh.
  std::vector<grid::mesh::Vertex> vertices(const grid::mesh::DeviceMesh& mesh) {
    const VkDeviceSize size =
        VkDeviceSize{mesh.vertex_count} * sizeof(grid::mesh::Vertex);
    if (size == 0) {
      ADD_FAILURE() << "an empty mesh";
      return {};
    }
    // A view of the extractor's buffer: its deleter frees nothing.
    const core::Buffer buffer(
        mesh.vertices, size, mesh.vertex_usage, mesh.sharing_mode, nullptr,
        [] {}, std::nullopt);
    std::vector<grid::mesh::Vertex> read(mesh.vertex_count);
    core::CommandBatch batch(device(), allocator());
    core::Status status = batch.readback(buffer, 0, size, read.data());
    if (status.ok()) {
      status = batch.submit();
    }
    EXPECT_TRUE(status.ok()) << status.message();
    return read;
  }

  // The mean x of the mesh's vertices on the device, in metres: about its
  // frame's sphere's centre, kMetresAFrame times the frame.
  double mean_x(const grid::mesh::DeviceMesh& mesh) {
    const std::vector<grid::mesh::Vertex> read = vertices(mesh);
    if (read.empty()) {
      return 0.0;
    }
    double sum = 0.0;
    for (const grid::mesh::Vertex& vertex : read) {
      sum += vertex.position.x;
    }
    return sum / static_cast<double>(read.size());
  }

  // The positions of the mesh's vertices, sorted, so two extracts of the same
  // voxels compare equal in whatever order the device wrote them.
  std::vector<std::array<float, 3>> positions(
      const grid::mesh::DeviceMesh& mesh) {
    std::vector<std::array<float, 3>> sorted;
    for (const grid::mesh::Vertex& vertex : vertices(mesh)) {
      sorted.push_back(
          {vertex.position.x, vertex.position.y, vertex.position.z});
    }
    std::sort(sorted.begin(), sorted.end());
    return sorted;
  }

  // Each frame's blocks, as its header counts them (grid's
  // codec::read_frame_info), in file order.
  std::vector<std::uint32_t> block_counts() {
    std::vector<std::uint32_t> counts;
    core::Result<std::unique_ptr<stream::StreamReader>> reader =
        stream::StreamReader::open(path_);
    EXPECT_TRUE(reader.ok()) << reader.status().message();
    if (!reader.ok()) {
      return counts;
    }
    for (const stream::Record& record : records()) {
      Bytes payload;
      const core::Status read = (*reader)->read(record, &payload);
      EXPECT_TRUE(read.ok()) << read.message();
      const core::Result<grid::codec::FrameInfo> info =
          grid::codec::read_frame_info(payload.data(), payload.size());
      EXPECT_TRUE(info.ok()) << info.status().message();
      counts.push_back(info.ok() ? info->block_count : 0);
    }
    return counts;
  }

  // Frame `frame`, just shown paused with the deblock on, which ran `filter`
  // on its `blocks` and kept `exact` of them at their exact decode: rebuilt
  // with the deblock off, which keeps every block exact, its mesh is the
  // same exactly when the deblock kept every block; and rebuilt with it on
  // again, it is as shown. So the next step decodes with the deblock on.
  void expect_deblock_against_off(std::uint64_t frame, const char* filter,
                                  std::uint32_t blocks, std::uint32_t exact) {
    StreamPlayer& player = *player_;
    const std::vector<std::array<float, 3>> on = positions(exchange_.live());
    ASSERT_FALSE(on.empty());
    player.set_deblock(false);
    const std::optional<FrameStamp> off = next_frame();
    ASSERT_TRUE(off.has_value()) << "no rebuild of frame " << frame;
    EXPECT_TRUE(off->rebuild);
    EXPECT_EQ(off->index, frame);
    EXPECT_STREQ(off->deblock.filter, "off");
    EXPECT_EQ(off->deblock.block_count, blocks);
    EXPECT_EQ(off->deblock.exact_blocks, blocks);
    EXPECT_EQ(positions(exchange_.live()) == on, exact == blocks)
        << "frame " << frame << ", " << exact << " of " << blocks
        << " blocks exact";
    player.set_deblock(true);
    const std::optional<FrameStamp> again = next_frame();
    ASSERT_TRUE(again.has_value()) << "no rebuild of frame " << frame;
    EXPECT_TRUE(again->rebuild);
    EXPECT_STREQ(again->deblock.filter, filter);
    EXPECT_EQ(again->deblock.block_count, blocks);
    EXPECT_EQ(again->deblock.exact_blocks, exact);
    EXPECT_EQ(positions(exchange_.live()), on);
  }

  // The test's stream is the true-SDF one, which the deblock runs on.
  void use_true_sdf() {
    path_ = scratch_.copy(VP_TEST_STREAMS, vp_test::kTrueSdfStream);
    ASSERT_FALSE(path_.empty());
  }

  // The next mesh is frame `frame`'s, published again by a rebuild with the
  // deblock `on`, which ran `filter`, and is the live mesh, of that frame.
  void expect_rebuild(std::uint64_t frame, bool on, const char* filter) {
    const std::optional<FrameStamp> got = next_frame();
    ASSERT_TRUE(got.has_value()) << "no rebuild of frame " << frame;
    EXPECT_TRUE(got->rebuild);
    EXPECT_EQ(got->index, frame);
    EXPECT_EQ(got->pts_ns, pts_of(frame));
    EXPECT_EQ(got->set, vp_test::set_of(frame)) << "its frame's";
    EXPECT_EQ(got->deblock_on, on);
    EXPECT_STREQ(got->deblock.filter, filter);
    EXPECT_NEAR(got->deblock.step, kTrueSdfStep, kStepError);
    EXPECT_NEAR(mean_x(exchange_.live()), kMetresAFrame * frame, 0.02);
  }

  // The records of the stream at `path`, the test's by default, in file
  // order.
  std::vector<stream::Record> records() { return records(path_); }
  static std::vector<stream::Record> records(const std::string& path) {
    std::vector<stream::Record> all;
    core::Result<std::unique_ptr<stream::StreamReader>> reader =
        stream::StreamReader::open(path);
    EXPECT_TRUE(reader.ok()) << reader.status().message();
    if (!reader.ok()) {
      return all;
    }
    stream::Position pos = (*reader)->begin();
    stream::Record record;
    for (;;) {
      const core::Result<bool> more = (*reader)->next(&pos, &record);
      EXPECT_TRUE(more.ok()) << more.status().message();
      if (!more.ok() || !*more) {
        return all;
      }
      all.push_back(record);
    }
  }

  // The file's `size` bytes at `at`.
  std::vector<std::uint8_t> read_at(std::uint64_t at, std::size_t size) {
    std::vector<std::uint8_t> bytes(size);
    std::ifstream file(path_, std::ios::binary);
    file.seekg(static_cast<std::streamoff>(at));
    file.read(reinterpret_cast<char*>(bytes.data()),
              static_cast<std::streamsize>(size));
    EXPECT_TRUE(file.good());
    return bytes;
  }

  // Overwrites the file's bytes at `at` with `bytes`.
  void write_at(std::uint64_t at, const std::vector<std::uint8_t>& bytes) {
    std::fstream file(path_, std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(static_cast<std::streamoff>(at));
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    file.close();
    ASSERT_TRUE(file.good());
  }

  // Flips the byte at `at` in the file.
  void flip(std::uint64_t at) {
    std::vector<std::uint8_t> byte = read_at(at, 1);
    byte[0] ^= 0x5A;
    write_at(at, byte);
  }

  // What reading `record` again from a fresh reader returns: the header's
  // failure, or its payload's.
  core::Status reread(const stream::Record& record) {
    core::Result<std::unique_ptr<stream::StreamReader>> reader =
        stream::StreamReader::open(path_);
    if (!reader.ok()) {
      return reader.status();
    }
    stream::Position pos{record.offset};
    stream::Record found;
    const core::Result<bool> more = (*reader)->next(&pos, &found);
    if (!more.ok()) {
      return more.status();
    }
    std::vector<std::uint8_t> payload;
    return (*reader)->read(record, &payload);
  }

  // Flips a byte in `record`'s payload, which then fails its CRC.
  void corrupt_payload(const stream::Record& record) {
    ASSERT_NO_FATAL_FAILURE(
        flip(record.offset + kRecordHeader + record.size / 2));
    EXPECT_EQ(reread(record).domain(), Code::InvalidArgument);
  }

  // Flips a byte of `record`'s pts, so its header fails its CRC.
  void corrupt_header(const stream::Record& record) {
    ASSERT_NO_FATAL_FAILURE(flip(record.offset + kPtsAt));
    EXPECT_EQ(reread(record).domain(), Code::InvalidArgument);
  }

  // Sets a flag version 0 does not define in `record`'s header, with its CRC
  // rewritten, so the header reads as one this version cannot.
  void set_unknown_flag(const stream::Record& record) {
    std::vector<std::uint8_t> header = read_at(record.offset, kRecordHeader);
    header[kFlagsAt + 1] |= 0x80;  // little-endian u16: bit 15
    put_u32(header, kHeaderCrcAt, crc32(header.data(), kHeaderCrcAt));
    ASSERT_NO_FATAL_FAILURE(write_at(record.offset, header));
    EXPECT_EQ(reread(record).domain(), Code::Unsupported);
  }

  // Sets the block count `record`'s frame claims to `blocks`, with its
  // payload's and its header's CRCs rewritten, so the record still reads.
  void set_block_count(const stream::Record& record, std::uint32_t blocks) {
    const auto payload = static_cast<std::size_t>(record.size);
    std::vector<std::uint8_t> chunk =
        read_at(record.offset, kRecordHeader + payload);
    put_u32(chunk, kRecordHeader + kBlockCountAt, blocks);
    put_u32(chunk, kBodyCrcAt, crc32(chunk.data() + kRecordHeader, payload));
    put_u32(chunk, kHeaderCrcAt, crc32(chunk.data(), kHeaderCrcAt));
    ASSERT_NO_FATAL_FAILURE(write_at(record.offset, chunk));
    const core::Status reread_status = reread(record);
    EXPECT_TRUE(reread_status.ok()) << reread_status.message();
  }

  // Flips a byte near the end of `record`'s frame, in its last segment's
  // rANS stream (grid's frames end with their PAYLOAD section), with its
  // payload's and its header's CRCs rewritten, so the record still reads and
  // its header still parses: only decoding it on the device finds the fault.
  void corrupt_segments(const stream::Record& record) {
    const auto payload = static_cast<std::size_t>(record.size);
    std::vector<std::uint8_t> chunk =
        read_at(record.offset, kRecordHeader + payload);
    chunk[kRecordHeader + payload - 2] ^= 0x5A;
    put_u32(chunk, kBodyCrcAt, crc32(chunk.data() + kRecordHeader, payload));
    put_u32(chunk, kHeaderCrcAt, crc32(chunk.data(), kHeaderCrcAt));
    ASSERT_NO_FATAL_FAILURE(write_at(record.offset, chunk));
    const core::Status reread_status = reread(record);
    EXPECT_TRUE(reread_status.ok()) << reread_status.message();
  }

  // Steps once and checks the frame that comes is the stream's frame
  // `frame`, stamped `index`.
  void step_to(StreamPlayer& player, std::uint64_t frame, std::uint64_t index) {
    player.step();
    const std::optional<FrameStamp> got = next_frame();
    ASSERT_TRUE(got.has_value()) << "no frame " << frame;
    EXPECT_EQ(got->index, index);
    EXPECT_EQ(got->pts_ns, pts_of(frame));
  }
  void step_to(StreamPlayer& player, std::uint64_t frame) {
    step_to(player, frame, frame);
  }

  Scratch scratch_;
  // The test's copy of the stream it plays.
  std::string path_;
  // The stream's frame that meshes nothing, if any.
  std::optional<std::uint64_t> empty_frame_;
  // The device timing start() sets.
  DeviceTiming timing_ = DeviceTiming::kOff;
  FrameExchange exchange_;
  std::uint32_t slot_ = 0;
  // The hooks start() gives the worker, when set, and what they note.
  std::optional<FrameHooks> hooks_;
  HookLog log_;
  // compacting_hooks' reads, under compacted_mutex_; a deque, as each is a
  // readback's destination until its batch has finished.
  std::mutex compacted_mutex_;
  std::deque<Compacted> compacted_;
  // Destroyed first: its worker uses the exchange and the hooks.
  std::unique_ptr<StreamPlayer> player_;
};

TEST_F(StreamPlayerTest, TheTracksCountsAndRateAreWhatWasWritten) {
  const std::vector<stream::Record> all = records();
  ASSERT_EQ(all.size(), kFrames);
  std::uint64_t bytes = 0;
  for (const stream::Record& record : all) {
    bytes += record.size;
  }
  core::Result<std::unique_ptr<StreamPlayer>> opened =
      StreamPlayer::open(path_);
  ASSERT_TRUE(opened.ok()) << opened.status().message();
  const stream::TrackStats& track = (*opened)->track_stats();
  EXPECT_EQ(track.records, kFrames);
  EXPECT_EQ(track.keys, (kFrames + kIntraPeriod - 1) / kIntraPeriod);
  EXPECT_EQ(track.payload_bytes, bytes);
  EXPECT_EQ(track.min_pts_ns, pts_of(0));
  EXPECT_EQ(track.max_pts_ns, pts_of(kFrames - 1));
  EXPECT_EQ((*opened)->rate().num, 25u);
  EXPECT_EQ((*opened)->rate().den, 1u);
}

// Untimed, as until set_device_timing asks: each frame's read, decode, mesh,
// submit, wait for its batch and finish have host time, the wait all of it,
// and none a device half; and the worker's device counts its submits, its
// waits and their time, its staging buffers, dispatches and barriers, each
// frame's its own.
TEST_F(StreamPlayerTest, PlaysEachFrameInOrderWhenStepped) {
  const std::vector<stream::Record> all = records();
  ASSERT_EQ(all.size(), kFrames);
  const core::DeviceCounters before = device().counters();
  core::DeviceCounters frames;  // the frames' own, summed
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  EXPECT_EQ(player->device_timing(), DeviceTiming::kOff);
  EXPECT_EQ(player->frame_count(), kFrames);
  EXPECT_EQ(player->track(), 0u);
  EXPECT_FALSE(next_frame(kQuiet).has_value()) << "paused, nothing goes";
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    player->step();
    const std::optional<FrameStamp> frame = next_frame();
    ASSERT_TRUE(frame.has_value()) << "no frame " << i;
    EXPECT_EQ(frame->index, i);
    EXPECT_EQ(frame->pass, 1u);
    EXPECT_EQ(frame->pts_ns, pts_of(i));
    EXPECT_EQ(frame->bytes, all[i].size);
    EXPECT_EQ(frame->key, all[i].info.key);
    EXPECT_EQ(frame->key, i % kIntraPeriod == 0);
    // Its record's set; kNoSet for the record that names none.
    EXPECT_EQ(frame->set, all[i].info.set);
    EXPECT_EQ(frame->set, vp_test::set_of(i));
    EXPECT_FALSE(frame->timed);
    const std::vector<core::StageRow>& rows = frame->stages.rows();
    ASSERT_EQ(rows.size(), 6u) << "frame " << i;
    EXPECT_STREQ(rows[0].name, "read");
    EXPECT_STREQ(rows[1].name, "decode");
    EXPECT_STREQ(rows[2].name, "mesh");
    EXPECT_STREQ(rows[3].name, "submit");
    EXPECT_STREQ(rows[4].name, "wait: GPU");
    EXPECT_STREQ(rows[5].name, "finish");
    for (const core::StageRow& row : rows) {
      EXPECT_GT(row.cpu_ms, 0.0) << row.name << ", frame " << i;
      EXPECT_LE(row.wait_ms, row.cpu_ms) << row.name << ", frame " << i;
      EXPECT_EQ(row.device, core::DeviceTime::None) << row.name;
    }
    EXPECT_EQ(rows[4].wait_ms, rows[4].cpu_ms) << "a wait is all wait";
    // The test's own readback of the mesh before counts too, so at least
    // these.
    const core::DeviceCounters& counted = frame->counters;
    EXPECT_GE(counted.submits, 1u) << "frame " << i;
    EXPECT_GT(counted.dispatches, 0u) << "frame " << i;
    EXPECT_GT(counted.barriers, 0u) << "frame " << i;
    frames.submits += counted.submits;
    frames.host_waits += counted.host_waits;
    frames.blocked_ns += counted.blocked_ns;
    frames.staging_buffers += counted.staging_buffers;
    frames.dispatches += counted.dispatches;
    frames.barriers += counted.barriers;
  }
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
  // Each frame's are its own, not the device's since it began.
  const core::DeviceCounters after = device().counters();
  EXPECT_LE(frames.submits, after.submits - before.submits);
  EXPECT_LE(frames.host_waits, after.host_waits - before.host_waits);
  EXPECT_LE(frames.blocked_ns, after.blocked_ns - before.blocked_ns);
  EXPECT_LE(frames.staging_buffers,
            after.staging_buffers - before.staging_buffers);
  EXPECT_LE(frames.dispatches, after.dispatches - before.dispatches);
  EXPECT_LE(frames.barriers, after.barriers - before.barriers);
}

// Timed, every frame's decode and mesh have a device half, where the queue
// can time measured, each over grid's breakdowns, which follow its row and
// whose tiles it holds, folded in once from its batch; read, submit, the
// wait and finish have none.
TEST_F(StreamPlayerTest, ATimedFramesDecodeAndMeshHoldGridsBreakdowns) {
  // grid's timers are private; one on the same device times where they can.
  core::Result<core::GpuTimer> timer = core::GpuTimer::create(device());
  ASSERT_TRUE(timer.ok()) << timer.status().message();
  const core::DeviceTime expected = timer.value().available()
                                        ? core::DeviceTime::Measured
                                        : core::DeviceTime::Unavailable;
  timing_ = DeviceTiming::kAll;
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    SCOPED_TRACE(i);
    player->step();
    const std::optional<FrameStamp> frame = next_frame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(frame->timed);
    const std::vector<core::StageRow>& rows = frame->stages.rows();
    ASSERT_GE(rows.size(), 5u);
    EXPECT_STREQ(rows[0].name, "read");
    EXPECT_EQ(rows[0].device, core::DeviceTime::None);
    // Each stage, then its breakdowns, whose device time it holds.
    std::size_t at = 1;
    for (const char* stage : {"decode", "mesh"}) {
      ASSERT_LT(at, rows.size());
      const core::StageRow& row = rows[at++];
      EXPECT_STREQ(row.name, stage);
      EXPECT_EQ(row.device, expected) << stage;
      double breakdowns_ms = 0.0;
      int breakdowns = 0;
      for (; at < rows.size() && std::strncmp(rows[at].name, "  ..", 4) == 0;
           ++at) {
        EXPECT_EQ(rows[at].device, expected) << rows[at].name;
        breakdowns_ms += rows[at].gpu_ms;
        ++breakdowns;
      }
      EXPECT_GE(breakdowns, 1) << stage;
      if (expected == core::DeviceTime::Measured) {
        EXPECT_GT(row.gpu_ms, 0.0) << stage;
        EXPECT_GE(row.gpu_ms + 1e-9, breakdowns_ms) << stage;
      }
    }
    for (const char* host : {"submit", "wait: GPU", "finish"}) {
      ASSERT_LT(at, rows.size());
      EXPECT_STREQ(rows[at].name, host);
      EXPECT_EQ(rows[at++].device, core::DeviceTime::None) << host;
    }
    EXPECT_EQ(at, rows.size()) << "nothing after finish";
    EXPECT_STREQ(rows[2].name, "  ..rans decode");
  }
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// Sampled, one frame in kSampleEvery is timed, from the first; and timing
// set while paused applies from the next frame the worker decodes.
TEST_F(StreamPlayerTest, SampledTimesOneFrameInKSampleEveryFromTheFirst) {
  timing_ = DeviceTiming::kSampled;
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    player->step();
    const std::optional<FrameStamp> frame = next_frame();
    ASSERT_TRUE(frame.has_value()) << "no frame " << i;
    EXPECT_EQ(frame->timed, i % volumetric_kit::player::kSampleEvery == 0)
        << "frame " << i;
    // A timed frame has grid's breakdowns too.
    EXPECT_EQ(frame->stages.rows().size() > 6u, frame->timed) << "frame " << i;
  }
  // Paused, the worker has read the next frame but records it once a step
  // makes it due, so it is timed as now set, and the one after it too.
  player->set_device_timing(DeviceTiming::kAll);
  for (int i = 0; i < 2; ++i) {
    player->step();
    const std::optional<FrameStamp> timed = next_frame();
    ASSERT_TRUE(timed.has_value());
    EXPECT_TRUE(timed->timed) << "frame " << timed->index;
  }
}

// Looping, each pass of the track counts from 1.
TEST_F(StreamPlayerTest, EachPassOfTheTrackIsCounted) {
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  for (std::uint64_t i = 0; i < 2 * kFrames + 1; ++i) {
    player->step();
    const std::optional<FrameStamp> frame = next_frame();
    ASSERT_TRUE(frame.has_value()) << "no frame " << i;
    EXPECT_EQ(frame->index, i % kFrames);
    EXPECT_EQ(frame->pass, 1 + i / kFrames) << "frame " << i;
  }
}

TEST_F(StreamPlayerTest, AFrameThatMeshesNothingReachesTheCommitWithItsStamp) {
  path_ = scratch_.copy(VP_TEST_STREAMS, vp_test::kEmptyFrameStream);
  ASSERT_FALSE(path_.empty());
  empty_frame_ = vp_test::kEmptyFrame;
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 0));
  // Frame 1 is committed through the commit, with its stamp (next_frame
  // checks both), and the live mesh is then the empty one.
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 1));
  EXPECT_TRUE(exchange_.live().empty());
  // The frames after it mesh again.
  for (std::uint64_t i = 2; i < kFrames; ++i) {
    ASSERT_NO_FATAL_FAILURE(step_to(*player, i));
  }
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, PlayingNoFrameArrivesBeforeItsPts) {
  // Lower bounds only, which a slow machine cannot break: the first frame is
  // due at once, and a late frame only makes those after it later.
  const SteadyClock::time_point t0 = SteadyClock::now();
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    const std::optional<FrameStamp> frame = next_frame();
    const auto arrived_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            SteadyClock::now() - t0)
            .count());
    ASSERT_TRUE(frame.has_value()) << "no frame " << i;
    EXPECT_EQ(frame->index, i);
    EXPECT_GE(arrived_ns, pts_of(i) - pts_of(0)) << "frame " << i;
  }
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// A frame counts as shown when it was due, not when its decode ends, so a
// slow decode does not push the frames after it back: the frame before the
// pause, its decode held past the next one's due time, lets the next go at
// once rather than the pause after that decode.
TEST_F(StreamPlayerTest, ASlowDecodeDoesNotDelayTheFramesAfterIt) {
  constexpr std::uint64_t kHeld = vp_test::kGapAt - 1;
  // Frame kHeld is due at play, and the next this long after it.
  const auto next_due =
      std::chrono::nanoseconds(pts_of(kHeld + 1) - pts_of(kHeld));
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  for (std::uint64_t i = 0; i < kHeld; ++i) {
    ASSERT_NO_FATAL_FAILURE(step_to(*player, i));
  }
  {
    // Held here, the device's submit mutex keeps frame kHeld's decode from
    // ending until the next frame is due.
    std::unique_lock<std::mutex> queue(*device().submit_mutex());
    player->play();
    std::this_thread::sleep_for(next_due + 200ms);
  }
  const std::optional<FrameStamp> held = next_frame();
  ASSERT_TRUE(held.has_value()) << "no frame " << kHeld;
  EXPECT_EQ(held->index, kHeld);
  const std::optional<FrameStamp> next = next_frame();
  ASSERT_TRUE(next.has_value()) << "no frame " << kHeld + 1;
  EXPECT_EQ(next->index, kHeld + 1);
  EXPECT_LT(next->published - held->published, next_due / 2)
      << "the next frame waited out its pts gap after the held decode";
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, WithLoopOnAStepPastTheEndWraps) {
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  EXPECT_TRUE(player->loop());
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    ASSERT_NO_FATAL_FAILURE(step_to(*player, i));
  }
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 0));
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 1));
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, WithLoopOffTheLastFrameHoldsUntilPlay) {
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  player->set_loop(false);
  EXPECT_FALSE(player->loop());
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    ASSERT_NO_FATAL_FAILURE(step_to(*player, i));
  }
  player->step();
  EXPECT_FALSE(next_frame(kQuiet).has_value()) << "a step past the end";
  EXPECT_FALSE(player->playing());
  // Play restarts from the first frame.
  player->play();
  const std::optional<FrameStamp> first = next_frame();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->index, 0u);
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, WithLoopOffPlayAfterSteppingToTheEndRestarts) {
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  player->set_loop(false);
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    ASSERT_NO_FATAL_FAILURE(step_to(*player, i));
  }
  std::this_thread::sleep_for(kQuiet);  // the worker reaches the end
  player->play();
  const std::optional<FrameStamp> first = next_frame();
  ASSERT_TRUE(first.has_value()) << "the first play brought no frame";
  EXPECT_EQ(first->index, 0u);
  EXPECT_TRUE(player->playing());
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, WithLoopOffPlayJustAfterSteppingToTheEndRestarts) {
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  player->set_loop(false);
  for (std::uint64_t i = 0; i + 2 < kFrames; ++i) {
    ASSERT_NO_FATAL_FAILURE(step_to(*player, i));
  }
  // Both last frames stepped onto, the worker holds the last until the
  // consumer takes the one before, so the play comes before it is at the end.
  player->step();
  player->step();
  std::this_thread::sleep_for(kQuiet);
  player->play();
  for (const std::uint64_t frame :
       {kFrames - 2, kFrames - 1, std::uint64_t{0}}) {
    const std::optional<FrameStamp> got = next_frame();
    ASSERT_TRUE(got.has_value()) << "no frame " << frame;
    EXPECT_EQ(got->index, frame);
  }
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, PlayingIntoTheEndWithLoopOffPausesOnTheLastFrame) {
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  player->set_loop(false);
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    const std::optional<FrameStamp> frame = next_frame();
    ASSERT_TRUE(frame.has_value()) << "no frame " << i;
    EXPECT_EQ(frame->index, i);
  }
  const SteadyClock::time_point deadline = SteadyClock::now() + kArrival;
  while (player->playing() && SteadyClock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_FALSE(player->playing());
  EXPECT_FALSE(next_frame(kQuiet).has_value());
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, ACorruptPFrameResyncsAtTheNextKey) {
  const std::vector<stream::Record> all = records();
  ASSERT_EQ(all.size(), kFrames);
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    EXPECT_EQ(all[i].info.key, i % kIntraPeriod == 0) << "record " << i;
  }
  ASSERT_NO_FATAL_FAILURE(corrupt_payload(all[1]));
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 0));
  // Frame 1 fails its CRC and frame 2 has lost its reference: one step
  // brings frame 3, the next key.
  for (std::uint64_t i = 3; i < kFrames; ++i) {
    ASSERT_NO_FATAL_FAILURE(step_to(*player, i));
  }
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// A P-frame whose rANS data is damaged reads and parses, so its batch runs,
// and grid refuses it on the device: the shown mesh stays, its extract, run
// gated off, gives its ring slot back unpublished, and the frame after, which
// lost its reference, is skipped, so playback resumes at the next key. Over
// more passes than the ring has slots, no slot is lost.
TEST_F(StreamPlayerTest, ARefusedFrameReturnsItsMeshSlot) {
  constexpr std::uint32_t kSlots = kFramesInFlight + 2;
  const std::vector<stream::Record> all = records();
  ASSERT_EQ(all.size(), kFrames);
  ASSERT_NO_FATAL_FAILURE(corrupt_segments(all[1]));
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  for (std::uint64_t pass = 1; pass <= kSlots + 1; ++pass) {
    for (const std::uint64_t frame : {std::uint64_t{0}, std::uint64_t{3},
                                      std::uint64_t{4}, std::uint64_t{5}}) {
      const std::optional<FrameStamp> got = take();
      ASSERT_TRUE(got.has_value()) << "pass " << pass << ", frame " << frame;
      EXPECT_EQ(got->index, frame) << "pass " << pass;
      EXPECT_EQ(got->pass, pass);
      if (frame == 3 && pass > 1) {
        // Frame 1's batch ran, then frame 3's; in the first pass, buffers
        // grown to the stream add their own.
        EXPECT_EQ(got->counters.submits, 2u) << "pass " << pass;
      }
    }
  }
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, ADamagedRecordHeaderResyncsAtTheNextKey) {
  const std::vector<stream::Record> all = records();
  ASSERT_EQ(all.size(), kFrames);
  ASSERT_NO_FATAL_FAILURE(corrupt_header(all[1]));
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 0));
  // Frame 1 is not read, so not counted, and frame 2 has lost its reference:
  // one step brings frame 3, the next key, stamped one low.
  for (std::uint64_t i = 3; i < kFrames; ++i) {
    ASSERT_NO_FATAL_FAILURE(step_to(*player, i, i - 1));
  }
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, ARecordThisVersionCannotReadIsSkipped) {
  const std::vector<stream::Record> all = records();
  ASSERT_EQ(all.size(), kFrames);
  ASSERT_NO_FATAL_FAILURE(set_unknown_flag(all[1]));
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 0));
  for (std::uint64_t i = 3; i < kFrames; ++i) {
    ASSERT_NO_FATAL_FAILURE(step_to(*player, i, i - 1));
  }
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, WithNoFrameToShowTheWorkerStopsWithTheReason) {
  // Every key corrupt: no frame decodes.
  const std::vector<stream::Record> all = records();
  ASSERT_EQ(all.size(), kFrames);
  for (const stream::Record& record : all) {
    if (record.info.key) {
      ASSERT_NO_FATAL_FAILURE(corrupt_payload(record));
    }
  }
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  EXPECT_TRUE(player->loop());
  const SteadyClock::time_point deadline = SteadyClock::now() + kArrival;
  while (player->status().ok() && SteadyClock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  // The first refusal, frame 0's CRC, rather than the P-frames' after it.
  const core::Status status = player->status();
  EXPECT_EQ(status.domain(), Code::InvalidArgument) << status.message();
  EXPECT_NE(status.message().find("no frame of the geometry track decodes"),
            std::string::npos)
      << status.message();
  EXPECT_FALSE(next_frame(kQuiet).has_value());
  EXPECT_LT(stop_time(*player), kPrompt);
}

// Every key claims one block past the ceiling: grid refuses each before it
// allocates for it, as a frame to skip, so the pass shows nothing and the
// worker stops with the first refusal, which names the ceiling the player
// passed.
TEST_F(StreamPlayerTest, KeysPastTheBlockCeilingStopTheWorkerWithTheReason) {
  using volumetric_kit::player::kMaxFrameBlocks;
  const std::vector<stream::Record> all = records();
  ASSERT_EQ(all.size(), kFrames);
  for (const stream::Record& record : all) {
    if (record.info.key) {
      ASSERT_NO_FATAL_FAILURE(set_block_count(record, kMaxFrameBlocks + 1));
    }
  }
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  const SteadyClock::time_point deadline = SteadyClock::now() + kArrival;
  while (player->status().ok() && SteadyClock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  const core::Status status = player->status();
  EXPECT_EQ(status.domain(), Code::InvalidArgument) << status.message();
  EXPECT_NE(status.message().find("no frame of the geometry track decodes"),
            std::string::npos)
      << status.message();
  EXPECT_NE(
      status.message().find("ceiling of " + std::to_string(kMaxFrameBlocks)),
      std::string::npos)
      << status.message();
  EXPECT_FALSE(next_frame(kQuiet).has_value());
  EXPECT_LT(stop_time(*player), kPrompt);
}

TEST_F(StreamPlayerTest, QuitIsPromptWhilePlaying) {
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(next_frame().has_value());
  ASSERT_TRUE(next_frame().has_value());
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, QuitIsPromptWhilePaused) {
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 0));
  std::this_thread::sleep_for(kQuiet);  // the worker waits for a step
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, AConsumerTakingNothingStallsTheWorkerUntilQuit) {
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(next_frame().has_value());
  // Frame 1 is published and never taken, so the worker waits to mesh frame
  // 2 rather than run out of ring slots.
  std::this_thread::sleep_for(kQuiet);
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// The worker sleeps until taken() says the last mesh was taken, rather than
// polling the exchange: a take it is not told of leaves the next publish
// waiting, and taken() lets it go.
TEST_F(StreamPlayerTest, TheWorkerSleepsUntilTakenRatherThanPolling) {
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 0));
  // Frame 1 goes and is left untaken, so frame 2, stepped too, waits for the
  // consumer before its publish; the sleep only lets the worker get there.
  player->step();
  player->step();
  std::this_thread::sleep_for(kQuiet);
  // The next mesh within `wait`, taken as next_frame takes it but untold.
  const auto take_untold = [&](SteadyClock::duration wait) {
    std::optional<FrameStamp> got;
    const SteadyClock::time_point deadline = SteadyClock::now() + wait;
    while (!got && SteadyClock::now() < deadline) {
      exchange_.begin_frame(slot_, [&](const auto&, FrameStamp& stamp) {
        got = stamp;
        return true;
      });
      slot_ = (slot_ + 1) % kFramesInFlight;
      std::this_thread::sleep_for(1ms);
    }
    return got;
  };
  const std::optional<FrameStamp> one = take_untold(kArrival);
  ASSERT_TRUE(one.has_value());
  EXPECT_EQ(one->index, 1u);
  EXPECT_FALSE(take_untold(kQuiet).has_value()) << "meshed before taken()";
  player->taken();
  const std::optional<FrameStamp> two = next_frame();
  ASSERT_TRUE(two.has_value()) << "nothing after taken()";
  EXPECT_EQ(two->index, 2u);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// A frame's decode and mesh, as one command batch records them: a P-frame's
// and a key's of the tests' stream, each sphere's blocks moved a block, with
// the deblock's LG filter, in place where device-mapped memory is cached and
// copied elsewhere (its output copies take one barrier more). Pinned, as
// Apple GPUs step in time near 80 of either (DECISIONS.md).
struct BatchCounts {
  std::uint64_t dispatches;
  std::uint64_t barriers;  // in place
};
constexpr BatchCounts kPFrameBatch{39, 47};
constexpr BatchCounts kKeyBatch{39, 47};

// Played, every frame of a steady pass, the second, is one batch: one
// submit, no staging buffer, at most one wait that blocked (its fence, which
// has normally signalled by its due time), and the dispatches and barriers
// its kind records. Taken with no device work of the test's.
TEST_F(StreamPlayerTest, OneBatchPerFrame) {
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  const bool in_place = device().caps().device_mapped_cached();
  std::uint64_t seen = 0;
  while (seen < kFrames) {
    const std::optional<FrameStamp> frame = take();
    ASSERT_TRUE(frame.has_value()) << "no frame after " << seen;
    if (frame->pass < 2) {
      continue;
    }
    ++seen;
    SCOPED_TRACE(::testing::Message() << "frame " << frame->index);
    const core::DeviceCounters& counted = frame->counters;
    EXPECT_EQ(counted.submits, 1u);
    EXPECT_EQ(counted.staging_buffers, 0u);
    EXPECT_LE(counted.host_waits, 1u);
    const BatchCounts expected = frame->key ? kKeyBatch : kPFrameBatch;
    EXPECT_EQ(counted.dispatches, expected.dispatches);
    EXPECT_EQ(counted.barriers, expected.barriers + (in_place ? 0 : 1));
    RecordProperty(std::string(frame->key ? "key" : "p") + "_frame_batch",
                   std::to_string(counted.dispatches) + " dispatches, " +
                       std::to_string(counted.barriers) + " barriers");
    // Its rounds on the device, and its latency's first leg.
    EXPECT_GE(frame->rounds, 1u);
    EXPECT_LE(frame->rounds, grid::volume::VoxelHashMap::kRecordedRounds);
    EXPECT_NE(frame->due, SteadyClock::time_point{});
    EXPECT_LE(frame->due, frame->published);
  }
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// A frame's due time is the one the clock set, not when the worker woke to
// it: each frame's is its pts step after the frame before's, unless that one
// was shown over a period late, which moves the clock on (Playback).
TEST_F(StreamPlayerTest, AFramesDueTimeIsTheOneTheClockSet) {
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  std::optional<FrameStamp> before = take();
  ASSERT_TRUE(before.has_value());
  std::uint64_t compared = 0;
  for (std::uint64_t i = 1; i < 2 * kFrames; ++i) {
    std::optional<FrameStamp> frame = take();
    ASSERT_TRUE(frame.has_value()) << "no frame " << i;
    if (frame->pass == before->pass &&
        before->published - before->due <=
            std::chrono::nanoseconds(kPeriodNs)) {
      EXPECT_EQ(frame->due - before->due,
                std::chrono::nanoseconds(pts_of(frame->index) -
                                         pts_of(before->index)))
          << "frame " << frame->index << " of pass " << frame->pass;
      ++compared;
    }
    before = std::move(frame);
  }
  EXPECT_GT(compared, 0u);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// A frame seen over a period late moves the clock on: the frame after it is
// due its pts step after the late one was seen, not after the late one's due
// time, so the frames behind it follow rather than come in a burst
// (Playback). Frame 2 left untaken holds the worker in frame 3's publish,
// so it sees frame 4 due no sooner than frame 2 is taken.
TEST_F(StreamPlayerTest, AFrameSeenOverAPeriodLateMovesTheClockOn) {
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  std::optional<FrameStamp> frame;
  while (!frame || frame->index < 1) {
    frame = take();
    ASSERT_TRUE(frame.has_value());
  }
  // Past frame 4's due time, frame 1's plus three steps, and a period on.
  std::this_thread::sleep_for(
      std::chrono::nanoseconds(pts_of(4) - pts_of(1) + 2 * kPeriodNs) + 250ms);
  const SteadyClock::time_point taken = SteadyClock::now();
  std::optional<FrameStamp> late;
  while (!frame || frame->index < 5) {
    frame = take();
    ASSERT_TRUE(frame.has_value());
    if (frame->index == 4) {
      late = frame;
    }
  }
  ASSERT_TRUE(late.has_value());
  const auto period = std::chrono::nanoseconds(kPeriodNs);
  if (taken - late->due <= period) {
    GTEST_SKIP() << "frame 4 was not seen a period late";
  }
  EXPECT_GE(
      frame->due - late->due,
      std::chrono::nanoseconds(pts_of(5) - pts_of(4)) + (taken - late->due))
      << "frame 5 is due its step after frame 4's due time, not after frame "
         "4 was seen";
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// Every mesh carries the value its batch set on the extractor's timeline,
// reached by its hand-over (take checks), each frame's past the one before.
TEST_F(StreamPlayerTest, EveryMeshCarriesAReachedReadyValue) {
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  std::uint64_t last = 0;
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    const std::optional<FrameStamp> frame = take();
    ASSERT_TRUE(frame.has_value()) << "no frame " << i;
    const core::TimelinePoint& ready = exchange_.live().ready;
    EXPECT_GT(ready.value, last) << "frame " << i;
    last = ready.value;
  }
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// Timed, a frame's batch shows the queue's idle since the worker's batch
// before when both were timed and nothing came between: not the first
// frame's, and in the steady pass, the second, every frame's; and its queue
// latency where the device calibrates its clocks. Untimed, neither. Both are
// in ms: the idle under a second, as the stream's longest pts step is
// kGapNs + kPeriodNs, and the latency, a submit's, under 100 ms (a
// calibration may place the first timestamp a little before the submit
// call, by its deviation).
TEST_F(StreamPlayerTest, DeviceIdleNeedsTwoTimedBatchesInARow) {
  constexpr double kMaxIdleMs = 1000.0;
  constexpr double kMaxQueueMs = 100.0;
  constexpr double kCalibrationMs = 1.0;
  core::Result<core::GpuTimer> timer = core::GpuTimer::create(device());
  ASSERT_TRUE(timer.ok()) << timer.status().message();
  const bool times = timer.value().available();
  const bool calibrates = device().calibrate().ok();
  timing_ = DeviceTiming::kAll;
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  for (std::uint64_t i = 0; i < 2 * kFrames; ++i) {
    const std::optional<FrameStamp> frame = take();
    ASSERT_TRUE(frame.has_value()) << "no frame " << i;
    EXPECT_TRUE(frame->timed);
    if (i == 0) {
      EXPECT_FALSE(frame->device_idle_ms.has_value()) << "no batch before";
    } else if (frame->pass == 2) {
      EXPECT_EQ(frame->device_idle_ms.has_value(), times) << "frame " << i;
    }
    if (frame->device_idle_ms) {
      EXPECT_GE(*frame->device_idle_ms, 0.0);
      EXPECT_LT(*frame->device_idle_ms, kMaxIdleMs) << "frame " << i;
    }
    EXPECT_EQ(frame->queue_ms.has_value(), times && calibrates)
        << "frame " << i;
    if (frame->queue_ms) {
      EXPECT_GT(*frame->queue_ms, -kCalibrationMs) << "frame " << i;
      EXPECT_LT(*frame->queue_ms, kMaxQueueMs) << "frame " << i;
    }
  }
  player->set_device_timing(DeviceTiming::kOff);
  // The frame after the next is recorded untimed.
  std::optional<FrameStamp> untimed;
  for (int i = 0; i < 3 && (!untimed || untimed->timed); ++i) {
    untimed = take();
    ASSERT_TRUE(untimed.has_value());
  }
  EXPECT_FALSE(untimed->timed);
  EXPECT_FALSE(untimed->device_idle_ms.has_value());
  EXPECT_FALSE(untimed->queue_ms.has_value());
  // Timed again, the first timed frame follows an untimed batch: no idle.
  player->set_device_timing(DeviceTiming::kAll);
  std::optional<FrameStamp> timed;
  for (int i = 0; i < 3 && (!timed || !timed->timed); ++i) {
    timed = take();
    ASSERT_TRUE(timed.has_value());
  }
  EXPECT_TRUE(timed->timed);
  EXPECT_FALSE(timed->device_idle_ms.has_value()) << "an untimed batch before";
  // Stepped, with the test's own readback of each mesh submitted between two
  // frames' batches: no idle either.
  player->pause();
  for (std::uint64_t i = 0; i < 3; ++i) {
    player->step();
    const std::optional<FrameStamp> stepped = next_frame();
    ASSERT_TRUE(stepped.has_value()) << "step " << i;
    if (stepped->timed && i > 0) {
      EXPECT_FALSE(stepped->device_idle_ms.has_value())
          << "a submit between, step " << i;
    }
  }
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// Paused, a frame is recorded only once it is due: in the steady pass, the
// second, a step's frame is its one batch, after which the worker reads the
// next record and submits nothing until the next step.
TEST_F(StreamPlayerTest, NothingIsRecordedAheadWhilePaused) {
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    ASSERT_NO_FATAL_FAILURE(step_to(*player, i));
  }
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 0));
  const std::uint64_t submits = device().counters().submits;
  player->step();
  const std::optional<FrameStamp> one = take();
  ASSERT_TRUE(one.has_value());
  EXPECT_EQ(one->index, 1u);
  EXPECT_EQ(one->pass, 2u);
  std::this_thread::sleep_for(kQuiet);  // the worker reads frame 2
  EXPECT_EQ(device().counters().submits - submits, 1u)
      << "frame 2 recorded ahead while paused";
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 2));
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// Playing, the frame after the one taken is recorded at once, before its due
// time: past the stream's half-second pause, frame kGapAt. A pause then
// publishes it at once, not when it was due, and nothing more goes, so the
// grid holds the frame shown, which a toggle rebuilds (DECISIONS.md).
TEST_F(StreamPlayerTest, APauseShowsTheFrameInHandThenHolds) {
  ASSERT_NO_FATAL_FAILURE(use_true_sdf());
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  std::optional<FrameStamp> frame;
  while (!frame || frame->index + 1 < vp_test::kGapAt) {
    frame = take();
    ASSERT_TRUE(frame.has_value());
  }
  // Frame kGapAt is due kGapNs on; the worker records it meanwhile.
  if (!next_batch_in_flight(*frame)) {
    GTEST_SKIP() << "frame " << vp_test::kGapAt << " was not in flight "
                 << "well ahead of its due time";
  }
  const SteadyClock::time_point paused = SteadyClock::now();
  player->pause();
  const std::optional<FrameStamp> held = next_frame();
  ASSERT_TRUE(held.has_value()) << "the pause published no frame";
  EXPECT_EQ(held->index, vp_test::kGapAt);
  EXPECT_LT(SteadyClock::now() - paused,
            std::chrono::nanoseconds(vp_test::kGapNs / 2))
      << "published when due, not at the pause";
  EXPECT_FALSE(next_frame(kQuiet).has_value()) << "paused, nothing else goes";
  player->set_deblock(false);
  ASSERT_NO_FATAL_FAILURE(expect_rebuild(vp_test::kGapAt, false, "off"));
  EXPECT_FALSE(player->playing());
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// Parked with a frame's batch in flight, recorded ahead of its due time, the
// park waits that batch out and returns at once; nothing is submitted or
// published while parked, past its due time; at unpark the frame is finished
// and published, with no other batch.
TEST_F(StreamPlayerTest,
       AParkWithABatchInFlightSubmitsNothingAndPublishesAfterUnpark) {
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  // In the steady pass, the second, so the frame is its one batch.
  std::optional<FrameStamp> frame;
  while (!frame || frame->pass < 2 || frame->index + 1 < vp_test::kGapAt) {
    frame = take();
    ASSERT_TRUE(frame.has_value());
  }
  if (!next_batch_in_flight(*frame)) {
    GTEST_SKIP() << "frame " << vp_test::kGapAt << " was not in flight "
                 << "well ahead of its due time";
  }
  const std::uint64_t submits = device().counters().submits;
  const SteadyClock::time_point parking = SteadyClock::now();
  ASSERT_TRUE(player->park(kArrival));
  EXPECT_LT(SteadyClock::now() - parking, kPrompt);
  EXPECT_FALSE(take(std::chrono::nanoseconds(vp_test::kGapNs)).has_value())
      << "published while parked";
  EXPECT_EQ(device().counters().submits, submits) << "submitted while parked";
  player->unpark();
  const std::optional<FrameStamp> next = take();
  ASSERT_TRUE(next.has_value()) << "nothing after unpark";
  EXPECT_EQ(next->index, vp_test::kGapAt);
  EXPECT_EQ(next->counters.submits, 1u);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// The grow stream's frame kGrownFrame nearly fills the grid: its batch runs,
// the table refuses blocks on the device, and its extract runs gated off.
// Parked with that batch in flight, nothing is submitted; at unpark the codec
// grows the grid and decodes the frame again ("  ..grow"), the worker meshes
// it in a batch of its own, and publishes it.
TEST_F(StreamPlayerTest, AParkWhileARecoveryIsDueRunsItAtUnpark) {
  path_ = scratch_.copy(VP_TEST_STREAMS, vp_test::kGrowStream);
  ASSERT_FALSE(path_.empty());
  const std::vector<std::uint32_t> blocks = block_counts();
  ASSERT_EQ(blocks.size(), kFrames);
  ASSERT_LE(2 * blocks[0], vp_test::kFirstGridSlots) << "a grid of more slots";
  ASSERT_GT(blocks[vp_test::kGrownFrame],
            vp_test::kFirstGridSlots - vp_test::kTableFloor);
  ASSERT_LE(blocks[vp_test::kGrownFrame], vp_test::kFirstGridSlots);
  timing_ = DeviceTiming::kAll;
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  std::optional<FrameStamp> frame;
  while (!frame || frame->index + 1 < vp_test::kGrownFrame) {
    frame = take();
    ASSERT_TRUE(frame.has_value());
  }
  if (!next_batch_in_flight(*frame)) {
    GTEST_SKIP() << "frame " << vp_test::kGrownFrame << " was not in flight "
                 << "well ahead of its due time";
  }
  // From the park's call to the unpark: no submit, the park's own wait
  // included.
  const std::uint64_t submits = device().counters().submits;
  ASSERT_TRUE(player->park(kArrival));
  EXPECT_FALSE(take(std::chrono::nanoseconds(vp_test::kGapNs)).has_value())
      << "published while parked";
  EXPECT_EQ(device().counters().submits, submits) << "recovered while parked";
  player->unpark();
  const std::optional<FrameStamp> grown = take();
  ASSERT_TRUE(grown.has_value()) << "nothing after unpark";
  EXPECT_EQ(grown->index, vp_test::kGrownFrame);
  EXPECT_GT(grown->counters.submits, 1u) << "no recovery";
  bool grow_row = false;
  for (const core::StageRow& row : grown->stages.rows()) {
    grow_row = grow_row || std::strcmp(row.name, "  ..grow") == 0;
  }
  EXPECT_TRUE(grow_row) << "no \"  ..grow\" row";
  EXPECT_GT(exchange_.live().triangle_count, 0u);
  // The frames after it decode into the grown grid.
  const std::optional<FrameStamp> after = take();
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->index, vp_test::kGrownFrame + 1);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, AFatalErrorStopsTheWorker) {
  // A ring one short of mesh_wiring's: with both of the consumer's frames
  // holding a mesh and frame 2 published untaken, frame 3's extract, recorded
  // before that take, is refused.
  StreamPlayer* player =
      start(/*playing=*/false, /*slot_count=*/kFramesInFlight + 1);
  ASSERT_NE(player, nullptr);
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 0));
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 1));
  player->step();
  player->step();
  const SteadyClock::time_point deadline = SteadyClock::now() + kArrival;
  while (player->status().ok() && SteadyClock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  const core::Status status = player->status();
  EXPECT_FALSE(status.ok());
  EXPECT_NE(status.message().find("outstanding"), std::string::npos)
      << status.message();
  EXPECT_LT(stop_time(*player), kPrompt);
}

TEST_F(StreamPlayerTest, AFailedBuildOnTheWorkerReachesStatus) {
  // MarchingCubes::create refuses a ring with no slot. The worker builds the
  // extractor, so start returns OK first.
  StreamPlayer* player = start(/*playing=*/true, /*slot_count=*/0);
  ASSERT_NE(player, nullptr);
  const SteadyClock::time_point deadline = SteadyClock::now() + kArrival;
  while (player->status().ok() && SteadyClock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  const core::Status status = player->status();
  EXPECT_EQ(status.domain(), Code::InvalidArgument) << status.message();
  EXPECT_NE(status.message().find("slot_count"), std::string::npos)
      << status.message();
  EXPECT_FALSE(next_frame(kQuiet).has_value());
  // Its worker ended, but it was started.
  const core::Status again =
      player->start(device(), allocator(), extractor_config(), exchange_);
  EXPECT_EQ(again.domain(), Code::InvalidArgument) << again.message();
  EXPECT_LT(stop_time(*player), kPrompt);
}

TEST_F(StreamPlayerTest, StartIsRefusedOnceStartedOrStopped) {
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  const core::Status again =
      player->start(device(), allocator(), extractor_config(), exchange_);
  EXPECT_EQ(again.domain(), Code::InvalidArgument) << again.message();
  core::Result<std::unique_ptr<StreamPlayer>> stopped =
      StreamPlayer::open(path_);
  ASSERT_TRUE(stopped.ok()) << stopped.status().message();
  (*stopped)->stop();
  const core::Status after_stop =
      (*stopped)->start(device(), allocator(), extractor_config(), exchange_);
  EXPECT_EQ(after_stop.domain(), Code::InvalidArgument) << after_stop.message();
}

TEST_F(StreamPlayerTest, AStepDuringAParkGoesAtUnpark) {
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 0));
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 1));
  // Mid-playback: the worker reads frame 2 once frame 1 goes.
  EXPECT_TRUE(player->park(kArrival));
  player->step();
  EXPECT_FALSE(next_frame(kQuiet).has_value()) << "published while parked";
  EXPECT_FALSE(player->playing());
  player->unpark();
  const std::optional<FrameStamp> next = next_frame();
  ASSERT_TRUE(next.has_value()) << "the step did not go at unpark";
  EXPECT_EQ(next->index, 2u);
  EXPECT_FALSE(player->playing());
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, APauseDuringAParkHoldsAfterUnpark) {
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 0));
  EXPECT_TRUE(player->park(kArrival));
  // Played, then paused, while parked: frame 1 does not go.
  player->play();
  EXPECT_TRUE(player->playing());
  EXPECT_FALSE(next_frame(kQuiet).has_value()) << "published while parked";
  player->pause();
  player->unpark();
  EXPECT_FALSE(player->playing());
  EXPECT_FALSE(next_frame(kQuiet).has_value()) << "the pause did not hold";
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 1));
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, PlayingDoesNotChangeAcrossAPark) {
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  const std::optional<FrameStamp> first = next_frame();
  ASSERT_TRUE(first.has_value());
  EXPECT_TRUE(player->park(kArrival));
  EXPECT_TRUE(player->playing());
  // A frame published before the park returned may wait to be taken; no
  // other comes.
  std::uint64_t last = first->index;
  if (const std::optional<FrameStamp> pending = next_frame(kQuiet)) {
    last = pending->index;
  }
  EXPECT_FALSE(next_frame(kQuiet).has_value()) << "published while parked";
  EXPECT_TRUE(player->playing());
  player->unpark();
  EXPECT_TRUE(player->playing());
  for (std::uint64_t i = last + 1; i < last + 3; ++i) {
    const std::optional<FrameStamp> frame = next_frame();
    ASSERT_TRUE(frame.has_value()) << "no frame " << i;
    EXPECT_EQ(frame->index, i);
  }
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, AParkHoldsAMeshWaitingForTheConsumer) {
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 0));
  // Frame 1 goes and is left untaken, so frame 2, stepped too, is meshed and
  // waits for the consumer before its publish, in no step; the sleep only
  // lets the worker get there.
  player->step();
  player->step();
  std::this_thread::sleep_for(kQuiet);
  EXPECT_TRUE(player->park(kArrival));
  // Taking frame 1, published before the park, frees the worker's wait; it
  // does not publish while parked.
  std::uint64_t last = 0;
  if (const std::optional<FrameStamp> pending = next_frame(kQuiet)) {
    last = pending->index;
  }
  EXPECT_FALSE(next_frame(kQuiet).has_value()) << "published while parked";
  player->unpark();
  const std::optional<FrameStamp> next = next_frame();
  ASSERT_TRUE(next.has_value()) << "nothing after unpark";
  EXPECT_EQ(next->index, last + 1);
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, AParkReturnsOnceTheStepUnderWayEnds) {
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 0));
  // Held here, the device's submit mutex keeps frame 1's step from ending.
  std::unique_lock<std::mutex> queue(*device().submit_mutex());
  player->step();
  ASSERT_TRUE(park_mid_step(*player)) << "frame 1's step never began";
  queue.unlock();
  const SteadyClock::time_point parking = SteadyClock::now();
  EXPECT_TRUE(player->park(kArrival));
  EXPECT_LT(SteadyClock::now() - parking, kPrompt)
      << "the step's end did not wake the park";
  player->unpark();
  const std::optional<FrameStamp> next = next_frame();
  ASSERT_TRUE(next.has_value()) << "nothing after unpark";
  EXPECT_EQ(next->index, 1u);
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, AParkThatTimesOutMidDecodeLetsNothingStartAfter) {
  // Held here, the device's submit mutex keeps the worker in its step.
  std::unique_lock<std::mutex> queue(*device().submit_mutex());
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  // The kernel build cannot end, so a park times out; once it ends, the hold
  // keeps frame 0's read and decode from starting.
  ASSERT_TRUE(park_mid_step(*player)) << "the build never began";
  queue.unlock();
  ASSERT_TRUE(player->park(kArrival));
  // Frame 0's decode, let go under the held mutex, times a park out in turn.
  queue.lock();
  player->unpark();
  ASSERT_TRUE(park_mid_step(*player)) << "the decode never began";
  EXPECT_FALSE(player->park(kQuiet));
  queue.unlock();
  // Playing, frame 0 would go once decoded, but the hold stays.
  EXPECT_FALSE(next_frame(kQuiet).has_value())
      << "published after a timed-out park";
  EXPECT_TRUE(player->park(kArrival));
  EXPECT_TRUE(player->playing());
  player->unpark();
  const std::optional<FrameStamp> frame = next_frame();
  ASSERT_TRUE(frame.has_value()) << "nothing after unpark";
  EXPECT_EQ(frame->index, 0u);
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, AStopWhileParkedAtAStepsStartIsPrompt) {
  // Held here, the device's submit mutex keeps the worker in its build.
  std::unique_lock<std::mutex> queue(*device().submit_mutex());
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(park_mid_step(*player)) << "the build never began";
  queue.unlock();
  // Once the build ends, the hold keeps the worker at the start of frame 0's
  // read, with no wait for a due time between.
  ASSERT_TRUE(player->park(kArrival));
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, AParkBeforeStartOrAfterStopReturnsAtOnce) {
  core::Result<std::unique_ptr<StreamPlayer>> opened =
      StreamPlayer::open(path_);
  ASSERT_TRUE(opened.ok()) << opened.status().message();
  player_ = std::move(opened).value();
  EXPECT_TRUE(player_->park(0ms)) << "before start";
  // The hold holds from the build on: playing, nothing goes, and the worker
  // is never busy.
  const core::Status started =
      player_->start(device(), allocator(), extractor_config(), exchange_);
  ASSERT_TRUE(started.ok()) << started.message();
  EXPECT_FALSE(next_frame(kQuiet).has_value()) << "published while parked";
  EXPECT_TRUE(player_->park(0ms)) << "the worker built while parked";
  player_->unpark();
  ASSERT_TRUE(next_frame().has_value()) << "nothing after unpark";
  EXPECT_TRUE(player_->park(kArrival));
  // A stop while parked is prompt too.
  EXPECT_LT(stop_time(*player_), kPrompt);
  EXPECT_TRUE(player_->park(0ms)) << "after stop";
  EXPECT_TRUE(player_->status().ok()) << player_->status().message();
}

// The true-SDF stream has no unobserved voxel, so the deblock filters every
// block of each frame, and keeps none at its exact decode: each frame's mesh
// differs from its rebuild with the deblock off.
TEST_F(StreamPlayerTest, TheDeblockIsOnByDefaultAndEachFrameSaysWhatItDid) {
  ASSERT_NO_FATAL_FAILURE(use_true_sdf());
  const std::vector<std::uint32_t> blocks = block_counts();
  ASSERT_EQ(blocks.size(), kFrames);
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  EXPECT_TRUE(player->deblock());
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    player->step();
    const std::optional<FrameStamp> frame = next_frame();
    ASSERT_TRUE(frame.has_value()) << "no frame " << i;
    EXPECT_EQ(frame->index, i);
    EXPECT_FALSE(frame->rebuild);
    EXPECT_TRUE(frame->deblock_on);
    EXPECT_STREQ(frame->deblock.filter, "R") << "frame " << i;
    EXPECT_NEAR(frame->deblock.step, kTrueSdfStep, kStepError);
    EXPECT_GT(blocks[i], 0u);
    EXPECT_EQ(frame->deblock.block_count, blocks[i]) << "frame " << i;
    EXPECT_EQ(frame->deblock.exact_blocks, 0u) << "frame " << i;
    EXPECT_NEAR(mean_x(exchange_.live()), kMetresAFrame * i, 0.02);
    ASSERT_NO_FATAL_FAILURE(expect_deblock_against_off(i, "R", blocks[i], 0));
  }
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// The sphere's frames hold an unobserved voxel in every block (4 voxels of
// truncation, 8 a block), so the deblock's per-block fallback keeps them all
// at their exact decode, under the filter the frame's T picks, LG: no frame
// is skipped, and each mesh is its rebuild's with the deblock off.
TEST_F(StreamPlayerTest, TheDeblockKeepsTheMaskedSpheresBlocksExact) {
  const std::vector<std::uint32_t> blocks = block_counts();
  ASSERT_EQ(blocks.size(), kFrames);
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    player->step();
    const std::optional<FrameStamp> frame = next_frame();
    ASSERT_TRUE(frame.has_value()) << "no frame " << i;
    EXPECT_FALSE(frame->rebuild);
    EXPECT_TRUE(frame->deblock_on);
    EXPECT_STREQ(frame->deblock.filter, "LG") << "frame " << i;
    EXPECT_NEAR(frame->deblock.step, kSphereStep, kStepError);
    EXPECT_GT(blocks[i], 0u);
    EXPECT_EQ(frame->deblock.block_count, blocks[i]) << "frame " << i;
    EXPECT_EQ(frame->deblock.exact_blocks, blocks[i]) << "frame " << i;
    ASSERT_NO_FATAL_FAILURE(
        expect_deblock_against_off(i, "LG", blocks[i], blocks[i]));
  }
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// The worker builds its decoder after start, and the first decode takes the
// setting.
TEST_F(StreamPlayerTest, TurnedOffBeforeStartTheFirstFrameDecodesWithout) {
  ASSERT_NO_FATAL_FAILURE(use_true_sdf());
  core::Result<std::unique_ptr<StreamPlayer>> opened =
      StreamPlayer::open(path_);
  ASSERT_TRUE(opened.ok()) << opened.status().message();
  player_ = std::move(opened).value();
  player_->set_deblock(false);
  EXPECT_FALSE(player_->deblock());
  player_->pause();
  const core::Status started =
      player_->start(device(), allocator(), extractor_config(), exchange_);
  ASSERT_TRUE(started.ok()) << started.message();
  player_->step();
  const std::optional<FrameStamp> frame = next_frame();
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->index, 0u);
  EXPECT_FALSE(frame->deblock_on);
  EXPECT_STREQ(frame->deblock.filter, "off");
  EXPECT_NEAR(frame->deblock.step, kTrueSdfStep, kStepError);
  EXPECT_FALSE(next_frame(kQuiet).has_value()) << "paused, nothing goes";
  EXPECT_TRUE(player_->status().ok()) << player_->status().message();
}

// Paused on frame 1, the worker has read frame 2 but records it only when it
// is due, so the grid holds frame 1: a toggle rebuilds that, and nothing
// else goes until a step.
TEST_F(StreamPlayerTest, ToggledWhilePausedTheFrameShownIsRebuilt) {
  ASSERT_NO_FATAL_FAILURE(use_true_sdf());
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 0));
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 1));
  std::this_thread::sleep_for(kQuiet);  // the worker waits for frame 2's due
  player->set_deblock(false);
  ASSERT_NO_FATAL_FAILURE(expect_rebuild(1, false, "off"));
  EXPECT_FALSE(next_frame(kQuiet).has_value()) << "paused, nothing else goes";
  player->set_deblock(true);
  ASSERT_NO_FATAL_FAILURE(expect_rebuild(1, true, "R"));
  EXPECT_FALSE(next_frame(kQuiet).has_value()) << "paused, nothing else goes";
  EXPECT_FALSE(player->playing());
  // The next frame decodes as set.
  player->step();
  const std::optional<FrameStamp> next = next_frame();
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(next->index, 2u);
  EXPECT_FALSE(next->rebuild);
  EXPECT_STREQ(next->deblock.filter, "R");
  EXPECT_NEAR(mean_x(exchange_.live()), 2 * kMetresAFrame, 0.02);
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// A rebuild is never timed, though every frame is: its "decode", the
// rebuild's, and its "mesh" have no device half and no breakdown, and there
// is no "read". The frame after is timed as before.
TEST_F(StreamPlayerTest, ARebuildIsNeverTimed) {
  ASSERT_NO_FATAL_FAILURE(use_true_sdf());
  timing_ = DeviceTiming::kAll;
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 0));
  std::this_thread::sleep_for(kQuiet);  // the worker waits for frame 1's due
  player->set_deblock(false);
  const std::optional<FrameStamp> rebuilt = next_frame();
  ASSERT_TRUE(rebuilt.has_value()) << "no rebuild of frame 0";
  EXPECT_TRUE(rebuilt->rebuild);
  EXPECT_EQ(rebuilt->index, 0u);
  EXPECT_FALSE(rebuilt->timed);
  const std::vector<core::StageRow>& rows = rebuilt->stages.rows();
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_STREQ(rows[0].name, "decode");
  EXPECT_STREQ(rows[1].name, "mesh");
  for (const core::StageRow& row : rows) {
    EXPECT_EQ(row.device, core::DeviceTime::None) << row.name;
  }
  player->step();
  const std::optional<FrameStamp> next = next_frame();
  ASSERT_TRUE(next.has_value()) << "no frame 1";
  EXPECT_EQ(next->index, 1u);
  EXPECT_FALSE(next->rebuild);
  EXPECT_TRUE(next->timed);
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, ToggledOnTheLastFrameHeldWithLoopOffItIsRebuilt) {
  ASSERT_NO_FATAL_FAILURE(use_true_sdf());
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  player->set_loop(false);
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    ASSERT_NO_FATAL_FAILURE(step_to(*player, i));
  }
  std::this_thread::sleep_for(kQuiet);  // the worker reaches the end
  player->set_deblock(false);
  ASSERT_NO_FATAL_FAILURE(expect_rebuild(kFrames - 1, false, "off"));
  EXPECT_FALSE(next_frame(kQuiet).has_value()) << "held, nothing else goes";
  // Play restarts from the first frame, which decodes as set.
  player->play();
  const std::optional<FrameStamp> first = next_frame();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->index, 0u);
  EXPECT_FALSE(first->rebuild);
  EXPECT_FALSE(first->deblock_on);
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, AToggleDuringAParkRebuildsAtUnpark) {
  ASSERT_NO_FATAL_FAILURE(use_true_sdf());
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 0));
  EXPECT_TRUE(player->park(kArrival));
  player->set_deblock(false);
  EXPECT_FALSE(next_frame(kQuiet).has_value()) << "rebuilt while parked";
  player->unpark();
  ASSERT_NO_FATAL_FAILURE(expect_rebuild(0, false, "off"));
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// Playing, a toggle reaches the next frame recorded: the second after the
// toggle, as the next is recorded already; nothing is rebuilt.
TEST_F(StreamPlayerTest, ToggledWhilePlayingTheFramesAfterTakeIt) {
  ASSERT_NO_FATAL_FAILURE(use_true_sdf());
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  const std::optional<FrameStamp> first = next_frame();
  ASSERT_TRUE(first.has_value());
  EXPECT_TRUE(first->deblock_on);
  player->set_deblock(false);
  std::optional<FrameStamp> frame;
  for (int i = 0; i < 3; ++i) {
    frame = next_frame();
    ASSERT_TRUE(frame.has_value()) << "no frame after the toggle";
    EXPECT_FALSE(frame->rebuild);
    if (!frame->deblock_on) {
      break;
    }
  }
  EXPECT_FALSE(frame->deblock_on) << "the toggle reached no frame";
  EXPECT_STREQ(frame->deblock.filter, "off");
  for (int i = 0; i < 2; ++i) {
    const std::optional<FrameStamp> after = next_frame();
    ASSERT_TRUE(after.has_value());
    EXPECT_FALSE(after->rebuild);
    EXPECT_FALSE(after->deblock_on);
  }
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// --- Hooks (frame_hooks.hpp): the worker's, and FrameTaker's ---------------

// A view of the extractor's `buffer` of `bytes`: its deleter frees nothing.
core::Buffer borrowed(VkBuffer buffer, VkDeviceSize bytes,
                      VkBufferUsageFlags usage, VkSharingMode sharing) {
  return core::Buffer(
      buffer, bytes, usage, sharing, nullptr, [] {}, std::nullopt);
}

// The extras a test's after_mesh made, by frame, and which have been
// released: shared with each extra's deleter, which may run after the test's
// body, from the exchange's teardown.
struct Extras {
  std::mutex mutex;
  std::vector<std::uint64_t> released;  // frame indices, in release order

  // An extra for frame `index`, which notes its release.
  static std::shared_ptr<const void> make(const std::shared_ptr<Extras>& log,
                                          std::uint64_t index) {
    return std::shared_ptr<const void>(
        new std::uint64_t(index), [log](const void* extra) {
          const auto* frame = static_cast<const std::uint64_t*>(extra);
          {
            std::lock_guard<std::mutex> lock(log->mutex);
            log->released.push_back(*frame);
          }
          delete frame;
        });
  }
  bool was_released(std::uint64_t index) {
    std::lock_guard<std::mutex> lock(mutex);
    return std::find(released.begin(), released.end(), index) != released.end();
  }
};

// Stepped through a pass, then toggled while paused: after_mesh runs once
// before each publish, each frame's and then the rebuild's, with the grid its
// mesh was made from, which holds that frame's blocks, and the mesh then
// committed, ready at after_mesh's batch's point, not the extract's.
TEST_F(StreamPlayerTest, AfterMeshRunsOnceBeforeEachPublishWithItsMeshsGrid) {
  const std::vector<std::uint32_t> blocks = block_counts();
  ASSERT_EQ(blocks.size(), kFrames);
  logging_hooks();
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  std::vector<std::uint64_t> generations;
  std::vector<const core::TimelineSemaphore*> readies;  // as committed
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    player->step();
    const std::optional<FrameStamp> frame = take_with(taker);
    ASSERT_TRUE(frame.has_value()) << "no frame " << i;
    EXPECT_EQ(frame->index, i);
    generations.push_back(exchange_.live().generation);
    readies.push_back(exchange_.live().ready.semaphore);
  }
  player->set_deblock(false);
  const std::optional<FrameStamp> rebuilt = take_with(taker);
  ASSERT_TRUE(rebuilt.has_value()) << "no rebuild";
  EXPECT_TRUE(rebuilt->rebuild);
  generations.push_back(exchange_.live().generation);
  readies.push_back(exchange_.live().ready.semaphore);
  const std::vector<Meshed> meshed = log_.meshed();
  ASSERT_EQ(meshed.size(), kFrames + 1);
  for (std::size_t i = 0; i < meshed.size(); ++i) {
    SCOPED_TRACE(::testing::Message() << "publish " << i);
    const std::uint64_t frame = std::min<std::uint64_t>(i, kFrames - 1);
    EXPECT_EQ(meshed[i].index, frame);
    EXPECT_EQ(meshed[i].rebuild, i == kFrames);
    EXPECT_EQ(meshed[i].blocks, blocks[frame]);
    EXPECT_EQ(meshed[i].generation, generations[i]);
    EXPECT_GT(meshed[i].triangles, 0u);
    EXPECT_NE(readies[i], nullptr);
    EXPECT_NE(readies[i], meshed[i].ready) << "published at the extract's";
  }
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// The grow stream's frame kGrownFrame is decoded again at its finish and
// meshed in a batch of its own: after_mesh runs once for it, with the grown
// grid holding its blocks.
TEST_F(StreamPlayerTest, AfterMeshRunsForAFrameMeshedAgainAfterARedo) {
  path_ = scratch_.copy(VP_TEST_STREAMS, vp_test::kGrowStream);
  ASSERT_FALSE(path_.empty());
  const std::vector<std::uint32_t> blocks = block_counts();
  ASSERT_EQ(blocks.size(), kFrames);
  logging_hooks();
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  for (std::uint64_t i = 0; i <= vp_test::kGrownFrame; ++i) {
    player->step();
    const std::optional<FrameStamp> frame = take_with(taker);
    ASSERT_TRUE(frame.has_value()) << "no frame " << i;
    EXPECT_EQ(frame->index, i);
  }
  const std::vector<Meshed> meshed = log_.meshed();
  ASSERT_EQ(meshed.size(), vp_test::kGrownFrame + 1);
  const Meshed& grown = meshed.back();
  EXPECT_EQ(grown.index, vp_test::kGrownFrame);
  EXPECT_EQ(grown.blocks, blocks[vp_test::kGrownFrame]);
  EXPECT_EQ(grown.generation, exchange_.live().generation);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// after_mesh records work that reads the grid it is handed read-only, a
// compaction of its map, whose batch binds the map's own kernel sets until
// it has resolved. The grow stream's frame kGrownFrame grows the grid at its
// finish: the batch of the frame before has finished first, so the core
// refuses none and playback goes on, each batch counting its frame's blocks.
TEST_F(StreamPlayerTest, AfterMeshMayReadTheGridAFrameGrowsAtItsFinish) {
  ASSERT_NO_FATAL_FAILURE(use_stream(vp_test::kGrowStream));
  step_through_compacting(/*grown_at_record=*/false);
}

// As above, the outgrow stream's frame kGrownFrame growing the grid before
// it is recorded: the frame before's batch has finished first.
TEST_F(StreamPlayerTest, AfterMeshMayReadTheGridAFrameGrowsAtItsRecord) {
  ASSERT_NO_FATAL_FAILURE(use_stream(vp_test::kOutgrowStream));
  step_through_compacting(/*grown_at_record=*/true);
}

// Played, the outgrow stream's frame kGrownFrame is recorded as soon as the
// frame before is published, whose after_mesh batch may still run: the worker
// waits for it before the grow, which waits for the queue anyway. Every frame
// comes, each batch counting its frame's blocks; the frame's device counts
// are the test's properties.
TEST_F(StreamPlayerTest,
       PlayedAFrameThatOutgrowsTheGridWaitsForAfterMeshFirst) {
  ASSERT_NO_FATAL_FAILURE(use_stream(vp_test::kOutgrowStream));
  std::optional<FrameStamp> grown;
  ASSERT_NO_FATAL_FAILURE(
      play_through_compacting(/*grown_at_record=*/true, &grown));
  ASSERT_TRUE(grown.has_value());
  RecordProperty("grown_frame_submits",
                 std::to_string(grown->counters.submits));
  RecordProperty("grown_frame_host_waits",
                 std::to_string(grown->counters.host_waits));
  RecordProperty("grown_frame_blocked_us",
                 std::to_string(grown->counters.blocked_ns / 1000));
}

// Played, the grow stream's frame kGrownFrame grows the grid at its finish,
// recorded as soon as the frame before was published: that frame's batch
// has finished first.
TEST_F(StreamPlayerTest, AfterMeshMayReadTheGridPlayedAFrameGrowsAtItsFinish) {
  ASSERT_NO_FATAL_FAILURE(use_stream(vp_test::kGrowStream));
  std::optional<FrameStamp> grown;
  ASSERT_NO_FATAL_FAILURE(
      play_through_compacting(/*grown_at_record=*/false, &grown));
  EXPECT_TRUE(grown.has_value());
}

// The batch in flight when the grow stream's frame kGrownFrame grows the
// grid at its finish is a rebuild's, of the frame before: it has finished
// first.
TEST_F(StreamPlayerTest, AfterMeshMayReadTheGridARebuildThenAGrowAtItsFinish) {
  ASSERT_NO_FATAL_FAILURE(use_stream(vp_test::kGrowStream));
  rebuild_then_grow_compacting(/*grown_at_record=*/false);
}

// As above, the outgrow stream's frame kGrownFrame growing the grid before
// it is recorded.
TEST_F(StreamPlayerTest, AfterMeshMayReadTheGridARebuildThenAGrowAtItsRecord) {
  ASSERT_NO_FATAL_FAILURE(use_stream(vp_test::kOutgrowStream));
  rebuild_then_grow_compacting(/*grown_at_record=*/true);
}

// The mesh published is ready once after_mesh's batch is: a consumer batch
// that waits for its ready value reads what that batch wrote, each frame its
// own value.
TEST_F(StreamPlayerTest, AMeshIsPublishedReadyOnceAfterMeshsBatchIs) {
  core::BufferDesc desc;
  desc.size = sizeof(std::uint32_t);
  desc.usage =
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  desc.memory = core::MemoryUsage::DeviceOnly;
  core::Result<core::Buffer> made = allocator().create_buffer(desc);
  ASSERT_TRUE(made.ok()) << made.status().message();
  const core::Buffer written = std::move(made).value();
  std::atomic<const core::TimelineSemaphore*> meshed{nullptr};  // its ready's
  hooks_.emplace();
  hooks_->after_mesh = [&written, &meshed](core::CommandBatch& batch,
                                           const grid::volume::VoxelBlockGrid&,
                                           const grid::mesh::DeviceMesh& mesh,
                                           FrameStamp& stamp) {
    meshed = mesh.ready.semaphore;
    return batch.fill(written, 0, sizeof(std::uint32_t),
                      static_cast<std::uint32_t>(100 + stamp.index));
  };
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  std::uint64_t last = 0;  // the ready value published last
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    player->step();
    // Taken as it comes, its ready value reached or not.
    std::optional<FrameStamp> frame;
    ASSERT_TRUE(eventually([&] {
      exchange_.begin_frame(
          slot_, [&](const grid::mesh::DeviceMesh&, FrameStamp& stamp) {
            frame = std::move(stamp);
            player->taken();
            return true;
          });
      slot_ = (slot_ + 1) % kFramesInFlight;
      return frame.has_value();
    })) << "no frame "
        << i;
    // Not the extract's point, which after_mesh saw: one value on a
    // timeline of the hooks' batches each.
    const core::TimelinePoint ready = exchange_.live().ready;
    ASSERT_NE(ready.semaphore, nullptr);
    EXPECT_NE(ready.semaphore, meshed.load()) << "frame " << i;
    EXPECT_EQ(ready.value, last + 1) << "frame " << i;
    last = ready.value;
    std::uint32_t value = 0;
    core::CommandBatch read(device(), allocator());
    ASSERT_TRUE(read.readback(written, 0, sizeof value, &value).ok());
    core::Result<core::PendingBatch> pending =
        read.submit_async({exchange_.live().ready}, {});
    ASSERT_TRUE(pending.ok()) << pending.status().message();
    const core::Status waited = pending->wait();
    ASSERT_TRUE(waited.ok()) << waited.message();
    EXPECT_EQ(value, 100 + frame->index) << "frame " << i;
  }
  EXPECT_LT(stop_time(*player), kPrompt);  // before `written` goes
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// A take holds a mesh whose ready value the device has not reached, without
// asking the commit hook, and commits it at the first take after: a mesh
// published behind a value the host sets, to a player opened but not
// started, whose taken() only wakes its worker.
TEST_F(StreamPlayerTest, ATakeHoldsAMeshUntilItsReadyValueIsReached) {
  core::Result<std::unique_ptr<StreamPlayer>> opened =
      StreamPlayer::open(path_);
  ASSERT_TRUE(opened.ok()) << opened.status().message();
  player_ = std::move(opened).value();
  core::Result<core::TimelineSemaphore> made =
      core::TimelineSemaphore::create(device());
  ASSERT_TRUE(made.ok()) << made.status().message();
  core::TimelineSemaphore ready = std::move(made).value();
  int asked = 0;
  FrameHooks hooks;
  hooks.commit = [&asked](const grid::mesh::DeviceMesh&, FrameStamp&) {
    ++asked;
    return core::Result<FrameCommit>(FrameCommit::kDraw);
  };
  FrameTaker taker(*player_, exchange_, kFramesInFlight, &hooks);
  // An empty mesh, which needs no buffer to be handed to the commit.
  grid::mesh::DeviceMesh mesh;
  mesh.generation = 1;
  mesh.ready = {&ready, 1};
  FrameStamp stamp;
  stamp.index = 7;
  exchange_.publish(mesh, stamp);
  for (std::uint32_t slot = 0; slot < 4; ++slot) {
    const core::Result<std::optional<FrameStamp>> held =
        taker.take(slot % kFramesInFlight);
    ASSERT_TRUE(held.ok()) << held.status().message();
    EXPECT_FALSE(held->has_value()) << "committed unreached";
  }
  EXPECT_EQ(asked, 0);
  EXPECT_EQ(taker.frames_held(), 1u);
  EXPECT_EQ(exchange_.live().generation, 0u);
  ASSERT_TRUE(ready.signal(1).ok());
  const core::Result<std::optional<FrameStamp>> taken = taker.take(0);
  ASSERT_TRUE(taken.ok()) << taken.status().message();
  ASSERT_TRUE(taken->has_value());
  EXPECT_EQ((*taken)->index, 7u);
  EXPECT_TRUE((*taken)->taken.has_value());
  EXPECT_EQ(asked, 1);
  EXPECT_EQ(exchange_.live().generation, 1u);
  EXPECT_EQ(taker.frames_held(), 1u);
}

// Played, a steady frame with after_mesh is two submits, its batch and
// after_mesh's, with no staging buffer and at most one wait that blocked,
// as with none (OneBatchPerFrame): after_mesh's batch is waited for only at
// the next frame's finish, once that frame's batch, queued after it, has
// resolved, when the GPU has run it, so most frames have none, as a frame's
// own fence has normally signalled by its due time; a wait right after its
// submit, or before the next frame's record, would block each frame. Taken
// with no device work of the test's.
TEST_F(StreamPlayerTest, AfterMeshAddsOneSubmitAndNoBlockingWait) {
  core::BufferDesc desc;
  desc.size = sizeof(std::uint32_t);
  desc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  desc.memory = core::MemoryUsage::DeviceOnly;
  core::Result<core::Buffer> made = allocator().create_buffer(desc);
  ASSERT_TRUE(made.ok()) << made.status().message();
  const core::Buffer written = std::move(made).value();
  hooks_.emplace();
  hooks_->after_mesh =
      [&written](core::CommandBatch& batch, const grid::volume::VoxelBlockGrid&,
                 const grid::mesh::DeviceMesh&, FrameStamp& stamp) {
        return batch.fill(written, 0, sizeof(std::uint32_t),
                          static_cast<std::uint32_t>(stamp.index));
      };
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  std::uint64_t seen = 0;
  std::uint64_t blocked = 0;  // frames with a wait that blocked
  while (seen < kFrames) {
    const std::optional<FrameStamp> frame = take_with(taker);
    ASSERT_TRUE(frame.has_value()) << "no frame after " << seen;
    if (frame->pass < 2) {
      continue;
    }
    ++seen;
    SCOPED_TRACE(::testing::Message() << "frame " << frame->index);
    EXPECT_EQ(frame->counters.submits, 2u);
    EXPECT_EQ(frame->counters.staging_buffers, 0u);
    EXPECT_LE(frame->counters.host_waits, 1u);
    blocked += frame->counters.host_waits != 0 ? 1 : 0;
  }
  EXPECT_LE(blocked, seen / 2) << "after_mesh's batch waited for at once";
  EXPECT_LT(stop_time(*player), kPrompt);  // before `written` goes
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// A readback after_mesh records is written by the next after_mesh: the
// worker finished its batch first.
TEST_F(StreamPlayerTest, AfterMeshsReadbackIsWrittenBeforeTheNextAfterMesh) {
  struct Read {
    VkDrawIndexedIndirectCommand command{};
    std::uint32_t triangles = 0;
  };
  std::deque<Read> reads;  // the worker's, until it stops
  int unwritten = 0;
  hooks_.emplace();
  hooks_->after_mesh = [&](core::CommandBatch& batch,
                           const grid::volume::VoxelBlockGrid&,
                           const grid::mesh::DeviceMesh& mesh, FrameStamp&) {
    if (!reads.empty() &&
        reads.back().command.indexCount !=
            grid::mesh::kIndicesPerTriangle * reads.back().triangles) {
      ++unwritten;
    }
    reads.emplace_back();
    reads.back().triangles = mesh.triangle_count;
    return batch.readback(
        borrowed(mesh.indirect, sizeof(VkDrawIndexedIndirectCommand),
                 mesh.indirect_usage, mesh.sharing_mode),
        0, sizeof(VkDrawIndexedIndirectCommand), &reads.back().command);
  };
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    ASSERT_TRUE(take_with(taker).has_value()) << "no frame " << i;
  }
  stop_time(*player);  // its batch finished before the stop returns
  ASSERT_GE(reads.size(), kFrames);
  EXPECT_EQ(unwritten, 0);
  EXPECT_EQ(reads.back().command.indexCount,
            grid::mesh::kIndicesPerTriangle * reads.back().triangles);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// Played, then parked: the park finishes after_mesh's batch, its readback
// written, then runs on_park, once until an unpark, and returns; nothing else
// runs while parked; the unpark runs on_resume, then the worker goes on.
TEST_F(StreamPlayerTest, AParkFinishesAfterMeshsBatchThenOnParkAndOnResume) {
  std::mutex mutex;
  std::deque<VkDrawIndexedIndirectCommand> commands;  // under mutex
  std::deque<std::uint32_t> triangles;                // under mutex
  int unfinished_at_park = 0;  // on_park's, on the park's thread
  logging_hooks();
  const auto log = hooks_->after_mesh;
  const auto parked = hooks_->on_park;
  hooks_->on_park = [&, parked] {
    parked();
    std::lock_guard<std::mutex> lock(mutex);
    if (commands.empty() ||
        commands.back().indexCount !=
            grid::mesh::kIndicesPerTriangle * triangles.back()) {
      ++unfinished_at_park;
    }
  };
  hooks_->after_mesh = [&, log](core::CommandBatch& batch,
                                const grid::volume::VoxelBlockGrid& grid,
                                const grid::mesh::DeviceMesh& mesh,
                                FrameStamp& stamp) {
    VKC_TRY(log(batch, grid, mesh, stamp));
    std::lock_guard<std::mutex> lock(mutex);
    commands.emplace_back();
    triangles.push_back(mesh.triangle_count);
    return batch.readback(
        borrowed(mesh.indirect, sizeof(VkDrawIndexedIndirectCommand),
                 mesh.indirect_usage, mesh.sharing_mode),
        0, sizeof(VkDrawIndexedIndirectCommand), &commands.back());
  };
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  for (int i = 0; i < 2; ++i) {
    ASSERT_TRUE(take_with(taker).has_value());
  }
  ASSERT_TRUE(player->park(kArrival));
  {
    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_FALSE(commands.empty());
    EXPECT_EQ(commands.back().indexCount,
              grid::mesh::kIndicesPerTriangle * triangles.back())
        << "after_mesh's batch unfinished at the park's return";
    EXPECT_EQ(unfinished_at_park, 0) << "on_park before the batch finished";
  }
  EXPECT_EQ(log_.parks, 1);
  EXPECT_EQ(log_.resumes, 0);
  const std::size_t meshed = log_.meshed().size();
  std::this_thread::sleep_for(kQuiet);
  EXPECT_EQ(log_.meshed().size(), meshed) << "after_mesh ran parked";
  ASSERT_TRUE(player->park(kArrival));
  EXPECT_EQ(log_.parks, 1) << "on_park again before an unpark";
  player->unpark();
  EXPECT_EQ(log_.resumes, 1);
  EXPECT_TRUE(eventually([&] {
    (void)take_with(taker, 1ms);
    return log_.meshed().size() > meshed;
  })) << "no after_mesh after the unpark";
  player->unpark();  // not parked: no on_resume
  EXPECT_EQ(log_.resumes, 1);
  EXPECT_LT(stop_time(*player), kPrompt);  // before `commands` goes
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// Paused, frame 1 stepped to with an after_mesh batch of copies that run
// about kBusy, and published: a park then finds the worker idle and waits
// for that batch no longer than its timeout. It returns false with the batch
// still running, its readback unwritten and on_park not run, and the hold
// stays: a step goes only at the unpark. A later park finishes the batch,
// then runs on_park.
TEST_F(StreamPlayerTest, AParkWaitsForAfterMeshsBatchNoLongerThanItsTimeout) {
  constexpr VkDeviceSize kCopyBytes = VkDeviceSize{64} << 20;
  constexpr std::uint32_t kMaxCopies = 16384;
  constexpr auto kBusy = 400ms;
  constexpr auto kWait = 20ms;  // the park's timeout
  constexpr std::uint32_t kValue = 7;
  const auto make = [this](VkDeviceSize size) {
    core::BufferDesc desc;
    desc.size = size;
    desc.usage =
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    desc.memory = core::MemoryUsage::DeviceOnly;
    return allocator().create_buffer(desc);
  };
  core::Result<core::Buffer> from = make(kCopyBytes);
  core::Result<core::Buffer> to = make(kCopyBytes);
  core::Result<core::Buffer> mark = make(sizeof(std::uint32_t));
  ASSERT_TRUE(from.ok()) << from.status().message();
  ASSERT_TRUE(to.ok()) << to.status().message();
  ASSERT_TRUE(mark.ok()) << mark.status().message();
  const auto record_copies = [&](core::CommandBatch& batch,
                                 std::uint32_t count) -> core::Status {
    for (std::uint32_t i = 0; i < count; ++i) {
      VKC_TRY(batch.copy(*from, 0, *to, 0, kCopyBytes));
    }
    return core::Status();
  };
  // As many copies as run about kBusy on this device: timed from 8, four
  // times more each round, until a batch of them takes 50 ms.
  std::uint32_t copies = 8;
  for (;;) {
    core::CommandBatch batch(device(), allocator());
    core::Status ran = record_copies(batch, copies);
    const SteadyClock::time_point began = SteadyClock::now();
    if (ran.ok()) {
      ran = batch.submit();
    }
    ASSERT_TRUE(ran.ok()) << ran.message();
    const SteadyClock::duration took = SteadyClock::now() - began;
    if (took >= 50ms || copies >= kMaxCopies) {
      const double scale = std::chrono::duration<double>(kBusy) / took;
      copies = static_cast<std::uint32_t>(
          std::clamp(copies * scale, 1.0, static_cast<double>(kMaxCopies)));
      break;
    }
    copies *= 4;
  }
  logging_hooks();
  std::atomic<bool> heavy{false};  // the next after_mesh records the copies
  std::uint32_t marked = 0;        // their batch's readback
  const auto log = hooks_->after_mesh;
  hooks_->after_mesh = [&, log](core::CommandBatch& batch,
                                const grid::volume::VoxelBlockGrid& grid,
                                const grid::mesh::DeviceMesh& mesh,
                                FrameStamp& stamp) -> core::Status {
    VKC_TRY(log(batch, grid, mesh, stamp));
    if (!heavy.exchange(false)) {
      return core::Status();
    }
    VKC_TRY(record_copies(batch, copies));
    VKC_TRY(batch.fill(*mark, 0, sizeof(std::uint32_t), kValue));
    return batch.readback(*mark, 0, sizeof(std::uint32_t), &marked);
  };
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  player->step();
  ASSERT_TRUE(take_with(taker).has_value());  // frame 0
  // From here on no early return, so the stop comes before what the copies'
  // batch uses goes.
  heavy = true;
  player->step();
  // Frame 1 published, so its step has ended: kept back untaken.
  bool running = false;  // its batch, when its publish was seen
  EXPECT_TRUE(eventually([&] {
    const grid::mesh::ExchangeOutcome outcome = exchange_.begin_frame(
        slot_, [&](const grid::mesh::DeviceMesh& mesh, FrameStamp&) {
          if (mesh.ready.semaphore != nullptr) {
            const core::Result<std::uint64_t> reached =
                mesh.ready.semaphore->value();
            running = reached.ok() && *reached < mesh.ready.value;
          }
          return grid::mesh::CommitDecision::kPark;
        });
    slot_ = (slot_ + 1) % kFramesInFlight;
    return outcome == grid::mesh::ExchangeOutcome::kParked;
  })) << "frame 1 not published";
  EXPECT_TRUE(running) << "the copies finished before the publish was seen";
  const std::size_t meshed = log_.meshed().size();
  EXPECT_FALSE(player->park(kWait)) << "the park waited out the copies";
  EXPECT_EQ(log_.parks, 0) << "on_park with after_mesh's batch running";
  EXPECT_EQ(marked, 0u) << "the readback written";  // nothing waits meanwhile
  player->step();
  std::this_thread::sleep_for(kQuiet);
  EXPECT_EQ(log_.meshed().size(), meshed) << "a step after a timed-out park";
  EXPECT_TRUE(player->park(kArrival));
  EXPECT_EQ(marked, kValue)
      << "after_mesh's batch unfinished at the park's end";
  EXPECT_EQ(log_.parks, 1);
  player->unpark();
  EXPECT_EQ(log_.resumes, 1);
  for (std::uint64_t i = 1; i <= 2; ++i) {
    const std::optional<FrameStamp> frame = take_with(taker);
    if (!frame) {
      ADD_FAILURE() << "no frame " << i << " after the unpark";
      break;
    }
    EXPECT_EQ(frame->index, i);
  }
  EXPECT_LT(stop_time(*player), kPrompt);  // before the buffers and `marked` go
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// after_mesh's failure stops playback with its status: no frame after it is
// published.
TEST_F(StreamPlayerTest, AnAfterMeshFailureStopsPlaybackWithItsStatus) {
  std::atomic<int> calls{0};
  hooks_.emplace();
  hooks_->after_mesh = [&calls](core::CommandBatch&,
                                const grid::volume::VoxelBlockGrid&,
                                const grid::mesh::DeviceMesh&, FrameStamp&) {
    if (++calls == 3) {
      return core::Status::unsupported("the test's after_mesh refuses");
    }
    return core::Status();
  };
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  std::vector<std::uint64_t> shown;
  ASSERT_TRUE(eventually([&] {
    const std::optional<FrameStamp> frame = take_with(taker, 1ms);
    if (frame) {
      shown.push_back(frame->index);
    }
    return !player->status().ok();
  }));
  const core::Status status = player->status();
  EXPECT_EQ(status.domain(), Code::Unsupported) << status.message();
  EXPECT_NE(status.message().find("after_mesh refuses"), std::string::npos)
      << status.message();
  EXPECT_FALSE(take_with(taker, kQuiet).has_value());
  EXPECT_LT(stop_time(*player), kPrompt);  // before `calls` goes
  EXPECT_EQ(calls, 3);
  EXPECT_LE(shown.size(), 2u);
}

// A stop while the worker waits to publish a frame that nothing takes
// finishes that frame's after_mesh batch before its stamp, and so its extra,
// goes: the extra's release finds the batch's readback written.
TEST_F(StreamPlayerTest, AnUnpublishedFramesExtraGoesOnceItsBatchHasFinished) {
  constexpr std::uint32_t kValue = 7;
  core::BufferDesc desc;
  desc.size = sizeof(std::uint32_t);
  desc.usage =
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  desc.memory = core::MemoryUsage::DeviceOnly;
  core::Result<core::Buffer> made = allocator().create_buffer(desc);
  ASSERT_TRUE(made.ok()) << made.status().message();
  const core::Buffer written = std::move(made).value();
  // Each after_mesh's readback, kept here so a late write lands in memory
  // still there, and its extra's release: shared with the extras' deleters,
  // which may run after the test's body.
  struct Reads {
    std::mutex mutex;
    std::deque<std::uint32_t> values;  // the readbacks' destinations
    int released = 0;
    int unwritten = 0;  // released before its batch finished
  };
  const auto reads = std::make_shared<Reads>();
  hooks_.emplace();
  hooks_->after_mesh = [&written, reads](core::CommandBatch& batch,
                                         const grid::volume::VoxelBlockGrid&,
                                         const grid::mesh::DeviceMesh&,
                                         FrameStamp& stamp) -> core::Status {
    std::uint32_t* value = nullptr;
    {
      std::lock_guard<std::mutex> lock(reads->mutex);
      value = &reads->values.emplace_back(0u);
    }
    VKC_TRY(batch.fill(written, 0, sizeof(std::uint32_t), kValue));
    VKC_TRY(batch.readback(written, 0, sizeof(std::uint32_t), value));
    stamp.extra = std::shared_ptr<const void>(value, [reads](const void* read) {
      std::lock_guard<std::mutex> lock(reads->mutex);
      ++reads->released;
      if (*static_cast<const std::uint32_t*>(read) != kValue) {
        ++reads->unwritten;
      }
    });
    return core::Status();
  };
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  // Nothing is taken: frame 0 is published, and frame 1, meshed, waits.
  ASSERT_TRUE(eventually([&] {
    std::lock_guard<std::mutex> lock(reads->mutex);
    return reads->values.size() >= 2;
  }));
  std::this_thread::sleep_for(kQuiet);     // in the wait of frame 1's publish
  EXPECT_LT(stop_time(*player), kPrompt);  // before `written` goes
  {
    std::lock_guard<std::mutex> lock(reads->mutex);
    EXPECT_EQ(reads->values.size(), 2u);
    EXPECT_EQ(reads->released, 1) << "frame 1's, never published";
    EXPECT_EQ(reads->unwritten, 0) << "an extra went before its batch";
  }
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// A commit hook's failure is the take's, and the frame is held, neither drawn
// nor skipped: asked again at the next take, which may draw it.
TEST_F(StreamPlayerTest, ACommitFailureIsTheTakesAndHoldsTheFrame) {
  int asked = 0;  // for frame 1, on the take's thread
  hooks_.emplace();
  hooks_->commit = [&asked](const grid::mesh::DeviceMesh&,
                            FrameStamp& stamp) -> core::Result<FrameCommit> {
    if (stamp.index == 1 && ++asked <= 2) {
      return core::Status::unsupported("the test's commit refuses");
    }
    return core::Result<FrameCommit>(FrameCommit::kDraw);
  };
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  player->step();
  ASSERT_TRUE(take_with(taker).has_value());  // frame 0
  const std::uint64_t before = exchange_.live().generation;
  player->step();
  core::Status refused;
  ASSERT_TRUE(eventually([&] {
    core::Result<std::optional<FrameStamp>> taken = taker.take(slot_);
    slot_ = (slot_ + 1) % kFramesInFlight;
    if (!taken.ok()) {
      refused = taken.status();
      return true;
    }
    EXPECT_FALSE(taken->has_value()) << "frame 1 drawn";
    return false;
  }));
  EXPECT_EQ(refused.domain(), Code::Unsupported) << refused.message();
  EXPECT_NE(refused.message().find("commit refuses"), std::string::npos)
      << refused.message();
  EXPECT_EQ(exchange_.live().generation, before) << "the frame before stays";
  const core::Result<std::optional<FrameStamp>> again = taker.take(slot_);
  slot_ = (slot_ + 1) % kFramesInFlight;
  EXPECT_FALSE(again.ok()) << "not asked again";
  const std::optional<FrameStamp> frame = take_with(taker);
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->index, 1u);
  EXPECT_EQ(asked, 3);
  EXPECT_EQ(taker.frames_skipped(), 0u);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// vp_probe --worker's consumer (FrameTaker) under a commit hook that holds
// frame 2: frame 1 stays live, the hook is asked again each take, and the
// worker publishes frame 3 and meshes frame 4, two ahead, then waits for
// the take; released, frames 2 to 5 follow in order. Frame 2 counts as held
// once, as may a frame whose ready value the take found unreached.
TEST_F(StreamPlayerTest, AHeldFrameKeepsTheOneBeforeAndTheFileTwoAheadAtMost) {
  logging_hooks();
  std::atomic<bool> release{false};
  int holds = 0;  // the hook's kHold answers: the take's thread's
  hooks_->commit = [&](const grid::mesh::DeviceMesh&, FrameStamp& stamp) {
    if (stamp.index == 2 && !release) {
      ++holds;
      return core::Result<FrameCommit>(FrameCommit::kHold);
    }
    return core::Result<FrameCommit>(FrameCommit::kDraw);
  };
  StreamPlayer* player = start(/*playing=*/true, kFramesInFlight + 3);
  ASSERT_NE(player, nullptr);
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  for (std::uint64_t i = 0; i < 2; ++i) {
    const std::optional<FrameStamp> frame = take_with(taker);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->index, i);
  }
  const std::uint64_t held = exchange_.live().generation;
  const std::uint64_t held_before = taker.frames_held();
  // Frame 3 is due half a second on (kGapNs), frame 4 a period after.
  ASSERT_TRUE(eventually([&] {
    EXPECT_FALSE(take_with(taker, 1ms).has_value()) << "a frame while held";
    return log_.newest().value_or(0) >= 4;
  }));
  EXPECT_FALSE(take_with(taker, kQuiet).has_value());
  EXPECT_EQ(log_.newest(), 4u) << "more than two frames ahead";
  EXPECT_EQ(exchange_.live().generation, held) << "the frame before stays";
  EXPECT_GE(holds, 5);
  EXPECT_EQ(taker.frames_held(), held_before + 1);
  release = true;
  for (std::uint64_t i = 2; i < kFrames; ++i) {
    const std::optional<FrameStamp> frame = take_with(taker);
    ASSERT_TRUE(frame.has_value()) << "no frame " << i;
    EXPECT_EQ(frame->index, i);
  }
  EXPECT_EQ(taker.frames_skipped(), 0u);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// A commit hook that skips every odd frame, over three passes: no odd frame
// is ever drawn, the frame before stays live meanwhile, the next one
// follows, and the ring one mesh deeper (mesh_wiring's holds) never runs
// out.
TEST_F(StreamPlayerTest, ASkippedFrameIsNeverDrawnAndTheRingNeverRunsOut) {
  constexpr std::uint64_t kPasses = 3;
  hooks_.emplace();
  hooks_->commit = [](const grid::mesh::DeviceMesh&, FrameStamp& stamp) {
    return core::Result<FrameCommit>(stamp.index % 2 == 1 ? FrameCommit::kSkip
                                                          : FrameCommit::kDraw);
  };
  StreamPlayer* player = start(/*playing=*/true, kFramesInFlight + 3);
  ASSERT_NE(player, nullptr);
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  std::vector<std::uint64_t> drawn;
  for (std::uint64_t i = 0; i < kPasses * kFrames / 2; ++i) {
    const std::optional<FrameStamp> frame = take_with(taker);
    ASSERT_TRUE(frame.has_value()) << "no frame after " << drawn.size() << "; "
                                   << player->status().message();
    drawn.push_back(frame->index);
  }
  std::vector<std::uint64_t> expected;
  for (std::uint64_t pass = 0; pass < kPasses; ++pass) {
    for (std::uint64_t i = 0; i < kFrames; i += 2) {
      expected.push_back(i);
    }
  }
  EXPECT_EQ(drawn, expected);
  // The last pass's last odd frame may not be taken yet.
  EXPECT_GE(taker.frames_skipped(), kPasses * kFrames / 2 - 1);
  EXPECT_LE(taker.frames_skipped(), kPasses * kFrames / 2);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// One frame is skipped at most between two drawn: frame 2 skipped, the hook's
// skip of frame 3 holds it instead, asked again each take, until the hook
// draws it.
TEST_F(StreamPlayerTest, ASecondSkipBeforeADrawHoldsTheFrame) {
  std::atomic<int> asked{0};  // for frame 3
  hooks_.emplace();
  hooks_->commit = [&asked](const grid::mesh::DeviceMesh&, FrameStamp& stamp) {
    if (stamp.index == 2 || (stamp.index == 3 && ++asked < 4)) {
      return core::Result<FrameCommit>(FrameCommit::kSkip);
    }
    return core::Result<FrameCommit>(FrameCommit::kDraw);
  };
  StreamPlayer* player = start(/*playing=*/false, kFramesInFlight + 3);
  ASSERT_NE(player, nullptr);
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  for (std::uint64_t i = 0; i < 2; ++i) {
    player->step();
    const std::optional<FrameStamp> frame = take_with(taker);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->index, i);
  }
  player->step();  // frame 2, skipped
  ASSERT_TRUE(eventually([&] {
    EXPECT_FALSE(take_with(taker, 1ms).has_value()) << "frame 2 drawn";
    return taker.frames_skipped() == 1;
  }));
  player->step();  // frame 3, its skip held
  const std::optional<FrameStamp> frame = take_with(taker);
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->index, 3u);
  EXPECT_EQ(asked, 4);
  EXPECT_EQ(taker.frames_skipped(), 1u);
  EXPECT_EQ(taker.frames_held(), 1u);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// Each slot keeps the extra of the frame it drew until its next take: frame
// 0's goes only at the take of the slot that drew it last, after frame 1 is
// committed on the other; frame 1's while it is live, never.
TEST_F(StreamPlayerTest, ATakerKeepsAnExtraUntilTheSlotsThatDrewItTakeAgain) {
  const auto extras = std::make_shared<Extras>();
  hooks_.emplace();
  hooks_->after_mesh =
      [extras](core::CommandBatch&, const grid::volume::VoxelBlockGrid&,
               const grid::mesh::DeviceMesh&, FrameStamp& stamp) {
        stamp.extra = Extras::make(extras, stamp.index);
        return core::Status();
      };
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  player->step();
  ASSERT_TRUE(take_with(taker).has_value());  // frame 0, on one slot
  for (int i = 0; i < 3; ++i) {               // and on both
    ASSERT_TRUE(taker.take(slot_).ok());
    slot_ = (slot_ + 1) % kFramesInFlight;
  }
  player->step();
  ASSERT_TRUE(take_with(taker).has_value());  // frame 1, on slot_ - 1
  EXPECT_FALSE(extras->was_released(0)) << "the other slot drew frame 0 last";
  ASSERT_TRUE(taker.take(slot_).ok());
  EXPECT_TRUE(extras->was_released(0));
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(taker.take(slot_).ok());
    slot_ = (slot_ + 1) % kFramesInFlight;
  }
  EXPECT_FALSE(extras->was_released(1)) << "frame 1 is live";
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// --- Live: the tests' stream served on 127.0.0.1 ----------------------------
// Never 0.0.0.0, which could raise a firewall prompt.

// The longest a park's connection stays open once the park has returned.
constexpr auto kClosed = 50ms;
// Between two records of a steady flow.
constexpr auto kFlow = 20ms;
// The most StreamClient takes in one receive (grid's StreamClient::next).
constexpr std::size_t kClientReceive = std::size_t{2} << 20;

#ifdef __APPLE__
constexpr int kSendFlags = 0;  // SO_NOSIGPIPE is set on the socket
#else
constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

// The tests' stream, read back: its session, and each record's info and
// payload.
// A file never asks a live hook, and its frames' release is their due, with
// no playout.
TEST_F(StreamPlayerTest, AFileAsksNoLiveHookAndIsReleasedWhenDue) {
  const auto asks = std::make_shared<LiveAsks>();
  live_hooks(asks, [](const FrameStamp&) { return FrameCommit::kSkip; });
  StreamPlayer* player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  EXPECT_EQ(player->live_clock(), nullptr);
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    const std::optional<FrameStamp> frame = take_with(taker);
    ASSERT_TRUE(frame.has_value()) << "frame " << i;
    EXPECT_EQ(frame->index, i);
    EXPECT_EQ(frame->released, frame->due);
    EXPECT_FALSE(frame->playout.has_value());
    EXPECT_EQ(frame->records, 1u);
  }
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_EQ(asks->needs, 0);
  EXPECT_TRUE(asks->all().empty());
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// Live, the tests' records are a second of pts apart: at the tests' paces a
// record never comes late on its pts, so no test's own pace raises the
// playout delay (live_clock.hpp); the tests of the delay state theirs.
constexpr std::uint64_t kLiveStepNs = 1'000'000'000;

// The stream as a sender sends it, its pts kLiveStepNs apart.
struct Recording {
  stream::Session session;
  std::vector<stream::RecordInfo> infos;
  std::vector<Bytes> payloads;
};

// `info` in pass `pass` of a loop of the stream: its seq and its pts past
// every record of the passes before, so seq rises and a key's pts never
// falls.
stream::RecordInfo restamped(const stream::RecordInfo& info,
                             std::uint64_t pass) {
  stream::RecordInfo next = info;
  next.seq += pass * kFrames;
  next.pts_ns += pass * kFrames * kLiveStepNs;
  return next;
}

// A blocking listener on 127.0.0.1 at a free port, which goes into `*port`.
core::UniqueFd listen_on_loopback(std::uint16_t* port) {
  core::UniqueFd fd(::socket(AF_INET, SOCK_STREAM, 0));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t size = sizeof address;
  if (!fd.valid() ||
      ::bind(fd.get(), reinterpret_cast<const sockaddr*>(&address),
             sizeof address) != 0 ||
      ::listen(fd.get(), 8) != 0 ||
      ::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&address), &size) !=
          0) {
    ADD_FAILURE() << "no listener on 127.0.0.1: " << std::strerror(errno);
    return {};
  }
  *port = ntohs(address.sin_port);
  return fd;
}

// A port on 127.0.0.1 that a listener had a moment ago, now closed, so a
// connect to it is refused: macOS drops a SYN to a socket bound but not
// listening.
std::uint16_t refusing_port() {
  std::uint16_t port = 0;
  listen_on_loopback(&port);
  return port;
}

// A sender of raw bytes on 127.0.0.1, for what a StreamServer cannot show (a
// connection's end, bytes that are no stream). It takes one connection at a
// time, sends it `feed`'s pieces from 0, one each `period`, an empty one
// sending nothing, until the other end closes it; then it notes when, and
// takes the next.
class RawPeer {
 public:
  using Feed = std::function<Bytes(std::uint64_t piece)>;

  RawPeer(Feed feed, std::chrono::milliseconds period, int send_buffer = 0)
      : feed_(std::move(feed)),
        period_(period),
        send_buffer_(send_buffer),
        listener_(listen_on_loopback(&port_)),
        thread_([this] { run(); }) {}
  ~RawPeer() {
    stop_ = true;
    thread_.join();
  }
  RawPeer(const RawPeer&) = delete;
  RawPeer& operator=(const RawPeer&) = delete;

  std::uint16_t port() const { return port_; }

  // The connections taken so far.
  std::size_t accepted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return accepted_;
  }

  // Whether connection `n`, from 1, has closed within `wait`, and when.
  std::optional<SteadyClock::time_point> closed(
      std::size_t n, SteadyClock::duration wait = kArrival) const {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!closed_cv_.wait_for(lock, wait, [&] { return closed_.size() >= n; })) {
      return std::nullopt;
    }
    return closed_[n - 1];
  }

 private:
  void run() {
    while (!stop_) {
      pollfd ready{listener_.get(), POLLIN, 0};
      if (::poll(&ready, 1, 10) != 1) {
        continue;
      }
      core::UniqueFd peer(::accept(listener_.get(), nullptr, nullptr));
      if (!peer.valid() || !prepare(peer.get())) {
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++accepted_;
      }
      serve(peer.get());
      peer.reset();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_.push_back(SteadyClock::now());
      }
      closed_cv_.notify_all();
    }
  }

  // Non-blocking, optionally bounded buffering, and on Apple no SIGPIPE.
  bool prepare(int fd) const {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
      return false;
    }
    if (send_buffer_ != 0 &&
        ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer_,
                     sizeof send_buffer_) != 0) {
      return false;
    }
#ifdef __APPLE__
    const int on = 1;
    return ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on) == 0;
#else
    return true;
#endif
  }

  // Feed `fd` until its other end closes it, or stop.
  void serve(int fd) {
    SteadyClock::time_point due = SteadyClock::now();
    for (std::uint64_t piece = 0; !stop_;) {
      if (SteadyClock::now() >= due) {
        if (!send_all(fd, feed_(piece++))) {
          return;
        }
        due += period_;
      }
      if (ended(fd, due)) {
        return;
      }
    }
  }

  // Whether the other end has closed `fd`, waiting for it until `until`, at
  // most a few ms. It sends nothing, so a read finds only the end.
  static bool ended(int fd, SteadyClock::time_point until) {
    const auto wait =
        std::clamp(std::chrono::duration_cast<std::chrono::milliseconds>(
                       until - SteadyClock::now()),
                   0ms, 5ms);
    pollfd ready{fd, POLLIN, 0};
    if (::poll(&ready, 1, static_cast<int>(wait.count())) != 1) {
      return false;
    }
    char bytes[256];
    const ssize_t got = ::recv(fd, bytes, sizeof bytes, 0);
    return got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                        errno != EINTR);
  }

  // Send `bytes` whole, resuming partial sends while the socket is full:
  // false once the other end has closed it, or on stop.
  bool send_all(int fd, const Bytes& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
      if (stop_) {
        return false;
      }
      const ssize_t n =
          ::send(fd, bytes.data() + sent, bytes.size() - sent, kSendFlags);
      if (n > 0) {
        sent += static_cast<std::size_t>(n);
      } else if (n < 0 &&
                 (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        pollfd ready{fd, POLLOUT, 0};
        ::poll(&ready, 1, 5);
      } else {
        return false;
      }
    }
    return true;
  }

  Feed feed_;  // the peer's thread's alone
  const std::chrono::milliseconds period_;
  const int send_buffer_;
  std::uint16_t port_ = 0;
  core::UniqueFd listener_;
  std::atomic<bool> stop_{false};
  mutable std::mutex mutex_;
  mutable std::condition_variable closed_cv_;
  std::size_t accepted_ = 0;                     // under mutex_
  std::vector<SteadyClock::time_point> closed_;  // under mutex_
  std::thread thread_;                           // last: it uses the above
};

// A RawPeer's feed of bytes that are no stream, an HTTP reply, so each
// connection ends with a reason.
Bytes no_stream(std::uint64_t piece) {
  const std::string http = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
  return piece == 0 ? Bytes(http.begin(), http.end()) : Bytes();
}

// Publishes the tests' stream to a server on a thread of its own, looped and
// restamped, one record each `period`, until destroyed.
class Publisher {
 public:
  Publisher(networking::StreamServer& server, const Recording& recording,
            std::chrono::milliseconds period)
      : thread_([this, &server, &recording, period] {
          SteadyClock::time_point due = SteadyClock::now();
          for (std::uint64_t n = 0; !stop_; ++n) {
            const std::uint64_t i = n % kFrames;
            const stream::RecordInfo info =
                restamped(recording.infos[i], n / kFrames);
            const core::Status published =
                server.publish(info, recording.payloads[i].data(),
                               recording.payloads[i].size());
            EXPECT_TRUE(published.ok()) << published.message();
            if (info.key) {
              newest_key_ = info.seq;
            }
            due += period;
            std::this_thread::sleep_until(due);
          }
        }) {}
  ~Publisher() {
    stop_ = true;
    thread_.join();
  }
  Publisher(const Publisher&) = delete;
  Publisher& operator=(const Publisher&) = delete;

  // The newest key's seq published, or one before it: it is set after the
  // publish.
  std::uint64_t newest_key() const { return newest_key_; }

 private:
  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> newest_key_{0};
  std::thread thread_;  // last: it uses the above
};

class LivePlayerTest : public StreamPlayerTest {
 protected:
  void SetUp() override {
    StreamPlayerTest::SetUp();
    if (base_setup_incomplete() || HasFatalFailure()) {
      return;
    }
    ASSERT_NO_FATAL_FAILURE(read_recording(path_, &recording_));
  }

  // The stream at `path`, read back into `*recording`.
  static void read_recording(const std::string& path, Recording* recording) {
    core::Result<std::unique_ptr<stream::StreamReader>> reader =
        stream::StreamReader::open(path);
    ASSERT_TRUE(reader.ok()) << reader.status().message();
    *recording = {(*reader)->session(), {}, {}};
    for (const stream::Record& record : records(path)) {
      Bytes payload;
      const core::Status read = (*reader)->read(record, &payload);
      ASSERT_TRUE(read.ok()) << read.message();
      stream::RecordInfo info = record.info;
      info.pts_ns = recording->infos.size() * kLiveStepNs;
      recording->infos.push_back(info);
      recording->payloads.push_back(std::move(payload));
    }
    ASSERT_EQ(recording->infos.size(), kFrames);
  }

  // A server of `session`, the stream's by default, on 127.0.0.1, at `port`
  // or a free one; null with a failure recorded.
  std::unique_ptr<networking::StreamServer> serve(std::uint16_t port = 0) {
    return serve(recording_.session, port);
  }
  static std::unique_ptr<networking::StreamServer> serve(
      const stream::Session& session, std::uint16_t port = 0) {
    networking::ServerOptions options;
    options.host = "127.0.0.1";
    options.port = port;
    core::Result<std::unique_ptr<networking::StreamServer>> server =
        networking::StreamServer::start(session, options);
    EXPECT_TRUE(server.ok()) << server.status().message();
    return server.ok() ? std::move(server).value() : nullptr;
  }

  // Publish the stream's record `i` to `server`.
  void publish(networking::StreamServer& server, std::uint64_t i) {
    const core::Status published =
        server.publish(recording_.infos[i], recording_.payloads[i].data(),
                       recording_.payloads[i].size());
    ASSERT_TRUE(published.ok()) << published.message();
  }

  // A live player of `host`:`port`, started on the test's device, with
  // hooks_ when set; the fixture owns it.
  StreamPlayer* start_live(std::uint16_t port,
                           const std::string& host = "127.0.0.1") {
    core::Result<std::unique_ptr<StreamPlayer>> connected =
        StreamPlayer::connect({host, port});
    EXPECT_TRUE(connected.ok()) << connected.status().message();
    if (!connected.ok()) {
      return nullptr;
    }
    player_ = std::move(connected).value();
    const core::Status started =
        player_->start(device(), allocator(), extractor_config(), exchange_,
                       hooks_ ? &*hooks_ : nullptr);
    EXPECT_TRUE(started.ok()) << started.message();
    return started.ok() ? player_.get() : nullptr;
  }

  // Live, the tests' grow stream `stream` published to a server a record at
  // a time, each taken before the next, with compacting_hooks: every frame
  // comes, and each after_mesh's batch counted its frame's blocks on the
  // grid as it saw it, the first until record kGrownFrame and a grown one
  // from it.
  void live_through_compacting(const char* stream, bool grown_at_record) {
    ASSERT_NO_FATAL_FAILURE(use_stream(stream));
    ASSERT_NO_FATAL_FAILURE(read_recording(path_, &recording_));
    const std::vector<std::uint32_t> blocks = block_counts();
    ASSERT_NO_FATAL_FAILURE(expect_grown_at(blocks, grown_at_record));
    compacting_hooks();
    const std::unique_ptr<networking::StreamServer> server = serve();
    ASSERT_NE(server, nullptr);
    StreamPlayer* player = start_live(server->port());
    ASSERT_NE(player, nullptr);
    ASSERT_TRUE(reaches(LiveState::kConnected));
    FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
    for (std::uint64_t i = 0; i < kFrames; ++i) {
      ASSERT_NO_FATAL_FAILURE(publish(*server, i));
      const std::optional<FrameStamp> frame = take_with(taker);
      ASSERT_TRUE(frame.has_value())
          << "no frame " << i << ": " << player->status().message();
      EXPECT_EQ(frame->index, i);
    }
    EXPECT_LT(stop_time(*player), kPrompt);
    EXPECT_TRUE(player->status().ok()) << player->status().message();
    expect_grew_at_grown_frame(expect_compacted(blocks));
  }

  // Whether the player's connection reaches `state` within kArrival.
  bool reaches(LiveState state) {
    return eventually([&] { return player_->live_status().state == state; });
  }

  // Expects `status` to say why its connection ended with a reason starting
  // `start`: StreamClient's message less its "StreamClient: <address>: ", so
  // naming the address nowhere.
  static void expect_reason(const LiveStatus& status,
                            const std::string& start) {
    const std::string& why = status.last_error;
    EXPECT_EQ(why.rfind(start, 0), 0u) << why;
    EXPECT_EQ(why.find(status.address), std::string::npos) << why;
  }

  // The frames taken until one with `index` comes, each within kArrival.
  std::vector<FrameStamp> frames_through(std::uint64_t index) {
    std::vector<FrameStamp> frames;
    while (frames.empty() || frames.back().index != index) {
      const std::optional<FrameStamp> frame = next_frame();
      if (!frame) {
        ADD_FAILURE() << "no frame " << index;
        break;
      }
      frames.push_back(*frame);
    }
    return frames;
  }

  // The stream as a sender frames it, looped and restamped: piece 0 the
  // session's chunk, piece n its (n - 1)th record's, from a new encoder each
  // connection. For a RawPeer, on its thread.
  RawPeer::Feed stream_feed() {
    auto encoder = std::make_shared<std::optional<stream::ChunkEncoder>>();
    return [this, encoder](std::uint64_t piece) -> Bytes {
      if (piece == 0) {
        core::Result<stream::ChunkEncoder> made =
            stream::ChunkEncoder::create(recording_.session);
        EXPECT_TRUE(made.ok()) << made.status().message();
        if (!made.ok()) {
          return {};
        }
        encoder->emplace(std::move(made).value());
        return (*encoder)->session_chunk();
      }
      const std::uint64_t i = (piece - 1) % kFrames;
      const Bytes& payload = recording_.payloads[i];
      core::Result<stream::ChunkEncoder::RecordFraming> framing =
          (*encoder)->frame(
              restamped(recording_.infos[i], (piece - 1) / kFrames),
              payload.data(), payload.size());
      EXPECT_TRUE(framing.ok()) << framing.status().message();
      if (!framing.ok()) {
        return {};
      }
      Bytes chunk(framing->header.begin(), framing->header.end());
      chunk.insert(chunk.end(), payload.begin(), payload.end());
      chunk.resize(chunk.size() + framing->padding, 0);
      return chunk;
    };
  }

  // Time a burst with K0 published but untaken and P1 decoded behind it, or
  // with equally sized invalid K0/P1 payloads so no frame waits. The burst
  // also has valid framing but invalid codec payloads: none of the measured
  // work depends on the GPU. Framing and allocation precede the timer too.
  // For a large record, time the sender getting its bytes out through bounded
  // socket buffers, holding its last byte back until afterwards: the record's
  // CRC, expensive under TSan, cannot start within the timed interval. Its
  // send buffer holds a receive, so on Linux a worker that drains promptly
  // seldom finds the socket empty, where it may wait a slice.
  void measure_burst_drain(std::size_t count, std::size_t payload_size,
                           bool untaken, bool time_sender,
                           SteadyClock::duration* elapsed) {
    core::Result<stream::ChunkEncoder> made =
        stream::ChunkEncoder::create(recording_.session);
    ASSERT_TRUE(made.ok()) << made.status().message();
    stream::ChunkEncoder encoder = std::move(made).value();
    const auto append = [&encoder](const stream::RecordInfo& info,
                                   const Bytes& payload, Bytes* bytes) {
      const core::Result<stream::ChunkEncoder::RecordFraming> framing =
          encoder.frame(info, payload.data(), payload.size());
      EXPECT_TRUE(framing.ok()) << framing.status().message();
      if (!framing.ok()) {
        return false;
      }
      bytes->insert(bytes->end(), framing->header.begin(),
                    framing->header.end());
      bytes->insert(bytes->end(), payload.begin(), payload.end());
      bytes->resize(bytes->size() + framing->padding, 0);
      return true;
    };
    Bytes initial = encoder.session_chunk();
    const Bytes key = untaken ? recording_.payloads[0]
                              : Bytes(recording_.payloads[0].size(), 0);
    ASSERT_TRUE(append(recording_.infos[0], key, &initial));
    Bytes behind;
    const Bytes predicted = untaken ? recording_.payloads[1]
                                    : Bytes(recording_.payloads[1].size(), 0);
    ASSERT_TRUE(append(recording_.infos[1], predicted, &behind));
    stream::RecordInfo info = recording_.infos[1];
    info.seq = 2;
    info.pts_ns = info.seq * kLiveStepNs;
    ASSERT_TRUE(append(info, Bytes(1, 0), &behind));
    Bytes burst;
    const Bytes payload(payload_size, 0);
    for (std::size_t i = 0; i < count; ++i) {
      ++info.seq;
      info.pts_ns = info.seq * kLiveStepNs;
      ASSERT_TRUE(append(info, payload, &burst));
    }
    Bytes tail;
    if (time_sender) {
      // The last payload byte and any padding, so the body stays incomplete.
      const std::size_t withheld = 1 + (8 - payload_size % 8) % 8;
      tail.assign(burst.end() - withheld, burst.end());
      burst.resize(burst.size() - withheld);
    }
    const std::uint64_t last = info.seq;
    std::atomic<unsigned> phase{0};
    const RawPeer peer(
        [initial = std::move(initial), behind = std::move(behind),
         burst = std::move(burst), tail = std::move(tail), &phase,
         sent = 0u](std::uint64_t) mutable {
          if (sent == 0) {
            ++sent;
            return std::move(initial);
          }
          if (sent == 1 && phase >= 1) {
            ++sent;
            return std::move(behind);
          }
          if (sent == 2 && phase >= 2) {
            ++sent;
            return std::move(burst);
          }
          if (sent == 3) {
            ++sent;
            phase = 3;  // the preceding send_all completed
          }
          if (sent == 4 && phase >= 4) {
            ++sent;
            return std::move(tail);
          }
          return Bytes();
        },
        1ms, time_sender ? static_cast<int>(kClientReceive) : 0);
    StreamPlayer* player = start_live(peer.port());
    ASSERT_NE(player, nullptr);
    if (untaken) {
      const std::atomic<bool> cancel{false};
      ASSERT_TRUE(
          eventually([&] { return !exchange_.wait_collected(0ms, cancel); }));
    } else {
      ASSERT_TRUE(eventually([&] {
        const LiveStatus status = player->live_status();
        return status.state == LiveState::kLive && status.seq == 0;
      }));
    }
    phase = 1;
    // Seeing record 2 proves P1's decode finished (or was skipped). It does
    // not replace a decoded P1, which remains behind the untaken K0.
    ASSERT_TRUE(eventually([&] { return player->live_status().seq == 2; }));
    const SteadyClock::time_point sent = SteadyClock::now();
    phase = 2;
    ASSERT_TRUE(eventually([&] {
      return time_sender ? phase == 3 : player->live_status().seq == last;
    }));
    *elapsed = SteadyClock::now() - sent;
    if (time_sender) {
      EXPECT_EQ(player->live_status().seq, 2u) << "body must remain incomplete";
    }
    phase = 4;
    RecordProperty(
        untaken ? "drain_ms" : "baseline_ms",
        std::chrono::duration_cast<std::chrono::milliseconds>(*elapsed)
            .count());
    ASSERT_TRUE(eventually([&] { return player->live_status().seq == last; }));
    if (untaken) {
      const std::optional<FrameStamp> first = take(kArrival);
      ASSERT_TRUE(first.has_value());
      EXPECT_EQ(first->index, 0u);
      const std::optional<FrameStamp> next = take(kArrival);
      ASSERT_TRUE(next.has_value());
      EXPECT_EQ(next->index, 1u);
      EXPECT_EQ(next->records, count + 2);
    }
    EXPECT_LT(stop_time(*player), kPrompt);
    EXPECT_TRUE(player->status().ok()) << player->status().message();
  }

  // Publish record `n` of the stream looped, its pts n steps: a sender's
  // steady pace, with no pass's gap.
  void publish_steady(networking::StreamServer& server, std::uint64_t n,
                      std::uint64_t step_ns = kLiveStepNs) {
    publish_at(server, n, n * step_ns);
  }
  // Publish record `n` of the stream looped, at `pts_ns`.
  void publish_at(networking::StreamServer& server, std::uint64_t n,
                  std::uint64_t pts_ns) {
    stream::RecordInfo info = recording_.infos[n % kFrames];
    info.seq = n;
    info.pts_ns = pts_ns;
    const Bytes& payload = recording_.payloads[n % kFrames];
    const core::Status published =
        server.publish(info, payload.data(), payload.size());
    ASSERT_TRUE(published.ok()) << published.message();
  }

  Recording recording_;
};

TEST_F(LivePlayerTest, FramesArriveWithTheirSeqAsTheirIndex) {
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  StreamPlayer* player = start_live(server->port());
  ASSERT_NE(player, nullptr);
  EXPECT_TRUE(player->live());
  ASSERT_TRUE(reaches(LiveState::kConnected));
  // One at a time, each taken before the next goes.
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    ASSERT_NO_FATAL_FAILURE(publish(*server, i));
    const std::optional<FrameStamp> frame = next_frame();
    ASSERT_TRUE(frame.has_value()) << "no frame " << i;
    EXPECT_EQ(frame->index, i);
    EXPECT_EQ(frame->pts_ns, recording_.infos[i].pts_ns);
    EXPECT_EQ(frame->bytes, recording_.payloads[i].size());
    EXPECT_EQ(frame->key, i % kIntraPeriod == 0);
    // Its record's set, as the status line's; kNoSet for the record that
    // names none.
    EXPECT_EQ(frame->set, vp_test::set_of(i));
    EXPECT_EQ(player->live_status().set, vp_test::set_of(i));
    EXPECT_EQ(frame->records, 1u);
    EXPECT_EQ(frame->record_bytes, frame->bytes);
    EXPECT_TRUE(frame->deblock_on);
    // No "read": the wait for the network is no stage. Its decode is a
    // batch of its own, and so is its mesh.
    const std::vector<core::StageRow>& rows = frame->stages.rows();
    ASSERT_EQ(rows.size(), 5u) << "frame " << i;
    EXPECT_STREQ(rows[0].name, "decode");
    EXPECT_STREQ(rows[1].name, "submit");
    EXPECT_STREQ(rows[2].name, "wait: GPU");
    EXPECT_STREQ(rows[3].name, "finish");
    EXPECT_STREQ(rows[4].name, "mesh");
    // Due once its record came whole, and published after; its mesh is of
    // its own decode, finished before it was meshed.
    EXPECT_NE(frame->due, SteadyClock::time_point{});
    EXPECT_LE(frame->due, frame->published);
    EXPECT_NEAR(mean_x(exchange_.live()), kMetresAFrame * i, 0.02);
  }
  const LiveStatus status = player->live_status();
  EXPECT_EQ(status.state, LiveState::kLive);
  EXPECT_EQ(status.seq, kFrames - 1);
  EXPECT_EQ(status.address, "127.0.0.1:" + std::to_string(server->port()));
  EXPECT_TRUE(status.last_error.empty()) << status.last_error;
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// Records one at a time, each taken before the next goes: each is decoded in
// a batch of its own, read straight into one of the decoder's input slots,
// and meshed in another, so a frame is two submits and, once the slots have
// grown to the stream's records, no staging buffer. Taken with no device work
// of the test's.
TEST_F(LivePlayerTest, EachRecordIsOneDecodeBatchAndEachMeshOne) {
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  StreamPlayer* player = start_live(server->port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  for (std::uint64_t n = 0; n < 2 * kFrames; ++n) {
    const core::Status published =
        server->publish(restamped(recording_.infos[n % kFrames], n / kFrames),
                        recording_.payloads[n % kFrames].data(),
                        recording_.payloads[n % kFrames].size());
    ASSERT_TRUE(published.ok()) << published.message();
    const std::optional<FrameStamp> frame = take();
    ASSERT_TRUE(frame.has_value()) << "no frame " << n;
    EXPECT_EQ(frame->records, 1u);
    if (n >= kFrames) {
      EXPECT_EQ(frame->counters.submits, 2u) << "frame " << n;
      EXPECT_EQ(frame->counters.staging_buffers, 0u) << "frame " << n;
    }
  }
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// P2 arrives while P1's decode is held on the GPU. Its due stamp identifies
// that arrival, so publication latency includes the wait for P1's decode.
// Warm all capacities first, and leave K0's mesh untaken so the worker keeps
// reading records rather than waiting to mesh P1.
TEST_F(LivePlayerTest, ArrivalIncludesWaitingForThePreviousDecode) {
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  StreamPlayer* player = start_live(server->port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    ASSERT_NO_FATAL_FAILURE(publish(*server, i));
    ASSERT_TRUE(take().has_value()) << "warm frame " << i;
  }
  const auto send = [&](std::uint64_t i) {
    const Bytes& bytes = recording_.payloads[i];
    const core::Status published = server->publish(
        restamped(recording_.infos[i], 1), bytes.data(), bytes.size());
    EXPECT_TRUE(published.ok()) << published.message();
    return published.ok() &&
           eventually([&] { return player->live_status().seq == kFrames + i; });
  };
  ASSERT_TRUE(send(0));
  const std::atomic<bool> cancel{false};
  ASSERT_TRUE(
      eventually([&] { return !exchange_.wait_collected(0ms, cancel); }));

  core::Result<core::TimelineSemaphore> gate =
      core::TimelineSemaphore::create(device());
  ASSERT_TRUE(gate.ok()) << gate.status().message();
  core::CommandBatch batch(device(), allocator());
  core::Result<core::PendingBatch> pending =
      batch.submit_async({{&*gate, 1}}, {});
  ASSERT_TRUE(pending.ok()) << pending.status().message();
  // Declared after the pending batch: an assertion failure still releases
  // its wait before the batch's destructor or the fixture stops the worker.
  struct Release {
    core::TimelineSemaphore& gate;
    bool done = false;
    void signal() {
      if (!done) {
        const core::Status signalled = gate.signal(1);
        EXPECT_TRUE(signalled.ok()) << signalled.message();
        done = signalled.ok();
      }
    }
    ~Release() { signal(); }
  } release{*gate};
  ASSERT_TRUE(send(1));
  ASSERT_TRUE(send(2));
  const SteadyClock::time_point arrived = player->live_status().last_record;
  ASSERT_NE(arrived, SteadyClock::time_point{});
  std::this_thread::sleep_for(50ms);  // a known stall, not a speed threshold
  const SteadyClock::time_point released = SteadyClock::now();
  release.signal();
  const core::Status finished = pending->wait();
  ASSERT_TRUE(finished.ok()) << finished.message();

  const std::optional<FrameStamp> held = take();
  ASSERT_TRUE(held.has_value());
  EXPECT_EQ(held->index, kFrames);
  const std::optional<FrameStamp> newest = take();
  ASSERT_TRUE(newest.has_value());
  ASSERT_EQ(newest->index, kFrames + 2);
  EXPECT_EQ(newest->due, arrived);
  EXPECT_LE(newest->due, released);
  EXPECT_GE(newest->published, released);
  EXPECT_GE(newest->published - newest->due, released - arrived);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// The setting each frame carries, and what the deblock did to it, are the
// decoder's at its record: on, the sphere's filter, LG; off, none; every
// block exact either way, as each holds an unobserved voxel.
TEST_F(LivePlayerTest, ADeblockToggleReachesTheNextRecord) {
  const std::vector<std::uint32_t> blocks = block_counts();
  ASSERT_EQ(blocks.size(), kFrames);
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  StreamPlayer* player = start_live(server->port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  for (std::uint64_t i = 0; i < 3; ++i) {
    const bool on = i != 1;
    player->set_deblock(on);
    ASSERT_NO_FATAL_FAILURE(publish(*server, i));
    const std::optional<FrameStamp> frame = next_frame();
    ASSERT_TRUE(frame.has_value()) << "no frame " << i;
    EXPECT_EQ(frame->index, i);
    EXPECT_EQ(frame->deblock_on, on) << "frame " << i;
    EXPECT_STREQ(frame->deblock.filter, on ? "LG" : "off");
    EXPECT_NEAR(frame->deblock.step, kSphereStep, kStepError);
    EXPECT_EQ(frame->deblock.block_count, blocks[i]) << "frame " << i;
    EXPECT_EQ(frame->deblock.exact_blocks, blocks[i]) << "frame " << i;
    EXPECT_FALSE(frame->rebuild);
  }
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(LivePlayerTest, AJoinAfterAPFrameShowsThatGopFromItsKey) {
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  // K0 P1 P2 K3 P4, before the player connects: the server keeps K3 P4.
  for (std::uint64_t i = 0; i < 5; ++i) {
    ASSERT_NO_FATAL_FAILURE(publish(*server, i));
  }
  ASSERT_NE(start_live(server->port()), nullptr);
  // P4 decodes only after K3, and either may be meshed first.
  const std::vector<FrameStamp> frames = frames_through(4);
  std::uint64_t records = 0;
  for (const FrameStamp& frame : frames) {
    EXPECT_GE(frame.index, 3u);
    records += frame.records;
  }
  EXPECT_EQ(records, 2u) << "K3 and P4";
  EXPECT_TRUE(player_->status().ok()) << player_->status().message();
}

// A connection that begins at a P-frame, as from a sender that does not join
// at a key: the P-frames before the first key are skipped, though counted as
// received, and the first frame shown is the key's or after it.
TEST_F(LivePlayerTest, PFramesBeforeTheFirstKeyAreSkipped) {
  const RawPeer peer(
      [feed = stream_feed()](std::uint64_t piece) {
        return feed(piece == 0 ? 0 : piece + 1);  // the session, then P1 on
      },
      10ms);
  StreamPlayer* player = start_live(peer.port());
  ASSERT_NE(player, nullptr);
  const std::optional<FrameStamp> frame = next_frame();
  ASSERT_TRUE(frame.has_value());
  EXPECT_GE(frame->index, kIntraPeriod);
  EXPECT_EQ(frame->records, frame->index) << "P1 on, the skipped included";
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(LivePlayerTest, AllPublishedWhileAMeshWaitsAreDecodedAndTheNewestShown) {
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  StreamPlayer* player = start_live(server->port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  // Nothing is taken until every record has come: the first meshed (K0, or
  // P1 if it came first) waits, and the rest are decoded behind it. Each goes
  // once the one before has come, or the server could skip P1 and P2 for K3
  // (newest key wins).
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    ASSERT_NO_FATAL_FAILURE(publish(*server, i));
    ASSERT_TRUE(eventually([&] {
      const LiveStatus status = player->live_status();
      return status.state == LiveState::kLive && status.seq == i;
    })) << "record "
        << i;
  }
  const std::vector<FrameStamp> frames = frames_through(kFrames - 1);
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_LE(frames[0].index, 1u);
  EXPECT_GT(frames[1].records, 1u);
  EXPECT_EQ(frames[0].records + frames[1].records, kFrames);
  std::uint64_t bytes = 0;
  for (const Bytes& payload : recording_.payloads) {
    bytes += payload.size();
  }
  EXPECT_EQ(frames[0].record_bytes + frames[1].record_bytes, bytes);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(LivePlayerTest, AnUntakenMeshDoesNotDelayBufferedRecords) {
  // All 128 tiny records fit in one receive. A wait per record adds at
  // least 1.28 s even though parsing and skipping them takes little time.
  SteadyClock::duration elapsed{};
  ASSERT_NO_FATAL_FAILURE(measure_burst_drain(128, 1, /*untaken=*/true,
                                              /*time_sender=*/false, &elapsed));
  EXPECT_LT(elapsed, 750ms)
      << std::chrono::duration<double, std::milli>(elapsed).count()
      << " ms to drain queued records with an untaken mesh";
}

TEST_F(LivePlayerTest, AnUntakenMeshDoesNotDelayAPartialRecord) {
  // One record of 16 receives must keep draining while no record is ready.
  // A worker that waited its 10 ms slice after each partial receive would
  // add at least 120 ms: when the sender finishes, the socket buffers, which
  // Linux doubles, hold at most 4 receives' bytes. The limit is 80 ms over
  // the same receive/parser work without a pending frame, plus a quarter of
  // that work's time, so a sanitizer's slower, more variable receives are
  // not mistaken for waits; there, where that work takes seconds, only
  // longer waits fail. Linux alone checks it: on macOS a healthy worker can
  // empty the socket at about every receive and wait a slice, as the
  // regression would (CI run 39897310418, attempt 1, macos-26 Release, with
  // a 256 KiB send buffer: 1,100 ms added, about a slice per 300 KiB).
  constexpr std::size_t kRecord = 16 * kClientReceive;
  SteadyClock::duration baseline{};
  ASSERT_NO_FATAL_FAILURE(measure_burst_drain(1, kRecord, /*untaken=*/false,
                                              /*time_sender=*/true, &baseline));
  SteadyClock::duration elapsed{};
  ASSERT_NO_FATAL_FAILURE(measure_burst_drain(1, kRecord, /*untaken=*/true,
                                              /*time_sender=*/true, &elapsed));
#ifndef __APPLE__
  EXPECT_LT(elapsed, baseline + baseline / 4 + 80ms)
      << std::chrono::duration<double, std::milli>(elapsed - baseline).count()
      << " ms added to socket draining by an untaken mesh";
#endif
}

// Records one at a time, each published once the frame before is taken: each
// is meshed as soon as it is decoded, as nothing newer has come, rather than
// held until 100 ms after the last mesh. A frame's wait is timed beyond its
// own decode and mesh, which a loaded runner's GPU slows.
TEST_F(LivePlayerTest, ARecordIsMeshedOnceNothingNewerHasCome) {
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  StreamPlayer* player = start_live(server->port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  std::vector<double> waits;  // ms, from each record's publish to its frame
  for (std::uint64_t n = 0; n < 4 * kFrames; ++n) {
    const std::uint64_t i = n % kFrames;
    const SteadyClock::time_point sent = SteadyClock::now();
    const core::Status published = server->publish(
        restamped(recording_.infos[i], n / kFrames),
        recording_.payloads[i].data(), recording_.payloads[i].size());
    ASSERT_TRUE(published.ok()) << published.message();
    const std::optional<FrameStamp> frame = take(kArrival);
    ASSERT_TRUE(frame.has_value()) << "no frame " << n;
    double wait =
        std::chrono::duration<double, std::milli>(SteadyClock::now() - sent)
            .count();
    for (const core::StageRow& row : frame->stages.rows()) {
      wait -= row.cpu_ms;  // its decode and mesh
    }
    if (n > 0) {  // the first has no mesh before it to wait on
      waits.push_back(wait);
    }
  }
  std::nth_element(waits.begin(), waits.begin() + waits.size() / 2,
                   waits.end());
  EXPECT_LT(waits[waits.size() / 2], 50.0) << "ms, the median";
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// A frame meshed and left untaken, B, then the next record, C, decoded
// behind it: the take of B wakes the worker, which waits for it with C in
// hand, and C is meshed at once. Each take comes about 1 ms after C's decode
// was submitted, early in the first of the worker's 10 ms slices of waiting.
// C's wait, from the take to its publish beyond its own work since (the wait
// for its decode, its finish and its mesh), is within half a slice of B's
// lateness, from its arrival to its publish beyond its own work, by their
// medians of 15, as a loaded host delays both (CI run 40822414889). A worker
// that saw the take only at a slice's end, after a timed wait or the
// socket's, adds most of a slice to C's alone, 8-11 ms under TSan and in
// Release. A take that comes before the worker begins to wait shows no delay
// even with that bug, hence medians rather than every sample. B itself is
// held a turn after it comes, with the mesh taken: a worker that waits a
// slice there, on the socket or for a take that has already come, makes
// every B a slice late, where a loaded host makes only some late, so B's
// least lateness must be under half a slice. Once B's median is half a slice
// late, the bound grows past what the bug adds, so the test skips there.
// Each sample starts from a frame taken, so the worker is idle, and C goes
// once B's mesh has run, so B is published before C is read.
TEST_F(LivePlayerTest, ATakeWithAFrameInHandIsMeshedAtOnce) {
  constexpr auto kSlice = 10ms;  // the worker's (stream_player.cpp)
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  StreamPlayer* player = start_live(server->port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  std::uint64_t n = 0;
  // The stream's next record, looped and restamped: whether it came.
  const auto send = [&] {
    const stream::RecordInfo info =
        restamped(recording_.infos[n % kFrames], n / kFrames);
    const Bytes& payload = recording_.payloads[n % kFrames];
    ++n;
    return server->publish(info, payload.data(), payload.size()).ok() &&
           eventually([&] { return player->live_status().seq == info.seq; });
  };
  // ms from `from` to `frame`'s publish beyond its rows named in `own`.
  const auto beyond = [](const FrameStamp& frame, SteadyClock::time_point from,
                         std::initializer_list<const char*> own) {
    double ms =
        std::chrono::duration<double, std::milli>(frame.published - from)
            .count();
    for (const core::StageRow& row : frame.stages.rows()) {
      for (const char* name : own) {
        if (std::strcmp(row.name, name) == 0) {
          ms -= row.cpu_ms;
        }
      }
    }
    return ms;
  };
  const auto median = [](std::vector<double> ms) {
    std::nth_element(ms.begin(), ms.begin() + ms.size() / 2, ms.end());
    return ms[ms.size() / 2];
  };
  constexpr std::size_t kSamples = 15;
  std::vector<double> late;   // ms, each B's
  std::vector<double> waits;  // ms, each C's
  while (waits.size() < kSamples) {
    ASSERT_TRUE(send());
    ASSERT_TRUE(take(kArrival).has_value());
    // B's mesh sets the value after this one's on the extractor's timeline.
    const core::TimelinePoint ready = exchange_.live().ready;
    ASSERT_NE(ready.semaphore, nullptr);
    ASSERT_TRUE(send());  // B
    const core::Status meshed = ready.semaphore->wait(
        ready.value + 1,
        std::chrono::duration_cast<std::chrono::nanoseconds>(kArrival).count());
    ASSERT_TRUE(meshed.ok()) << meshed.message();
    const std::uint64_t submits = device().counters().submits;
    ASSERT_TRUE(send());                    // C
    ASSERT_TRUE(submitted_after(submits));  // its decode
    std::this_thread::sleep_for(1ms);       // so the worker waits for the take
    const SteadyClock::time_point taken_at = SteadyClock::now();
    const std::optional<FrameStamp> b = take(kArrival);
    ASSERT_TRUE(b.has_value());
    ASSERT_EQ(b->records, 1u) << "C came before B was meshed";
    const std::optional<FrameStamp> c = take(kArrival);
    ASSERT_TRUE(c.has_value());
    late.push_back(beyond(*b, b->due,
                          {"decode", "submit", "wait: GPU", "finish", "mesh"}));
    waits.push_back(beyond(*c, taken_at, {"wait: GPU", "finish", "mesh"}));
  }
  EXPECT_TRUE(player->status().ok()) << player->status().message();
  const double half_slice =
      std::chrono::duration<double, std::milli>(kSlice).count() / 2;
  ASSERT_LT(*std::min_element(late.begin(), late.end()), half_slice)
      << "ms, B's least lateness: every B waited a slice with the mesh taken";
  const double b_late = median(late);
  if (b_late >= half_slice) {
    GTEST_SKIP() << "B's median lateness, " << b_late
                 << " ms, is half a slice or more: too loaded a host to tell";
  }
  EXPECT_LT(median(waits), b_late + half_slice)
      << "ms, C's median wait after the take; B's median lateness, " << b_late
      << " ms";
}

// Records faster than their decode, so one is always in hand: a newer frame
// still comes every 100 ms or so, rather than once the backlog drains, which
// here it never does.
TEST_F(LivePlayerTest, RecordsFasterThanTheirDecodeStillShowNewerFrames) {
  const RawPeer peer(stream_feed(), 0ms);
  StreamPlayer* player = start_live(peer.port());
  ASSERT_NE(player, nullptr);
  std::optional<std::uint64_t> last;
  for (int n = 0; n < 5; ++n) {
    const std::optional<FrameStamp> frame = next_frame();
    ASSERT_TRUE(frame.has_value()) << "frame " << n;
    if (last) {
      EXPECT_GT(frame->index, *last);
    }
    last = frame->index;
  }
  EXPECT_FALSE(peer.closed(1, 0s).has_value()) << "the peer stopped";
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// A consumer that takes nothing for 1.5 s while records flow, each frame it
// takes going into PlaybackStats as the view takes it: status() stays OK, as
// the ring has 3 slots and an extract per record would run out of them; the
// frame that waited, then the newest decoded since, which stands for every
// record of the pause, come a moment apart, yet the records a second read at
// most twice the flow's; and a second of frames on, they read again.
TEST_F(LivePlayerTest, AConsumerPauseLeavesStatusOkThenGetsTheNewest) {
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  const Publisher flow(*server, recording_, kFlow);
  StreamPlayer* player = start_live(server->port());
  ASSERT_NE(player, nullptr);
  PlaybackStats stats(player->track_stats(), player->rate());
  const auto taken = [&] {
    const std::optional<FrameStamp> frame = take(kArrival);
    if (frame) {
      stats.add_frame(*frame, PlaybackStats::Clock::now());
    }
    return frame;
  };
  constexpr double kMost = 2.0 * static_cast<double>(1s / kFlow);
  ASSERT_TRUE(taken().has_value());
  std::this_thread::sleep_for(1500ms);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
  const std::uint64_t newest = player->live_status().seq;
  ASSERT_TRUE(taken().has_value());  // the frame that waited
  const std::optional<FrameStamp> next = taken();
  ASSERT_TRUE(next.has_value());
  EXPECT_GE(next->index, newest);
  EXPECT_GT(next->records, 1u);
  EXPECT_LE(stats.records_per_s(PlaybackStats::Clock::now()), kMost);
  const SteadyClock::time_point until = SteadyClock::now() + 1s;
  while (SteadyClock::now() < until) {
    ASSERT_TRUE(taken().has_value());
  }
  const double records = stats.records_per_s(PlaybackStats::Clock::now());
  EXPECT_GT(records, 0.0);
  EXPECT_LE(records, kMost);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(LivePlayerTest, AServerGoneIsARetryAndANewOneOnItsPortGivesFrames) {
  std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  const std::uint16_t port = server->port();
  StreamPlayer* player = start_live(port);
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  ASSERT_NO_FATAL_FAILURE(publish(*server, 0));
  const std::optional<FrameStamp> first = next_frame();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->index, 0u);
  server.reset();
  ASSERT_TRUE(eventually([&] {
    const LiveStatus status = player->live_status();
    return status.state == LiveState::kConnecting && !status.last_error.empty();
  }));
  EXPECT_EQ(player->live_status().last_error,
            "the sender closed the connection");
  EXPECT_TRUE(player->status().ok()) << player->status().message();
  // A new server on the same port, of the stream at twice the voxel size,
  // which a grid of the last connection's would refuse: frames again.
  const std::string coarse =
      scratch_.copy(VP_TEST_STREAMS, vp_test::kCoarseStream);
  ASSERT_FALSE(coarse.empty());
  ASSERT_NO_FATAL_FAILURE(read_recording(coarse, &recording_));
  server = serve(port);
  ASSERT_NE(server, nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  for (std::uint64_t i = 0; i < kIntraPeriod; ++i) {
    ASSERT_NO_FATAL_FAILURE(publish(*server, i));
    const std::optional<FrameStamp> frame = next_frame();
    ASSERT_TRUE(frame.has_value()) << "no frame " << i;
    EXPECT_EQ(frame->index, i);
  }
  // A park ends the connection, and nothing failed: no reason is left.
  ASSERT_TRUE(player->park(kArrival));
  EXPECT_TRUE(eventually([&] {
    const LiveStatus status = player->live_status();
    return status.state == LiveState::kConnecting && status.last_error.empty();
  })) << player->live_status().last_error;
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// Parked at points spread evenly over 0-20 ms while records arrive every 10
// ms, as one is in hand, a decode or an extract is under way or a receive
// waits for the next: each time the other end sees the connection close
// within kClosed of the park's return, and the unpark connects again. 8
// times, or VP_PARK_CYCLES times: the stress soak's 50 (CMakeLists.txt).
TEST_F(LivePlayerTest, AParkClosesTheConnectionWithinASlice) {
  const char* const asked = std::getenv("VP_PARK_CYCLES");
  const std::size_t cycles =
      asked != nullptr ? std::strtoul(asked, nullptr, 10) : 8;
  ASSERT_GE(cycles, 2u) << "VP_PARK_CYCLES=" << asked;
  const RawPeer peer(stream_feed(), 10ms);
  StreamPlayer* player = start_live(peer.port());
  ASSERT_NE(player, nullptr);
  for (std::size_t n = 1; n <= cycles; ++n) {
    SCOPED_TRACE(::testing::Message() << "park " << n);
    // Records arriving on connection n, its frames taken as they come.
    ASSERT_TRUE(eventually([&] {
      exchange_.begin_frame(slot_,
                            [](const auto&, FrameStamp&) { return true; });
      slot_ = (slot_ + 1) % kFramesInFlight;
      return peer.accepted() == n &&
             player->live_status().state == LiveState::kLive;
    }));
    std::this_thread::sleep_for(
        std::chrono::milliseconds(20 * (n - 1) / (cycles - 1)));
    ASSERT_TRUE(player->park(kArrival));
    const SteadyClock::time_point parked = SteadyClock::now();
    const std::uint64_t submits = device().counters().submits;
    const std::optional<SteadyClock::time_point> closed = peer.closed(n);
    ASSERT_TRUE(closed.has_value()) << "the connection stayed open";
    EXPECT_LT(*closed - parked, kClosed);
    // A decode in flight was waited out at the park, and nothing since.
    EXPECT_EQ(device().counters().submits, submits) << "submitted while parked";
    player->unpark();
  }
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// Parked while no record comes, as from a sender with no key yet: no step
// refuses one, and the connection still closes within kClosed.
TEST_F(LivePlayerTest, AParkClosesAnIdleConnectionWithinASlice) {
  const RawPeer peer(
      [feed = stream_feed()](std::uint64_t piece) {
        return piece == 0 ? feed(0) : Bytes();  // the session alone
      },
      10ms);
  StreamPlayer* player = start_live(peer.port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  ASSERT_TRUE(player->park(kArrival));
  const SteadyClock::time_point parked = SteadyClock::now();
  const std::optional<SteadyClock::time_point> closed = peer.closed(1);
  ASSERT_TRUE(closed.has_value()) << "the connection stayed open";
  EXPECT_LT(*closed - parked, kClosed);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(LivePlayerTest, AnUnparkJoinsAtTheNewestKey) {
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  const Publisher flow(*server, recording_, kFlow);
  StreamPlayer* player = start_live(server->port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(next_frame().has_value());
  ASSERT_TRUE(player->park(kArrival));
  // A frame published before the park returned may wait to be taken; no
  // other comes, while the server moves on by several keys.
  next_frame(kQuiet);
  EXPECT_FALSE(next_frame(kQuiet).has_value()) << "published while parked";
  const std::uint64_t key = flow.newest_key();
  player->unpark();
  const std::optional<FrameStamp> frame = next_frame();
  ASSERT_TRUE(frame.has_value()) << "nothing after unpark";
  EXPECT_GE(frame->index, key);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(LivePlayerTest, APeerSendingNoStreamShowsWhyWhileStatusStaysOk) {
  const RawPeer peer(no_stream, 10ms);
  StreamPlayer* player = start_live(peer.port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(eventually([&] {
    const LiveStatus status = player->live_status();
    return status.state == LiveState::kConnecting && !status.last_error.empty();
  }));
  expect_reason(player->live_status(), "ChunkParser: ");
  EXPECT_TRUE(player->status().ok()) << player->status().message();
  EXPECT_FALSE(next_frame(kQuiet).has_value());
  // It connects again, about a second on.
  EXPECT_TRUE(eventually([&] { return peer.accepted() >= 2; }));
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// A park while a failed connection waits to retry ends the wait: none is
// made while parked, and the unpark connects again at once, not when the
// second is up.
TEST_F(LivePlayerTest, AParkEndsTheWaitToRetry) {
  const RawPeer peer(no_stream, 10ms);
  StreamPlayer* player = start_live(peer.port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(
      eventually([&] { return !player->live_status().last_error.empty(); }));
  ASSERT_TRUE(player->park(kArrival));
  std::this_thread::sleep_for(kQuiet);  // the worker sees the park
  EXPECT_EQ(peer.accepted(), 1u) << "connected while parked";
  player->unpark();
  const SteadyClock::time_point unparked = SteadyClock::now();
  ASSERT_TRUE(eventually([&] { return peer.accepted() >= 2; }));
  const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
      SteadyClock::now() - unparked);
  EXPECT_LT(waited, kQuiet) << waited.count() << " ms";
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// A connection that fails just after a park, before the worker has seen it
// (bytes that are no chunk, sent as the park returns, almost always reach
// the receive under way): the park ends that failure's wait to retry too,
// so the unpark connects again at once.
TEST_F(LivePlayerTest, AParkJustBeforeAFailureEndsItsWaitToRetry) {
  std::atomic<bool> fail{false};
  const RawPeer peer(
      [feed = stream_feed(), &fail](std::uint64_t piece) {
        if (piece == 0) {
          return feed(0);  // the session
        }
        return fail ? Bytes(4096, 0xff) : Bytes();
      },
      1ms);
  StreamPlayer* player = start_live(peer.port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  ASSERT_TRUE(player->park(kArrival));
  fail = true;
  ASSERT_TRUE(peer.closed(1).has_value()) << "the connection stayed open";
  std::this_thread::sleep_for(kQuiet);  // the worker sees the park
  EXPECT_EQ(peer.accepted(), 1u) << "connected while parked";
  fail = false;
  player->unpark();
  const SteadyClock::time_point unparked = SteadyClock::now();
  ASSERT_TRUE(eventually([&] { return peer.accepted() >= 2; }));
  const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
      SteadyClock::now() - unparked);
  EXPECT_LT(waited, kQuiet) << waited.count() << " ms";
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// A sender of a track of another kind alone.
TEST_F(LivePlayerTest, ASessionWithNoGeometryTrackShowsWhyWhileStatusStaysOk) {
  stream::Session session;
  stream::Track track;
  track.kind = static_cast<stream::TrackKind>(2);
  session.tracks.push_back(track);
  const std::unique_ptr<networking::StreamServer> server = serve(session);
  ASSERT_NE(server, nullptr);
  StreamPlayer* player = start_live(server->port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(eventually([&] {
    const LiveStatus status = player->live_status();
    return status.state == LiveState::kConnecting && !status.last_error.empty();
  }));
  EXPECT_EQ(player->live_status().last_error,
            "the sender's session has no geometry track");
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// .invalid never resolves (RFC 6761).
TEST_F(LivePlayerTest, ANameThatDoesNotResolveShowsWhyWhileStatusStaysOk) {
  StreamPlayer* player = start_live(47801, "lab.invalid");
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(
      eventually([&] { return !player->live_status().last_error.empty(); }));
  expect_reason(player->live_status(), "cannot look up the name: ");
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(LivePlayerTest, AStopIsPromptWhileARefusedConnectWaitsToRetry) {
  StreamPlayer* player = start_live(refusing_port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(
      eventually([&] { return !player->live_status().last_error.empty(); }));
  EXPECT_EQ(player->live_status().last_error,
            "cannot connect: Connection refused");
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(LivePlayerTest, AStopIsPromptWhileConnectedAndIdle) {
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  StreamPlayer* player = start_live(server->port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  std::this_thread::sleep_for(kQuiet);
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(LivePlayerTest, AStopIsPromptWhileParked) {
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  StreamPlayer* player = start_live(server->port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  ASSERT_TRUE(player->park(kArrival));
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// Live, after_mesh runs once before each publish, with the grid its mesh was
// made from, holding that record's blocks, and the mesh is committed ready
// at after_mesh's batch's point; a park runs on_park, and the unpark
// on_resume.
TEST_F(LivePlayerTest, AfterMeshRunsBeforeEachLivePublishWithItsMeshsGrid) {
  const std::vector<std::uint32_t> blocks = block_counts();
  ASSERT_EQ(blocks.size(), kFrames);
  logging_hooks();
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  StreamPlayer* player = start_live(server->port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  std::vector<std::uint64_t> generations;
  std::vector<const core::TimelineSemaphore*> readies;  // as committed
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    ASSERT_NO_FATAL_FAILURE(publish(*server, i));
    const std::optional<FrameStamp> frame = take_with(taker);
    ASSERT_TRUE(frame.has_value()) << "no frame " << i;
    EXPECT_EQ(frame->index, i);
    generations.push_back(exchange_.live().generation);
    readies.push_back(exchange_.live().ready.semaphore);
  }
  const std::vector<Meshed> meshed = log_.meshed();
  ASSERT_EQ(meshed.size(), kFrames);
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    EXPECT_EQ(meshed[i].index, i);
    EXPECT_EQ(meshed[i].blocks, blocks[i]) << "frame " << i;
    EXPECT_EQ(meshed[i].generation, generations[i]) << "frame " << i;
    EXPECT_NE(readies[i], nullptr) << "frame " << i;
    EXPECT_NE(readies[i], meshed[i].ready)
        << "frame " << i << " published at the extract's point";
  }
  ASSERT_TRUE(player->park(kArrival));
  EXPECT_EQ(log_.parks, 1);
  EXPECT_EQ(log_.resumes, 0);
  player->unpark();
  EXPECT_EQ(log_.resumes, 1);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// Live, a commit hook that holds frame 1: frame 0 stays live while the
// worker goes on decoding each record that comes; released, frame 1 is
// drawn, then a newer one.
TEST_F(LivePlayerTest, AHeldFrameKeepsTheOneBeforeWhileRecordsKeepDecoding) {
  std::atomic<bool> release{false};
  hooks_.emplace();
  hooks_->commit = [&release](const grid::mesh::DeviceMesh&,
                              FrameStamp& stamp) {
    return core::Result<FrameCommit>(
        stamp.index == 1 && !release ? FrameCommit::kHold : FrameCommit::kDraw);
  };
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  StreamPlayer* player = start_live(server->port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  ASSERT_NO_FATAL_FAILURE(publish(*server, 0));
  const std::optional<FrameStamp> first = take_with(taker);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->index, 0u);
  const std::uint64_t held = exchange_.live().generation;
  ASSERT_NO_FATAL_FAILURE(publish(*server, 1));
  ASSERT_TRUE(eventually([&] {
    EXPECT_FALSE(take_with(taker, 1ms).has_value()) << "a frame while held";
    return taker.frames_held() == 1;
  }));
  for (std::uint64_t i = 2; i < kFrames; ++i) {
    ASSERT_NO_FATAL_FAILURE(publish(*server, i));
    ASSERT_TRUE(eventually([&] {
      EXPECT_FALSE(take_with(taker, 1ms).has_value()) << "a frame while held";
      return player->live_status().seq == i;
    })) << "record "
        << i << " not received while held";
  }
  EXPECT_EQ(exchange_.live().generation, held) << "the frame before stays";
  release = true;
  const std::optional<FrameStamp> second = take_with(taker);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->index, 1u);
  // The newest, after the records decoded meanwhile.
  std::optional<FrameStamp> newest;
  while (!newest || newest->index < kFrames - 1) {
    newest = take_with(taker);
    ASSERT_TRUE(newest.has_value());
    EXPECT_GT(newest->index, 1u);
  }
  EXPECT_GE(newest->records, 2u);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// Live, after_mesh's batch reads the grid its mesh was made from. When the
// sender goes and the next connection makes a grid of its own, the last grid
// goes only once that batch has finished, so the layer sees no buffer
// destroyed in use (the fixture fails on its errors). The mesh stays untaken
// until then, so no take reads the batch's value first.
TEST_F(LivePlayerTest, ANewConnectionsGridWaitsForAfterMeshsBatch) {
  std::mutex mutex;
  std::deque<float> reads;  // under mutex: each batch's readback destination
  hooks_.emplace();
  hooks_->after_mesh =
      [&](core::CommandBatch& batch, const grid::volume::VoxelBlockGrid& grid,
          const grid::mesh::DeviceMesh&, FrameStamp&) -> core::Status {
    VKC_ASSIGN(const grid::volume::AttributeView tsdf, grid.attribute("tsdf"));
    std::lock_guard<std::mutex> lock(mutex);
    return batch.readback(*tsdf.buffer, 0, sizeof(float),
                          &reads.emplace_back(0.0f));
  };
  std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  const std::uint16_t port = server->port();
  StreamPlayer* player = start_live(port);
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  ASSERT_NO_FATAL_FAILURE(publish(*server, 0));
  ASSERT_TRUE(eventually([&] {
    std::lock_guard<std::mutex> lock(mutex);
    return !reads.empty();
  }));
  server.reset();
  ASSERT_TRUE(eventually([&] {
    const LiveStatus status = player->live_status();
    return status.state == LiveState::kConnecting && !status.last_error.empty();
  }));
  server = serve(port);
  ASSERT_NE(server, nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  ASSERT_TRUE(take_with(taker).has_value());  // the last connection's
  ASSERT_NO_FATAL_FAILURE(publish(*server, 0));
  const std::optional<FrameStamp> frame = take_with(taker);
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->index, 0u);
  EXPECT_LT(stop_time(*player), kPrompt);  // before `reads` goes
  {
    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(reads.size(), 2u);
  }
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// Live, after_mesh records a compaction of the grid it is handed read-only,
// as the file tests' does. The grow stream's record kGrownFrame grows the
// grid at its decode's finish: the batch of the frame meshed before has
// finished first, so playback goes on, each batch counting its frame's blocks.
TEST_F(LivePlayerTest, AfterMeshMayReadTheGridARecordGrowsAtItsFinish) {
  live_through_compacting(vp_test::kGrowStream, /*grown_at_record=*/false);
}

// As above, the outgrow stream's record kGrownFrame growing the grid before
// its decode is recorded.
TEST_F(LivePlayerTest, AfterMeshMayReadTheGridARecordGrowsAtItsRecord) {
  live_through_compacting(vp_test::kOutgrowStream, /*grown_at_record=*/true);
}

// A program's need is asked once a record, and sets D: each frame's playout
// names it, and each is taken no earlier than its present time.
TEST_F(LivePlayerTest, AProgramsNeedIsAskedOnceARecordAndSetsTheDelay) {
  const auto asks = std::make_shared<LiveAsks>();
  asks->need_ns = std::chrono::nanoseconds(150ms).count();
  live_hooks(asks);
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  StreamPlayer* player = start_live(server->port());
  ASSERT_NE(player, nullptr);
  ASSERT_NE(player->live_clock(), nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  EXPECT_FALSE(player->live_clock()->time().has_value()) << "before a record";
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  for (std::uint64_t n = 0; n < kFrames; ++n) {
    ASSERT_NO_FATAL_FAILURE(publish_steady(*server, n));
    const std::optional<FrameStamp> frame = take_with(taker);
    ASSERT_TRUE(frame.has_value()) << "no frame " << n;
    EXPECT_EQ(frame->index, n);
    EXPECT_EQ(frame->records, 1u);
    expect_taken_when_due(*frame);
    EXPECT_EQ(frame->playout->need, DelayNeed::kProgram) << "frame " << n;
    EXPECT_EQ(frame->playout->target, 150ms);
    EXPECT_EQ(asks->needs, static_cast<int>(n + 1)) << "once a record";
  }
  EXPECT_EQ(taker.frames_held(), 0u) << "the gate's waits are no holds";
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// data_ready says never to 30 frames in a row: none is published, the frame
// shown stays, the clock's lead takes nothing of them, and the next frame,
// ready, is drawn, standing for every record since the last.
TEST_F(LivePlayerTest, DataReadysNeverPublishesNoneAndTheNextReadyIsDrawn) {
  constexpr std::uint64_t kNever = 30;
  const auto asks = std::make_shared<LiveAsks>();
  live_hooks(asks, [](const FrameStamp& stamp) {
    return stamp.index == 0 || stamp.index > kNever ? FrameCommit::kDraw
                                                    : FrameCommit::kSkip;
  });
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  StreamPlayer* player = start_live(server->port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  ASSERT_NO_FATAL_FAILURE(publish_steady(*server, 0));
  const std::optional<FrameStamp> first = take_with(taker);
  ASSERT_TRUE(first.has_value());
  const std::uint64_t shown = exchange_.live().generation;
  const std::atomic<bool> cancel{false};
  for (std::uint64_t n = 1; n <= kNever; ++n) {
    ASSERT_NO_FATAL_FAILURE(publish_steady(*server, n));
    ASSERT_TRUE(eventually([&] { return asks->count(n) == 1; }))
        << "frame " << n << " not asked";
    EXPECT_FALSE(take_with(taker, 1ms).has_value()) << "frame " << n;
    EXPECT_TRUE(exchange_.wait_collected(0ms, cancel)) << "frame " << n;
  }
  EXPECT_EQ(exchange_.live().generation, shown) << "the frame shown stays";
  const std::optional<Playout> refused = player->live_clock()->time();
  ASSERT_TRUE(refused.has_value());
  EXPECT_EQ(refused->lead, 0ns) << "a refusal is no publish";
  ASSERT_NO_FATAL_FAILURE(publish_steady(*server, kNever + 1));
  const std::optional<FrameStamp> next = take_with(taker);
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(next->index, kNever + 1);
  EXPECT_EQ(next->records, kNever + 1) << "released since frame 0";
  expect_taken_when_due(*next);
  for (std::uint64_t n = 1; n <= kNever + 1; ++n) {
    EXPECT_EQ(asks->count(n), 1u) << "frame " << n;
  }
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// data_ready says not yet to frame 1 for 200 ms: the frame before stays
// shown, records behind it come but none is released, then frame 1 is
// published once, and the records behind it are released after it.
TEST_F(LivePlayerTest, DataReadysNotYetHoldsTheFrameThenPublishesItOnce) {
  const auto asks = std::make_shared<LiveAsks>();
  const auto first_ask =
      std::make_shared<std::atomic<SteadyClock::rep>>(0);  // frame 1's
  live_hooks(asks, [first_ask](const FrameStamp& stamp) {
    if (stamp.index != 1) {
      return FrameCommit::kDraw;
    }
    const SteadyClock::rep now = SteadyClock::now().time_since_epoch().count();
    SteadyClock::rep expected = 0;
    first_ask->compare_exchange_strong(expected, now);
    return SteadyClock::duration(now - first_ask->load()) < 200ms
               ? FrameCommit::kHold
               : FrameCommit::kDraw;
  });
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  StreamPlayer* player = start_live(server->port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  ASSERT_NO_FATAL_FAILURE(publish_steady(*server, 0));
  ASSERT_TRUE(take_with(taker).has_value());
  const std::uint64_t shown = exchange_.live().generation;
  ASSERT_NO_FATAL_FAILURE(publish_steady(*server, 1));
  ASSERT_TRUE(eventually([&] { return asks->count(1) > 0; }));
  // Each once the one before came, or the server could skip to a newer key.
  for (std::uint64_t n = 2; n < kFrames; ++n) {
    ASSERT_NO_FATAL_FAILURE(publish_steady(*server, n));
    ASSERT_TRUE(eventually([&] { return player->live_status().seq == n; }));
  }
  std::vector<FrameStamp> frames;
  while (frames.empty() || frames.back().index < kFrames - 1) {
    const std::optional<FrameStamp> frame = take_with(taker);
    ASSERT_TRUE(frame.has_value()) << player->status().message();
    if (frames.empty()) {
      EXPECT_EQ(frame->index, 1u) << "a frame before the one held";
    }
    frames.push_back(*frame);
  }
  EXPECT_GE(asks->count(1), 2u) << "asked again while held";
  EXPECT_GE(frames[0].published,
            SteadyClock::time_point(SteadyClock::duration(first_ask->load())) +
                200ms);
  std::uint64_t records = 0;
  for (std::size_t i = 0; i < frames.size(); ++i) {
    records += frames[i].records;
    if (i > 0) {
      EXPECT_GT(frames[i].index, frames[i - 1].index);
      EXPECT_GE(frames[i].released, frames[0].published)
          << "frame " << frames[i].index << " released while 1 was held";
    }
  }
  EXPECT_EQ(records, kFrames - 1) << "records 1 on, each released once";
  EXPECT_NE(shown, exchange_.live().generation);
  // Records behind frame 1 were asked only once it was drawn.
  const std::vector<std::uint64_t> asked = asks->all();
  const auto first_behind =
      std::find_if(asked.begin(), asked.end(),
                   [](std::uint64_t index) { return index > 1; });
  EXPECT_TRUE(std::all_of(first_behind, asked.end(),
                          [](std::uint64_t index) { return index > 1; }));
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// A frame published, then a need raised to 600 ms as the next record comes,
// a period of pts after it: its take waits for its present time on the
// clock as raised, which the rise moved on, and the gate's wait is no hold.
TEST_F(LivePlayerTest, ANeedRaisedAfterAReleaseDelaysTheTakeByTheRise) {
  const auto asks = std::make_shared<LiveAsks>();
  live_hooks(asks);
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  StreamPlayer* player = start_live(server->port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  ASSERT_NO_FATAL_FAILURE(publish_steady(*server, 0));
  ASSERT_TRUE(take_with(taker).has_value());
  ASSERT_NO_FATAL_FAILURE(publish_steady(*server, 1));
  const std::atomic<bool> cancel{false};
  ASSERT_TRUE(eventually([&] {
    return !exchange_.wait_collected(0ms, cancel);
  })) << "frame 1 not published";
  const Playout before = player->live_clock()->time().value();
  const SteadyClock::time_point raised = SteadyClock::now();
  asks->need_ns = std::chrono::nanoseconds(600ms).count();
  ASSERT_NO_FATAL_FAILURE(publish_at(*server, 2, kLiveStepNs + kPeriodNs));
  ASSERT_TRUE(eventually(
      [&] { return player->live_clock()->time()->target == 600ms; }));
  const std::optional<FrameStamp> one = take_with(taker);
  ASSERT_TRUE(one.has_value());
  EXPECT_EQ(one->index, 1u);
  EXPECT_LT(one->published, raised) << "published before the rise";
  expect_taken_when_due(*one);
  EXPECT_EQ(one->playout->delay(), 600ms) << "risen at once";
  EXPECT_GT(one->playout->point, before.point);
  EXPECT_EQ(taker.frames_held(), 0u) << "the gate's wait is no hold";
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// A sender's steady stream at a program's need of 150 ms, then of none:
// at the need, every record is a frame of its own, in order; as D falls,
// the order is kept, and the need no longer sets it.
TEST_F(LivePlayerTest, AtANeedEachRecordIsAFrameInOrderAndAsDFallsOrderHolds) {
  constexpr std::uint64_t kAtNeed = 15;
  constexpr std::uint64_t kAll = 25;
  constexpr auto kPace = 100ms;  // the sender's, and its pts' step
  const auto asks = std::make_shared<LiveAsks>();
  asks->need_ns = std::chrono::nanoseconds(150ms).count();
  live_hooks(asks);
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  StreamPlayer* player = start_live(server->port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  std::atomic<bool> stop{false};
  std::thread sender([&] {
    SteadyClock::time_point due = SteadyClock::now();
    for (std::uint64_t n = 0; n < kAll && !stop; ++n) {
      stream::RecordInfo info = recording_.infos[n % kFrames];
      info.seq = n;
      info.pts_ns = n * std::chrono::nanoseconds(kPace).count();
      const Bytes& payload = recording_.payloads[n % kFrames];
      EXPECT_TRUE(server->publish(info, payload.data(), payload.size()).ok());
      due += kPace;
      std::this_thread::sleep_until(due);
    }
  });
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  std::vector<FrameStamp> frames;
  while (frames.empty() || frames.back().index < kAll - 1) {
    const std::optional<FrameStamp> frame = take_with(taker);
    if (!frame) {
      ADD_FAILURE() << "no frame after " << frames.size();
      break;
    }
    if (frame->index >= kAtNeed) {
      asks->need_ns = 0;
    }
    frames.push_back(*frame);
  }
  stop = true;
  sender.join();
  for (std::size_t i = 0; i < frames.size(); ++i) {
    SCOPED_TRACE(::testing::Message() << "frame " << frames[i].index);
    expect_taken_when_due(frames[i]);
    if (i > 0) {
      EXPECT_GT(frames[i].index, frames[i - 1].index) << "out of order";
    }
    if (frames[i].index <= kAtNeed) {
      EXPECT_EQ(frames[i].index, i) << "a record coalesced at the need";
      EXPECT_EQ(frames[i].records, 1u);
    }
  }
  EXPECT_NE(frames.back().playout->need, DelayNeed::kProgram)
      << "the program's need gone";
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// A need past a second is clamped: D is a second, its frames late by the
// excess, and they still come.
TEST_F(LivePlayerTest, ANeedPastASecondIsClampedAndFramesStillCome) {
  const auto asks = std::make_shared<LiveAsks>();
  asks->need_ns = std::chrono::nanoseconds(5s).count();
  live_hooks(asks);
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  StreamPlayer* player = start_live(server->port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  ASSERT_NO_FATAL_FAILURE(publish_steady(*server, 0));
  const std::optional<FrameStamp> frame = take_with(taker);
  ASSERT_TRUE(frame.has_value());
  expect_taken_when_due(*frame);
  EXPECT_TRUE(frame->playout->clamped);
  EXPECT_EQ(frame->playout->target, kMaxLiveDelay);
  EXPECT_EQ(frame->playout->delay(), kMaxLiveDelay);
  EXPECT_EQ(frame->playout->need, DelayNeed::kProgram);
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

// Records held as bytes at a park are dropped with the connection: the
// unpark joins at the newest key, each record of it released once.
TEST_F(LivePlayerTest, AParkDropsTheHeldRecordsAndTheUnparkJoinsAtTheKey) {
  const auto asks = std::make_shared<LiveAsks>();
  asks->need_ns = std::chrono::nanoseconds(kArrival).count();  // held long
  live_hooks(asks);
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  StreamPlayer* player = start_live(server->port());
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(reaches(LiveState::kConnected));
  // A period of pts apart, so each waits for the need.
  for (std::uint64_t n = 0; n < kIntraPeriod; ++n) {
    ASSERT_NO_FATAL_FAILURE(publish_steady(*server, n, kPeriodNs));
    ASSERT_TRUE(eventually([&] { return player->live_status().seq == n; }));
  }
  const std::atomic<bool> cancel{false};
  EXPECT_TRUE(exchange_.wait_collected(0ms, cancel)) << "one released";
  ASSERT_TRUE(player->park(kArrival));
  asks->need_ns = 0;
  player->unpark();
  FrameTaker taker(*player, exchange_, kFramesInFlight, &*hooks_);
  std::uint64_t records = 0;
  std::optional<FrameStamp> frame;
  while (!frame || frame->index < kIntraPeriod - 1) {
    frame = take_with(taker);
    ASSERT_TRUE(frame.has_value()) << player->status().message();
    records += frame->records;
  }
  EXPECT_EQ(records, kIntraPeriod) << "the new connection's alone";
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST(StreamPlayerConnect, RefusesNoHostOrPortAndHasNoTrack) {
  EXPECT_EQ(StreamPlayer::connect({"", 47801}).status().domain(),
            Code::InvalidArgument);
  EXPECT_EQ(StreamPlayer::connect({"lab", 0}).status().domain(),
            Code::InvalidArgument);
  // No I/O: nothing serves there, and nothing need.
  core::Result<std::unique_ptr<StreamPlayer>> player =
      StreamPlayer::connect({"lab.invalid", 1});
  ASSERT_TRUE(player.ok()) << player.status().message();
  EXPECT_TRUE((*player)->live());
  EXPECT_EQ((*player)->frame_count(), 0u);
  EXPECT_EQ((*player)->track_stats().records, 0u);
  EXPECT_EQ((*player)->rate().num, 0u);
  const LiveStatus status = (*player)->live_status();
  EXPECT_EQ(status.state, LiveState::kConnecting);
  EXPECT_EQ(status.address, "lab.invalid:1");
  EXPECT_TRUE(status.last_error.empty());
  // The playback controls do nothing.
  (*player)->pause();
  (*player)->set_loop(false);
  EXPECT_TRUE((*player)->playing());
  EXPECT_TRUE((*player)->loop());
}

TEST(StreamPlayerOpen, RefusesWhatItCannotPlay) {
  const Scratch scratch;
  const core::Result<std::unique_ptr<StreamPlayer>> missing =
      StreamPlayer::open(scratch.file("missing.vrst"));
  EXPECT_EQ(missing.status().domain(), Code::NotFound);

  const std::string text = scratch.file("notes.txt");
  std::ofstream(text) << "not a stream, but long enough to have been one if "
                         "it began with the magic\n";
  EXPECT_EQ(StreamPlayer::open(text).status().domain(), Code::InvalidArgument);

  // A track of another kind, with a record; and a geometry track with none.
  const auto write = [](const std::string& path, stream::TrackKind kind,
                        bool record) {
    stream::Session session;
    stream::Track track;
    track.kind = kind;
    session.tracks.push_back(track);
    core::Result<std::unique_ptr<stream::StreamWriter>> writer =
        stream::StreamWriter::create(path, session);
    ASSERT_TRUE(writer.ok()) << writer.status().message();
    const std::uint8_t byte = 0;
    stream::RecordInfo info;
    info.key = true;
    if (record) {
      ASSERT_TRUE((*writer)->append(info, &byte, 1).ok());
    }
    ASSERT_TRUE((*writer)->finish().ok());
  };
  const std::string other = scratch.file("other.vrst");
  ASSERT_NO_FATAL_FAILURE(
      write(other, static_cast<stream::TrackKind>(2), /*record=*/true));
  const core::Result<std::unique_ptr<StreamPlayer>> no_geometry =
      StreamPlayer::open(other);
  EXPECT_EQ(no_geometry.status().domain(), Code::NotFound);
  EXPECT_NE(no_geometry.status().message().find("no geometry"),
            std::string::npos);
  const std::string empty = scratch.file("empty.vrst");
  ASSERT_NO_FATAL_FAILURE(
      write(empty, stream::TrackKind::Geometry, /*record=*/false));
  EXPECT_EQ(StreamPlayer::open(empty).status().domain(), Code::NotFound);
}

}  // namespace
