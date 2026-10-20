// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// StreamPlayer on a device, over the tests' stream (test_stream.hpp). Paused
// and stepped, so each frame goes when the test says: frames arrive in order
// with their index and pts, each mesh non-empty on the device, and a frame
// that meshes nothing still reaches the commit with its stamp; with loop on a
// step past the end wraps, with loop off the last frame holds until play
// restarts; a corrupt P-frame, a damaged record header or a record this
// version cannot read resyncs at the next key, and a stream with no key left
// stops the worker with the reason; quit is prompt, playing, paused or
// waiting for a consumer that takes nothing; a fatal error stops the worker,
// a failed build on it included; start is refused once started or stopped.
// Playing, no frame arrives before its pts says. A park holds the worker:
// nothing is published while parked, an extract waiting for the consumer
// included; a control applied meanwhile (a step, a play and a pause) takes
// effect at unpark; playing() does not change; unpark carries on from the
// same frame; a park returns as soon as the step under way ends; one that
// times out mid-decode still lets nothing start after it; one before start
// holds the build; one before start or after stop returns at once; and a stop
// while parked is prompt, at a step's start too. On the host: open refuses
// what it cannot play. The device tests skip with no device. A stop that no
// control can wake, such as one lost in the exchange's wait, fails only on
// ctest's timeout.

#include "volumetric_kit/player/stream_player.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
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
#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"
#include "volumetric_kit/recon/core/device_requirements.hpp"
#include "volumetric_kit/recon/stream/stream_writer.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace recon = volumetric_kit::recon;
namespace stream = recon::stream;
using namespace std::chrono_literals;
using volumetric_kit::player::FrameExchange;
using volumetric_kit::player::FrameStamp;
using volumetric_kit::player::StreamPlayer;
using vp_test::kFrames;
using vp_test::kIntraPeriod;
using vp_test::pts_of;
using vp_test::Scratch;
using vp_test::write_stream;
using Code = core::Status::Code;
using SteadyClock = std::chrono::steady_clock;

