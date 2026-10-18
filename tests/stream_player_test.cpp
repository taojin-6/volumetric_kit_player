// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// StreamPlayer on a device, over a stream the test writes with recon's
// MeshIntegrator, Encoder and StreamWriter: six frames of a sphere moving a
// block a frame, at 25 fps, an I-frame every third. Paused and stepped, so
// each frame goes when the test says: frames arrive in order with their
// index and pts, each mesh non-empty on the device; with loop on a step past
// the end wraps, with loop off the last frame holds until play restarts;
// a corrupt P-frame resyncs at the next key, and a stream with no key left
// holds even with loop on; quit is prompt, playing, paused or waiting for a
// consumer that takes nothing; a fatal error stops the worker. On the host:
// open refuses what it cannot play. The device tests skip with no device.

#include "volumetric_kit/player/stream_player.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/core/vulkan/buffer.hpp"
#include "volumetric_kit/core/vulkan/command_batch.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"
#include "volumetric_kit/recon/codec/encoder.hpp"
#include "volumetric_kit/recon/core/device_requirements.hpp"
#include "volumetric_kit/recon/core/math/vector_types.hpp"
#include "volumetric_kit/recon/stream/stream_writer.hpp"
#include "volumetric_kit/recon/tsdf/mesh_integrator.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace recon = volumetric_kit::recon;
namespace stream = recon::stream;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using volumetric_kit::player::FrameExchange;
using volumetric_kit::player::FrameStamp;
using volumetric_kit::player::StreamPlayer;
using Code = core::Status::Code;
using SteadyClock = std::chrono::steady_clock;

constexpr std::uint64_t kFrames = 6;
constexpr std::uint32_t kIntraPeriod = 3;
constexpr std::uint64_t kPeriodNs = 40'000'000;  // 25 fps
constexpr float kVoxel = 0.01f;
constexpr float kTrunc = 0.04f;
constexpr float kRadius = 0.12f;
// The default exchange's.
constexpr std::uint32_t kFramesInFlight = 2;
// How long a frame the test let go may take to arrive; how long one it did
// not is waited for; and the longest a prompt stop takes.
constexpr auto kArrival = 10s;
constexpr auto kQuiet = 300ms;
constexpr auto kPrompt = 1s;

