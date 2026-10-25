// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The measure log on the host, format 2: its header names the build, the
// device, the rings, the stream, how it ran, the timing, the tile depth, the
// labels and the time, and on a phone its thermal state and Low Power Mode,
// then the columns; each frame is its worker rows, host as busy time, GPU
// only where measured and each device state by name, a breakdown's name kept
// and a name with a comma quoted, then the render rows of the display frame
// that took it, then its frame row: its device counts, dispatches and
// barriers too, what the deblock did, its rounds, its queue idle and latency
// and its latency legs, each empty where unknown; a rebuild writes nothing
// and is no frame; nothing is at the path until finish renames the part
// file, and after it nothing more is written; a folder that is not there is
// refused. The time reads as UTC, and a log's file name, or a failed
// launch's, is it and the player's commit. The iOS app's measure arguments
// parse with the system's left alone, the tile depth and the labels too, and
// what is wrong is named; the timings', depths' and switches' names
// round-trip. On a device: its line names it, its driver, the core's memory
// facts, its timestamps and its clocks' calibration, each as the device says,
// and its rings run in place exactly where its device-mapped memory is
// cached.

#include "volumetric_kit/player/measure_log.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "test_stream.hpp"
#include "volumetric_kit/core/base/stage_metrics.hpp"
#include "volumetric_kit/core/testing/vulkan_fixture.hpp"
#include "volumetric_kit/core/vulkan/host_ring.hpp"
#include "volumetric_kit/player/command_line.hpp"
#include "volumetric_kit/player/stream_player.hpp"
#include "volumetric_kit/recon/core/device_requirements.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace vkp = volumetric_kit::player;
using vkp::DeviceTiming;
using vkp::FrameStamp;
using vkp::MeasureArgs;
using vkp::MeasureContext;
using vkp::MeasureLog;
using Lines = std::vector<std::string>;