// The default exchange's.
constexpr std::uint32_t kFramesInFlight = 2;
// How long a frame the test let go may take to arrive; how long one it did
// not is waited for; and the longest a prompt stop or park takes.
constexpr auto kArrival = 10s;
constexpr auto kQuiet = 300ms;
constexpr auto kPrompt = 1s;
// A RECD chunk's header (session.hpp): its flags, its pts, and its CRC of the
// bytes before it.
constexpr std::size_t kRecordHeader = 56;
constexpr std::size_t kFlagsAt = 22;
constexpr std::size_t kPtsAt = 24;
constexpr std::size_t kHeaderCrcAt = 52;

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
// the device's submit mutex, and every step submits (the build, a decode, an
// extract). A park that finds the worker between steps holds it there, so
// this unparks and lets it go on until a park finds it busy. Whether one did
// within kArrival; the hold then stays in force.
bool park_mid_step(StreamPlayer& player) {
  const SteadyClock::time_point deadline = SteadyClock::now() + kArrival;
  while (player.park(0ms)) {
    player.unpark();
    if (SteadyClock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

class StreamPlayerTest : public core::test::VulkanDeviceTest {
 protected:
  core::DeviceRequirements requirements() const override {
    return recon::device_requirements();
  }

  void SetUp() override {
    VulkanDeviceTest::SetUp();
    if (base_setup_incomplete()) {
      return;
    }
    const core::Status written = write_stream(device(), allocator(), path_);
    ASSERT_TRUE(written.ok()) << written.message();
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
  // `slot_count` ring slots.
  static recon::mesh::MarchingCubesConfig extractor_config(
      std::uint32_t slot_count = kFramesInFlight + 1) {
    recon::mesh::MarchingCubesConfig config;
    config.extra_vertex_usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    config.extra_index_usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    config.slot_count = slot_count;
    return config;
  }

  // The stream opened and started on the test's device, paused unless
  // `playing`, into `slot_count` ring slots; the fixture owns it.
  StreamPlayer* start(bool playing,
                      std::uint32_t slot_count = kFramesInFlight + 1) {
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
    const core::Status started = player_->start(
        device(), allocator(), extractor_config(slot_count), exchange_);
    EXPECT_TRUE(started.ok()) << started.message();
    return started.ok() ? player_.get() : nullptr;
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
      const recon::mesh::ExchangeOutcome outcome = exchange_.begin_frame(
          slot_, [&](const recon::mesh::DeviceMesh&, FrameStamp& stamp) {
            taken = stamp;
            return true;
          });
      slot_ = (slot_ + 1) % kFramesInFlight;
      if (outcome == recon::mesh::ExchangeOutcome::kRefused) {
        ADD_FAILURE() << "refused: " << exchange_.refused();
        return std::nullopt;
      }
      if (outcome == recon::mesh::ExchangeOutcome::kCommitted) {
        // Every mesh committed, an empty one included, came through the
        // commit with its stamp.
        EXPECT_TRUE(taken.has_value()) << "a mesh committed without its stamp";
        const recon::mesh::DeviceMesh& mesh = exchange_.live();
        if (taken) {
          EXPECT_EQ(mesh.empty(), taken->index == empty_frame_)
              << "frame " << taken->index;
        }
        // An empty mesh may name null buffers: no draw command to read.
        if (mesh.valid()) {
          EXPECT_EQ(index_count(mesh),
                    recon::mesh::kIndicesPerTriangle * mesh.triangle_count);
        }
        return taken;
      }
      std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
  }

  // The index count the mesh's indirect draw command holds on the device.
  std::uint32_t index_count(const recon::mesh::DeviceMesh& mesh) {
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

  // The stream's records, in file order.
  std::vector<stream::Record> records() {
    std::vector<stream::Record> all;
    core::Result<std::unique_ptr<stream::StreamReader>> reader =
        stream::StreamReader::open(path_);
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
    const std::uint32_t crc = crc32(header.data(), kHeaderCrcAt);
    for (std::size_t i = 0; i < 4; ++i) {
      header[kHeaderCrcAt + i] = static_cast<std::uint8_t>(crc >> (8 * i));
    }
    ASSERT_NO_FATAL_FAILURE(write_at(record.offset, header));
    EXPECT_EQ(reread(record).domain(), Code::Unsupported);
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
  std::string path_ = scratch_.file("sphere.vrst");
  // The stream's frame that meshes nothing (write_stream's `empty`), if any.
  std::optional<std::uint64_t> empty_frame_;
  FrameExchange exchange_;
  std::uint32_t slot_ = 0;
  // Destroyed first: its worker uses the exchange.
  std::unique_ptr<StreamPlayer> player_;
};

TEST_F(StreamPlayerTest, PlaysEachFrameInOrderWhenStepped) {
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  EXPECT_EQ(player->frame_count(), kFrames);
  EXPECT_FALSE(next_frame(kQuiet).has_value()) << "paused, nothing goes";
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    player->step();
    const std::optional<FrameStamp> frame = next_frame();
    ASSERT_TRUE(frame.has_value()) << "no frame " << i;
    EXPECT_EQ(frame->index, i);
    EXPECT_EQ(frame->pts_ns, pts_of(i));
    EXPECT_GT(frame->decode_mesh_ms, 0.0);
  }
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, AFrameThatMeshesNothingReachesTheCommitWithItsStamp) {
  path_ = scratch_.file("empty_frame.vrst");
  empty_frame_ = 1;
  const core::Status written =
      write_stream(device(), allocator(), path_, empty_frame_);
  ASSERT_TRUE(written.ok()) << written.message();
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

TEST_F(StreamPlayerTest, AFatalErrorStopsTheWorker) {
  // A ring with no slot to spare: with both of the consumer's frames holding
  // a mesh, the third extract is refused.
  StreamPlayer* player =
      start(/*playing=*/false, /*slot_count=*/kFramesInFlight);
  ASSERT_NE(player, nullptr);
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 0));
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 1));
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
  // Mid-playback: the worker decodes frame 2 once frame 1 goes.
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

TEST_F(StreamPlayerTest, AParkHoldsAnExtractWaitingForTheConsumer) {
  StreamPlayer* player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 0));
  // Frame 1 goes and is left untaken, so frame 2, stepped too, waits for the
  // consumer before its extract; the sleep only lets the worker get there.
  player->step();
  player->step();
  std::this_thread::sleep_for(kQuiet);
  EXPECT_TRUE(player->park(kArrival));
  // Taking frame 1, published before the park, frees the worker's wait; it
  // does not extract while parked.
  std::uint64_t last = 0;
  if (const std::optional<FrameStamp> pending = next_frame(kQuiet)) {
    last = pending->index;
  }
  EXPECT_FALSE(next_frame(kQuiet).has_value()) << "extracted while parked";
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