// A directory of the test's own, removed with what it holds.
class Scratch {
 public:
  Scratch() {
    const ::testing::TestInfo* test =
        ::testing::UnitTest::GetInstance()->current_test_info();
    path_ = fs::temp_directory_path() / ("vp_" + std::string(test->name()) +
                                         "_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(path_, ec);
    fs::create_directories(path_);
  }
  ~Scratch() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;

  std::string file(const char* name) const { return (path_ / name).string(); }

 private:
  fs::path path_;
};

struct HostMesh {
  std::vector<recon::Vec3f> vertices;
  std::vector<std::uint32_t> indices;
};

// A UV sphere of kRadius round `centre`, wound outward, its edges about 1.6
// voxels long.
HostMesh sphere(const recon::Vec3f& centre) {
  constexpr std::uint32_t kStacks = 24;
  constexpr std::uint32_t kSlices = 48;
  constexpr float kPi = 3.14159265f;
  HostMesh mesh;
  for (std::uint32_t i = 0; i <= kStacks; ++i) {
    const float phi = kPi * static_cast<float>(i) / kStacks;
    for (std::uint32_t j = 0; j < kSlices; ++j) {
      const float theta = 2.0f * kPi * static_cast<float>(j) / kSlices;
      mesh.vertices.push_back(
          centre + kRadius * recon::Vec3f(std::sin(phi) * std::cos(theta),
                                          std::cos(phi),
                                          std::sin(phi) * std::sin(theta)));
    }
  }
  for (std::uint32_t i = 0; i < kStacks; ++i) {
    for (std::uint32_t j = 0; j < kSlices; ++j) {
      const std::uint32_t a = i * kSlices + j;
      const std::uint32_t b = i * kSlices + (j + 1) % kSlices;
      mesh.indices.insert(mesh.indices.end(),
                          {a, b, a + kSlices, b, b + kSlices, a + kSlices});
    }
  }
  return mesh;
}

// The stream: kFrames frames of the sphere moving a block (8 voxels) a frame
// along x, kPeriodNs apart, an I-frame every kIntraPeriod, each record a key
// exactly when its frame is an I-frame.
core::Status write_stream(core::Device& device, core::Allocator& allocator,
                          const std::string& path) {
  namespace vol = recon::volume;
  constexpr std::int32_t kBuckets = 1024;
  const vol::VoxelGridParams params{kVoxel,
                                    8,
                                    512,
                                    kTrunc,
                                    vol::kDefaultBucketSize,
                                    kBuckets,
                                    vol::kDefaultBucketSize * kBuckets,
                                    vol::kDefaultMaxChain};
  const vol::AttributeSpec attrs[] = {{"tsdf", sizeof(float)},
                                      {"weight", sizeof(float)}};
  VKC_ASSIGN(vol::VoxelBlockGrid grid,
             vol::VoxelBlockGrid::create(device, allocator, params, attrs, 2));
  VKC_ASSIGN(recon::tsdf::MeshIntegrator integrator,
             recon::tsdf::MeshIntegrator::create(device, allocator));
  recon::codec::EncoderConfig config;
  config.intra_period = kIntraPeriod;
  VKC_ASSIGN(recon::codec::Encoder encoder,
             recon::codec::Encoder::create(device, allocator, config));
  stream::Session session;
  session.writer = "stream_player_test";
  stream::Track track;
  track.kind = stream::TrackKind::Geometry;
  track.rate = {25, 1};
  session.tracks.push_back(track);
  VKC_ASSIGN(std::unique_ptr<stream::StreamWriter> writer,
             stream::StreamWriter::create(path, session));
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    const HostMesh mesh =
        sphere(recon::Vec3f(8.0f * kVoxel * static_cast<float>(i), 0, 0));
    const auto vertex_count = static_cast<std::uint32_t>(mesh.vertices.size());
    const auto triangle_count =
        static_cast<std::uint32_t>(mesh.indices.size() / 3);
    VKC_TRY(grid.clear());
    VKC_ASSIGN(const std::uint32_t failed,
               grid.map().allocate_from_triangles(
                   mesh.vertices.data(), vertex_count, mesh.indices.data(),
                   triangle_count));
    if (failed != 0) {
      return core::Status::out_of_memory("the sphere outgrew the grid");
    }
    VKC_TRY(integrator
                .integrate(grid, mesh.vertices.data(), vertex_count,
                           mesh.indices.data(), triangle_count)
                .status());
    VKC_ASSIGN(const std::vector<std::uint8_t> frame, encoder.encode(grid));
    VKC_ASSIGN(const recon::codec::FrameInfo info,
               recon::codec::read_frame_info(frame.data(), frame.size()));
    stream::RecordInfo record;
    record.key = info.type == recon::codec::FrameType::kIntra;
    record.pts_ns = i * kPeriodNs;
    record.seq = i;
    VKC_TRY(writer->append(record, frame.data(), frame.size()));
  }
  return writer->finish();
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

  // The stream opened and started on the test's device, paused unless
  // `playing`, extracted for a renderer of kFramesInFlight frames into
  // `slot_count` ring slots.
  std::unique_ptr<StreamPlayer> start(
      bool playing, std::uint32_t slot_count = kFramesInFlight + 1) {
    core::Result<std::unique_ptr<StreamPlayer>> opened =
        StreamPlayer::open(path_);
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    if (!opened.ok()) {
      return nullptr;
    }
    std::unique_ptr<StreamPlayer> player = std::move(opened).value();
    if (!playing) {
      player->pause();
    }
    recon::mesh::MarchingCubesConfig config;
    config.extra_vertex_usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    config.extra_index_usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    config.slot_count = slot_count;
    const core::Status started =
        player->start(device(), allocator(), config, exchange_);
    EXPECT_TRUE(started.ok()) << started.message();
    return started.ok() ? std::move(player) : nullptr;
  }

  // The next mesh the worker publishes, taken as a renderer's frame takes
  // it, and checked non-empty on the device: its stamp, or empty when none
  // comes within `wait`.
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
        // An empty mesh commits without the callback.
        const recon::mesh::DeviceMesh& mesh = exchange_.live();
        EXPECT_GT(mesh.triangle_count, 0u);
        EXPECT_EQ(index_count(mesh),
                  recon::mesh::kIndicesPerTriangle * mesh.triangle_count);
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

  // Flips a byte in `record`'s payload on disk, which then fails its CRC.
  void corrupt(const stream::Record& record) {
    constexpr std::uint64_t kRecordHeader = 56;  // session.hpp's RECD
    const auto at = static_cast<std::streamoff>(record.offset + kRecordHeader +
                                                record.size / 2);
    std::fstream file(path_, std::ios::in | std::ios::out | std::ios::binary);
    file.seekg(at);
    const int byte = file.get();
    file.seekp(at);
    file.put(static_cast<char>(byte ^ 0x5A));
    file.close();
    ASSERT_TRUE(file.good());
    core::Result<std::unique_ptr<stream::StreamReader>> reader =
        stream::StreamReader::open(path_);
    ASSERT_TRUE(reader.ok()) << reader.status().message();
    std::vector<std::uint8_t> payload;
    EXPECT_EQ((*reader)->read(record, &payload).domain(),
              Code::InvalidArgument);
  }

