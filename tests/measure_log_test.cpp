// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The measure log on the host: its header names the build, the device, the
// stream, the timing and the time, then the columns; each frame is its stage
// rows, host as busy time, GPU only where measured and each device state by
// name, a breakdown's name kept and a name with a comma quoted, then its
// device counts; nothing is at the path until finish renames the part file,
// and after it nothing more is written; a folder that is not there is
// refused. The time reads as UTC, and a log's file name is it and the
// player's commit. The iOS app's measure arguments parse with the system's
// left alone, and what is wrong is named; the timings' names round-trip. On
// a device: its line names it, its driver, its memory and its timestamps.

#include "volumetric_kit/player/measure_log.hpp"

#include <gtest/gtest.h>

#include <chrono>
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
  context.stream = "fixture.vrst, track 0";
  context.timing = DeviceTiming::kSampled;
  context.started = kStarted;
  return context;
}

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
  stamp.counters.submits = 5;
  stamp.counters.host_waits = 4;
  stamp.counters.blocked_ns = 1'288'731;
  stamp.counters.staging_buffers = 3;
  return stamp;
}

TEST(MeasureLog, TheHeaderNamesWhatWasMeasuredThenTheColumns) {
  vp_test::Scratch scratch;
  const std::string path = scratch.file("run.csv");
  core::Result<std::unique_ptr<MeasureLog>> log =
      MeasureLog::open(path, context());
  ASSERT_TRUE(log.ok()) << log.status().message();
  ASSERT_TRUE((*log)->finish().ok());
  const vkp::BuildInfo& build = vkp::build_info();
  EXPECT_EQ(read_lines(path),
            (Lines{"# format: volumetric_kit_player measure 1",
                   std::string("# player: ") + build.player,
                   std::string("# core: ") + build.core,
                   std::string("# recon: ") + build.recon,
                   std::string("# gfx: ") + build.gfx,
                   std::string("# build: ") + build.build_type,
                   "# device: Test GPU; vendor 0x1234",
                   "# stream: fixture.vrst, track 0", "# timing: sampled",
                   "# utc: 2026-10-23T05:12:00Z",
                   "pass,frame,type,bytes,triangles,stage,host_ms,wait_ms,"
                   "gpu_ms,device,submits,host_waits,blocked_ns,staging_"
                   "buffers"}));
  EXPECT_EQ((*log)->frames(), 0u);
}

TEST(MeasureLog, AFrameIsItsStageRowsThenItsDeviceCounts) {
  vp_test::Scratch scratch;
  const std::string path = scratch.file("run.csv");
  core::Result<std::unique_ptr<MeasureLog>> log =
      MeasureLog::open(path, context());
  ASSERT_TRUE(log.ok()) << log.status().message();
  ASSERT_TRUE((*log)->add(stamp(), 510).ok());
  FrameStamp key;
  key.key = true;
  key.stages.add_cpu("a, b", 1.0);
  ASSERT_TRUE((*log)->add(key, 0).ok());
  EXPECT_EQ((*log)->frames(), 2u);
  ASSERT_TRUE((*log)->finish().ok());
  const Lines lines = read_lines(path);
  ASSERT_EQ(lines.size(), 11u + 5u + 2u);
  EXPECT_EQ(Lines(lines.begin() + 11, lines.end()),
            (Lines{"2,7,P,600,510,read,0.2500,0.0000,,none,,,,",
                   "2,7,P,600,510,decode,2.0000,1.0000,0.5000,measured,,,,",
                   "2,7,P,600,510,  ..rans decode,1.0000,0.0000,,not_timed,,,,",
                   "2,7,P,600,510,mesh,0.0000,2.5000,,unavailable,,,,",
                   "2,7,P,600,510,,,,,,5,4,1288731,3",
                   "1,0,I,0,0,\"a, b\",1.0000,0.0000,,none,,,,",
                   "1,0,I,0,0,,,,,,0,0,0,0"}));
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
  EXPECT_FALSE(plain.value()->frames.has_value()) << "one pass";
  EXPECT_EQ(plain.value()->timing, DeviceTiming::kAll);
  const core::Result<std::optional<MeasureArgs>> full =
      vkp::parse_measure_args({"--timing", "sampled", "-AppleLanguages", "(en)",
                               "--frames", "300", "--measure", "rafa2.vrst"});
  ASSERT_TRUE(full.ok() && full.value().has_value()) << full.status().message();
  EXPECT_EQ(full.value()->stream, "rafa2.vrst");
  EXPECT_EQ(full.value()->frames, 300u);
  EXPECT_EQ(full.value()->timing, DeviceTiming::kSampled);
}

TEST(MeasureArgs, WhatIsWrongIsNamed) {
  for (const auto& [args, named] :
       std::vector<std::pair<std::vector<std::string>, std::string>>{
           {{"--measure"}, "--measure needs a value"},
           {{"--measure", "a/b.vrst"}, "--measure 'a/b.vrst'"},
           {{"--measure", ""}, "--measure ''"},
           {{"--measure", "a.vrst", "--frames", "0"}, "--frames '0'"},
           {{"--measure", "a.vrst", "--timing", "some"}, "--timing 'some'"},
           {{"--frames", "10"}, "need --measure"}}) {
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
  EXPECT_TRUE(std::regex_search(
      line, std::regex("; driver [^;]+; Vulkan \\d+\\.\\d+\\.\\d+; heaps "
                       "\\d+ MiB( local)?(, \\d+ MiB( local)?)*; local and "
                       "mapped (yes|no), cached (yes|no); timestamps \\d+ "
                       "bits, [0-9.e+-]+ ns a tick; calibrated (yes, within "
                       "\\d+ ns|no)$")))
      << line;
  EXPECT_EQ(line.find('\n'), std::string::npos);
}

}  // namespace