// 2026-10-23T05:12:00Z.
const std::chrono::system_clock::time_point kStarted{
    std::chrono::seconds(1'792'732'320)};

MeasureContext context() {
  MeasureContext context;
  context.device = "Test GPU; vendor 0x1234";
  context.rings = "copied";
  context.stream = "fixture.vrst, track 0";
  context.run = "vp_probe --worker --passes 2";
  context.timing = DeviceTiming::kSampled;
  context.tile_depth = 1;
  context.debug_labels = false;
  context.started = kStarted;
  return context;
}

// The header's lines before its columns, in order.
constexpr std::size_t kHeaderLines = 15;

Lines read_lines(const std::string& path) {
  std::ifstream file(path);
  Lines lines;
  for (std::string line; std::getline(file, line);) {
    lines.push_back(line);
  }
  return lines;
}

// A P-frame of pass 2, index 7, whose decode waited and was measured, with a
// breakdown that ran untimed and a mesh the queue could not time.
FrameStamp stamp() {
  FrameStamp stamp;
  stamp.index = 7;
  stamp.pass = 2;
  stamp.bytes = 600;
  stamp.key = false;
  stamp.timed = true;
  stamp.stages.add_cpu("read", 0.25);
  stamp.stages.add_cpu("decode", 3.0);
  stamp.stages.add_wait("decode", 1.0);
  stamp.stages.add_gpu("decode", 0.5);
  stamp.stages.add_cpu("  ..rans decode", 1.0);
  stamp.stages.add_device("  ..rans decode", core::DeviceTime::NotTimed);
  stamp.stages.add_cpu("mesh", 2.0);
  stamp.stages.add_wait("mesh", 2.5);  // past its host time: busy reads 0
  stamp.stages.add_device("mesh", core::DeviceTime::Unavailable);
  stamp.counters.submits = 1;
  stamp.counters.host_waits = 0;
  stamp.counters.blocked_ns = 0;
  stamp.counters.staging_buffers = 0;
  stamp.counters.dispatches = 39;
  stamp.counters.barriers = 48;
  stamp.deblock_on = true;
  stamp.deblock.filter = "LG";
  stamp.deblock.exact_blocks = 1'904;
  stamp.rounds = 2;
  stamp.device_idle_ms = 26.91;
  stamp.queue_ms = 0.41;
  const std::chrono::steady_clock::time_point due{std::chrono::seconds(100)};
  stamp.due = due;
  stamp.published = due + std::chrono::microseconds(200);
  stamp.taken = due + std::chrono::microseconds(8'700);
  stamp.presented = due + std::chrono::microseconds(9'800);
  return stamp;
}

// The render rows of the display frame that took it: its draw, timed, and
// its loop's present, a wait.
std::vector<core::StageRow> render() {
  core::StageMetrics rows;
  rows.add_cpu("draw (mesh+UI)", 0.91);
  rows.add_gpu("draw (mesh+UI)", 1.0441);
  rows.add_cpu("present", 1.25);
  rows.add_wait("present", 1.0);
  return rows.rows();
}

TEST(MeasureLog, TheHeaderNamesWhatWasMeasuredThenTheColumns) {
  vp_test::Scratch scratch;
  const std::string path = scratch.file("run.csv");
  core::Result<std::unique_ptr<MeasureLog>> log =
      MeasureLog::open(path, context());
  ASSERT_TRUE(log.ok()) << log.status().message();
  ASSERT_TRUE((*log)->finish().ok());
  const vkp::BuildInfo& build = vkp::build_info();
  EXPECT_EQ(
      read_lines(path),
      (Lines{"# format: volumetric_kit_player measure 2",
             std::string("# player: ") + build.player,
             std::string("# core: ") + build.core,
             std::string("# recon: ") + build.recon,
             std::string("# gfx: ") + build.gfx,
             std::string("# build: ") + build.build_type,
             "# device: Test GPU; vendor 0x1234", "# rings: copied",
             "# stream: fixture.vrst, track 0",
             "# run: vp_probe --worker --passes 2", "# timing: sampled",
             "# tile_depth: 1", "# labels: off", "# utc: 2026-10-23T05:12:00Z",
             "pass,frame,type,bytes,triangles,table,stage,host_ms,"
             "wait_ms,gpu_ms,device,submits,host_waits,blocked_ns,"
             "staging_buffers,dispatches,barriers,deblock,exact_blocks,"
             "rounds,idle_ms,queue_ms,publish_ms,take_ms,present_ms"}));
  EXPECT_EQ((*log)->frames(), 0u);
}

// A phone's measure launch states its thermal state and Low Power Mode as it
// started, after the time.
TEST(MeasureLog, APhonesHeaderStatesItsThermalStateAndLowPowerMode) {
  vp_test::Scratch scratch;
  const std::string path = scratch.file("run.csv");
  MeasureContext phone = context();
  phone.thermal = "fair";
  phone.low_power = false;
  core::Result<std::unique_ptr<MeasureLog>> log = MeasureLog::open(path, phone);
  ASSERT_TRUE(log.ok()) << log.status().message();
  ASSERT_TRUE((*log)->finish().ok());
  const Lines lines = read_lines(path);
  ASSERT_EQ(lines.size(), kHeaderLines + 2);
  EXPECT_EQ(lines[13], "# utc: 2026-10-23T05:12:00Z");
  EXPECT_EQ(lines[14], "# thermal: fair");
  EXPECT_EQ(lines[15], "# low_power: off");
  EXPECT_EQ(lines[16].rfind("pass,frame,", 0), 0u);
}

TEST(MeasureLog, AFrameIsItsWorkerRowsItsRenderRowsThenItsFigures) {
  vp_test::Scratch scratch;
  const std::string path = scratch.file("run.csv");
  core::Result<std::unique_ptr<MeasureLog>> log =
      MeasureLog::open(path, context());
  ASSERT_TRUE(log.ok()) << log.status().message();
  ASSERT_TRUE((*log)->add(stamp(), 510, render()).ok());
  FrameStamp key;
  key.key = true;
  key.stages.add_cpu("a, b", 1.0);
  ASSERT_TRUE((*log)->add(key, 0).ok());
  EXPECT_EQ((*log)->frames(), 2u);
  ASSERT_TRUE((*log)->finish().ok());
  const Lines lines = read_lines(path);
  ASSERT_EQ(lines.size(), kHeaderLines + 7u + 2u);
  constexpr const char* kNone = ",,,,,,,,,,,,,,";  // the frame row's columns
  EXPECT_EQ(
      Lines(lines.begin() + kHeaderLines, lines.end()),
      (Lines{
          std::string("2,7,P,600,510,worker,read,0.2500,0.0000,,none") + kNone,
          std::string("2,7,P,600,510,worker,decode,2.0000,1.0000,0.5000,"
                      "measured") +
              kNone,
          std::string("2,7,P,600,510,worker,  ..rans decode,1.0000,0.0000,,"
                      "not_timed") +
              kNone,
          std::string("2,7,P,600,510,worker,mesh,0.0000,2.5000,,unavailable") +
              kNone,
          std::string("2,7,P,600,510,render,draw (mesh+UI),0.9100,0.0000,"
                      "1.0441,measured") +
              kNone,
          std::string("2,7,P,600,510,render,present,0.2500,1.0000,,none") +
              kNone,
          "2,7,P,600,510,,,,,,,1,0,0,0,39,48,LG,1904,2,26.9100,0.4100,0.2000,"
          "8.5000,1.1000",
          std::string("1,0,I,0,0,worker,\"a, b\",1.0000,0.0000,,none") + kNone,
          "1,0,I,0,0,,,,,,,0,0,0,0,0,0,off,0,0,,,,,"}));
}

// A paused deblock toggle's rebuild is the frame before again.
TEST(MeasureLog, ARebuildIsNoFrame) {
  vp_test::Scratch scratch;
  const std::string path = scratch.file("run.csv");
  core::Result<std::unique_ptr<MeasureLog>> log =
      MeasureLog::open(path, context());
  ASSERT_TRUE(log.ok()) << log.status().message();
  ASSERT_TRUE((*log)->add(stamp(), 510).ok());
  FrameStamp rebuild = stamp();
  rebuild.rebuild = true;
  ASSERT_TRUE((*log)->add(rebuild, 510).ok());
  EXPECT_EQ((*log)->frames(), 1u);
  ASSERT_TRUE((*log)->finish().ok());
  EXPECT_EQ(read_lines(path).size(), kHeaderLines + 5u);
}

TEST(MeasureLog, NothingIsAtThePathUntilFinishAndNothingAfter) {
  vp_test::Scratch scratch;
  const std::string path = scratch.file("run.csv");
  core::Result<std::unique_ptr<MeasureLog>> log =
      MeasureLog::open(path, context());
  ASSERT_TRUE(log.ok()) << log.status().message();
  EXPECT_EQ((*log)->path(), path);
  ASSERT_TRUE((*log)->add(stamp(), 1).ok());
  EXPECT_FALSE(std::filesystem::exists(path));
  EXPECT_TRUE(std::filesystem::exists(path + ".part"));
  ASSERT_TRUE((*log)->finish().ok());
  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_FALSE(std::filesystem::exists(path + ".part"));
  const std::uintmax_t size = std::filesystem::file_size(path);
  EXPECT_EQ((*log)->add(stamp(), 1).domain(), core::Status::Code::IoError);
  EXPECT_EQ((*log)->finish().domain(), core::Status::Code::IoError);
  EXPECT_EQ(std::filesystem::file_size(path), size);
}

TEST(MeasureLog, AFolderThatIsNotThereIsRefused) {
  vp_test::Scratch scratch;
  const std::string path = scratch.file("missing/run.csv");
  const core::Result<std::unique_ptr<MeasureLog>> log =
      MeasureLog::open(path, context());
  ASSERT_FALSE(log.ok());
  EXPECT_EQ(log.status().domain(), core::Status::Code::IoError);
  EXPECT_NE(log.status().message().find(path), std::string::npos)
      << log.status().message();
}

TEST(MeasureLog, TheTimeReadsAsUtcAndAFileIsNamedByItAndTheCommit) {
  EXPECT_EQ(vkp::utc_text(kStarted), "2026-10-23T05:12:00Z");
  EXPECT_EQ(vkp::utc_text(kStarted, /*compact=*/true), "20261023T051200Z");
  // The commit's first 7 characters, and "-dirty" if it says so.
  const std::string commit = vkp::build_info().player;
  const std::string name = vkp::measure_file_name(kStarted);
  EXPECT_EQ(name.rfind("20261023T051200Z-" + commit.substr(0, 7), 0), 0u)
      << name;
  EXPECT_EQ(name.find("-dirty") != std::string::npos,
            commit.find("-dirty") != std::string::npos)
      << name;
  EXPECT_EQ(name.substr(name.size() - 4), ".csv");
  const std::string failed = vkp::measure_file_name(kStarted, ".failed");
  EXPECT_EQ(failed, name.substr(0, name.size() - 4) + ".failed");
  EXPECT_STRNE(vkp::build_info().build_type, "");
}

TEST(MeasureArgs, NoneWithoutMeasureAndTheSystemsAreLeftAlone) {
  const core::Result<std::optional<MeasureArgs>> none =
      vkp::parse_measure_args({"-NSDocumentRevisionsDebugMode", "YES"});
  ASSERT_TRUE(none.ok()) << none.status().message();
  EXPECT_FALSE(none.value().has_value());
  const core::Result<std::optional<MeasureArgs>> plain =
      vkp::parse_measure_args({"--measure", "rafa2.vrst"});
  ASSERT_TRUE(plain.ok() && plain.value().has_value());
  EXPECT_EQ(plain.value()->stream, "rafa2.vrst");
  EXPECT_EQ(plain.value()->passes, 1u) << "one pass";
  EXPECT_FALSE(plain.value()->frames.has_value()) << "all of it";
  EXPECT_EQ(plain.value()->timing, DeviceTiming::kAll);
  EXPECT_EQ(plain.value()->tile_depth, 0u) << "every scope";
  EXPECT_TRUE(plain.value()->debug_labels);
  const core::Result<std::optional<MeasureArgs>> full = vkp::parse_measure_args(
      {"--timing", "sampled", "-AppleLanguages", "(en)", "--frames", "300",
       "--labels", "off", "--measure", "rafa2.vrst", "--passes", "2",
       "--tile-depth", "1"});
  ASSERT_TRUE(full.ok() && full.value().has_value()) << full.status().message();
  EXPECT_EQ(full.value()->stream, "rafa2.vrst");
  EXPECT_EQ(full.value()->passes, 2u);
  EXPECT_EQ(full.value()->frames, 300u);
  EXPECT_EQ(full.value()->timing, DeviceTiming::kSampled);
  EXPECT_EQ(full.value()->tile_depth, 1u);
  EXPECT_FALSE(full.value()->debug_labels);
}

TEST(MeasureArgs, WhatIsWrongIsNamed) {
  for (const auto& [args, named] :
       std::vector<std::pair<std::vector<std::string>, std::string>>{
           {{"--measure"}, "--measure needs a value"},
           {{"--measure", "a/b.vrst"}, "--measure 'a/b.vrst'"},
           {{"--measure", ""}, "--measure ''"},
           {{"--measure", "a.vrst", "--frames", "0"}, "--frames '0'"},
           {{"--measure", "a.vrst", "--passes", "x"}, "--passes 'x'"},
           {{"--measure", "a.vrst", "--timing", "some"}, "--timing 'some'"},
           {{"--measure", "a.vrst", "--tile-depth", "2"}, "--tile-depth '2'"},
           {{"--measure", "a.vrst", "--labels", "yes"}, "--labels 'yes'"},
           {{"--measure", "a.vrst", "--labels"}, "--labels needs a value"},
           {{"--frames", "10"}, "need --measure"},
           {{"--passes", "2"}, "need --measure"},
           {{"--tile-depth", "1"}, "need --measure"},
           {{"--labels", "off"}, "need --measure"}}) {
    const core::Result<std::optional<MeasureArgs>> parsed =
        vkp::parse_measure_args(args);
    ASSERT_FALSE(parsed.ok()) << named;
    EXPECT_EQ(parsed.status().domain(), core::Status::Code::InvalidArgument);
    EXPECT_NE(parsed.status().message().find(named), std::string::npos)
        << parsed.status().message();
  }
}

TEST(DeviceTiming, ItsNamesRoundTrip) {
  for (const DeviceTiming timing :
       {DeviceTiming::kOff, DeviceTiming::kSampled, DeviceTiming::kAll}) {
    EXPECT_EQ(vkp::parse_device_timing(vkp::to_string(timing)), timing);
  }
  EXPECT_STREQ(vkp::to_string(DeviceTiming::kSampled), "sampled");
  EXPECT_FALSE(vkp::parse_device_timing("All").has_value());
  EXPECT_FALSE(vkp::parse_device_timing("").has_value());
}

TEST(TileDepthAndSwitch, TakeTheirNamesAlone) {
  EXPECT_EQ(vkp::parse_tile_depth("0"), 0u);
  EXPECT_EQ(vkp::parse_tile_depth("1"), 1u);
  for (const char* text : {"2", "", "01", " 1", "one"}) {
    EXPECT_FALSE(vkp::parse_tile_depth(text).has_value()) << "'" << text << "'";
  }
  EXPECT_EQ(vkp::parse_switch("on"), true);
  EXPECT_EQ(vkp::parse_switch("off"), false);
  for (const char* text : {"On", "", "1", "yes"}) {
    EXPECT_FALSE(vkp::parse_switch(text).has_value()) << "'" << text << "'";
  }
}

class DeviceLineTest : public core::test::VulkanDeviceTest {
 protected:
  core::DeviceRequirements requirements() const override {
    return volumetric_kit::recon::device_requirements();
  }
};

TEST_F(DeviceLineTest, ItNamesTheDeviceItsDriverMemoryAndTimestamps) {
  const std::string line = vkp::device_line(device());
  const std::string name = device().caps().properties().deviceName;
  EXPECT_EQ(line.rfind(name + "; vendor 0x", 0), 0u) << line;
  // The core's memory facts and the calibration, as the device states them.
  const std::string facts =
      std::string("; local and mapped ") +
      (device().caps().device_mapped_memory() ? "yes" : "no") + ", cached " +
      (device().caps().device_mapped_cached() ? "yes" : "no") + "; timestamps ";
  EXPECT_NE(line.find(facts), std::string::npos) << line;
  const std::string calibrated = device().calibrate().ok()
                                     ? "; calibrated yes, within \\d+ ns$"
                                     : "; calibrated no$";
  EXPECT_TRUE(std::regex_search(line, std::regex(calibrated))) << line;
  EXPECT_TRUE(std::regex_search(
      line, std::regex("; driver [^;]+; Vulkan \\d+\\.\\d+\\.\\d+; heaps "
                       "\\d+ MiB( local)?(, \\d+ MiB( local)?)*; local and "
                       "mapped (yes|no), cached (yes|no); timestamps \\d+ "
                       "bits, [0-9.e+-]+ ns a tick; calibrated")))
      << line;
  EXPECT_EQ(line.find('\n'), std::string::npos);
}

// recon's rings run in place exactly where the core's HostRing does: a ring
// made on the device says.
TEST_F(DeviceLineTest, TheRingsRunInPlaceExactlyWhereAHostRingDoes) {
  core::HostRingDesc desc;
  desc.slot_bytes = 64;
  core::Result<std::unique_ptr<core::HostRing>> ring =
      core::HostRing::create(device(), allocator(), desc);
  ASSERT_TRUE(ring.ok()) << ring.status().message();
  EXPECT_EQ(vkp::rings_line(device()),
            (*ring)->in_place() ? "in place" : "copied");
}

}  // namespace