  // Steps once and checks the frame that comes is frame `index`.
  void step_to(StreamPlayer& player, std::uint64_t index) {
    player.step();
    const std::optional<FrameStamp> frame = next_frame();
    ASSERT_TRUE(frame.has_value()) << "no frame " << index;
    EXPECT_EQ(frame->index, index);
    EXPECT_EQ(frame->pts_ns, index * kPeriodNs);
  }

  Scratch scratch_;
  const std::string path_ = scratch_.file("sphere.vrst");
  FrameExchange exchange_;
  std::uint32_t slot_ = 0;
};

// How long `player.stop()` takes. One that is not back within kPrompt fails
// the test; a control then wakes the worker, so the test still ends.
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
    player.play();
  }
  stopper.join();
  return SteadyClock::now() - start;
}

TEST_F(StreamPlayerTest, PlaysEachFrameInOrderWhenStepped) {
  std::unique_ptr<StreamPlayer> player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  EXPECT_EQ(player->frame_count(), kFrames);
  EXPECT_FALSE(next_frame(kQuiet).has_value()) << "paused, nothing goes";
  for (std::uint64_t i = 0; i < kFrames; ++i) {
    player->step();
    const std::optional<FrameStamp> frame = next_frame();
    ASSERT_TRUE(frame.has_value()) << "no frame " << i;
    EXPECT_EQ(frame->index, i);
    EXPECT_EQ(frame->pts_ns, i * kPeriodNs);
    EXPECT_GT(frame->decode_mesh_ms, 0.0);
  }
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, WithLoopOnAStepPastTheEndWraps) {
  std::unique_ptr<StreamPlayer> player = start(/*playing=*/false);
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
  std::unique_ptr<StreamPlayer> player = start(/*playing=*/false);
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

TEST_F(StreamPlayerTest, PlayingIntoTheEndWithLoopOffPausesOnTheLastFrame) {
  std::unique_ptr<StreamPlayer> player = start(/*playing=*/true);
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
  ASSERT_NO_FATAL_FAILURE(corrupt(all[1]));
  std::unique_ptr<StreamPlayer> player = start(/*playing=*/false);
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

TEST_F(StreamPlayerTest, WithNothingToShowItHoldsEvenWithLoopOn) {
  // Every key corrupt: no frame decodes.
  const std::vector<stream::Record> all = records();
  ASSERT_EQ(all.size(), kFrames);
  for (const stream::Record& record : all) {
    if (record.info.key) {
      ASSERT_NO_FATAL_FAILURE(corrupt(record));
    }
  }
  std::unique_ptr<StreamPlayer> player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  EXPECT_TRUE(player->loop());
  const SteadyClock::time_point deadline = SteadyClock::now() + kArrival;
  while (player->playing() && SteadyClock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_FALSE(player->playing()) << "held, rather than reading on";
  EXPECT_FALSE(next_frame(kQuiet).has_value());
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, QuitIsPromptWhilePlaying) {
  std::unique_ptr<StreamPlayer> player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(next_frame().has_value());
  ASSERT_TRUE(next_frame().has_value());
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, QuitIsPromptWhilePaused) {
  std::unique_ptr<StreamPlayer> player = start(/*playing=*/false);
  ASSERT_NE(player, nullptr);
  ASSERT_NO_FATAL_FAILURE(step_to(*player, 0));
  std::this_thread::sleep_for(kQuiet);  // the worker waits for a step
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, AConsumerTakingNothingStallsTheWorkerUntilQuit) {
  std::unique_ptr<StreamPlayer> player = start(/*playing=*/true);
  ASSERT_NE(player, nullptr);
  ASSERT_TRUE(next_frame().has_value());
  // Frame 1 is published and never taken, so the worker waits to mesh frame
  // 2. Publishing over frame 1 instead would run out of ring slots within
  // a few frames, which is an error.
  std::this_thread::sleep_for(kQuiet);
  EXPECT_LT(stop_time(*player), kPrompt);
  EXPECT_TRUE(player->status().ok()) << player->status().message();
}

TEST_F(StreamPlayerTest, AFatalErrorStopsTheWorker) {
  // A ring with no slot to spare: with both of the consumer's frames holding
  // a mesh, the third extract is refused.
  std::unique_ptr<StreamPlayer> player =
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
