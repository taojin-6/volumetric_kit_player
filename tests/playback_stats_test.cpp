// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// PlaybackStats on the host, with the time passed in: the track's mean
// bitrate, and none below 2 records or over a pts span of 0; the nominal
// rate, and none when unknown; the stage means over the last frames, as many
// as the nominal rate's frames a second (30 when unknown), CPU over every
// frame and GPU over those that have one, held while no frame comes; and the
// rate frames are taken at over the last second.

#include "volumetric_kit/player/playback_stats.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "volumetric_kit/core/base/stage_metrics.hpp"
#include "volumetric_kit/player/stream_player.hpp"
#include "volumetric_kit/recon/stream/session.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace stream = volumetric_kit::recon::stream;
using std::chrono::milliseconds;
using std::chrono::seconds;
using volumetric_kit::player::FrameStamp;
using volumetric_kit::player::PlaybackStats;
using TimePoint = PlaybackStats::Clock::time_point;

const TimePoint t0{seconds(100)};

// A frame whose "read" took `read_ms`, and whose "decode" took `decode_ms`
// on the host and, if given, `gpu_ms` on the device.
FrameStamp frame(double read_ms, double decode_ms = 1.0,
                 std::optional<double> gpu_ms = std::nullopt) {
  FrameStamp stamp;
  stamp.stages.add_cpu("read", read_ms);
  stamp.stages.add_cpu("decode", decode_ms);
  if (gpu_ms) {
    stamp.stages.add_gpu("decode", *gpu_ms);
  }
  return stamp;
}

// `stats`' mean of row `name`; a failure, and an empty row, with none.
core::StageRow mean_of(const PlaybackStats& stats, const char* name) {
  for (const core::StageRow& row : stats.stage_means()) {
    if (std::string(row.name) == name) {
      return row;
    }
  }
  ADD_FAILURE() << "no row " << name;
  return {};
}

TEST(PlaybackStats, TheBitrateIsTheTracksMean) {
  // 30 frames of 1000 bytes at 30 fps: 8000 bits a frame, 30 frames a second.
  stream::TrackStats track;
  track.records = 30;
  track.payload_bytes = 30 * 1000;
  track.min_pts_ns = 5'000'000'000;
  track.max_pts_ns = track.min_pts_ns + 29 * 1'000'000'000ULL / 30;
  const PlaybackStats stats(track, {30, 1});
  ASSERT_TRUE(stats.mean_mbit_per_s().has_value());
  EXPECT_NEAR(*stats.mean_mbit_per_s(), 0.24, 1e-6);
}

TEST(PlaybackStats, NoBitrateBelowTwoRecordsOrOverNoTime) {
  stream::TrackStats one;
  one.records = 1;
  one.payload_bytes = 1000;
  EXPECT_FALSE(PlaybackStats(one, {30, 1}).mean_mbit_per_s().has_value());
  stream::TrackStats still;  // pts that do not increase
  still.records = 5;
  still.payload_bytes = 5000;
  still.min_pts_ns = 40;
  still.max_pts_ns = 40;
  EXPECT_FALSE(PlaybackStats(still, {30, 1}).mean_mbit_per_s().has_value());
}

TEST(PlaybackStats, TheNominalRateIsTheTracks) {
  EXPECT_EQ(PlaybackStats({}, {25, 1}).nominal_fps(), 25.0);
  EXPECT_DOUBLE_EQ(*PlaybackStats({}, {30000, 1001}).nominal_fps(),
                   30000.0 / 1001.0);
  EXPECT_FALSE(PlaybackStats({}, {0, 1}).nominal_fps().has_value());
  EXPECT_FALSE(PlaybackStats({}, {30, 0}).nominal_fps().has_value());
}

TEST(PlaybackStats, NoMeansBeforeTheFirstFrame) {
  EXPECT_TRUE(PlaybackStats({}, {30, 1}).stage_means().empty());
}

// The newest frame's rows, in its order; CPU over every frame in the window,
// GPU over the frames whose row has a device span.
TEST(PlaybackStats, CpuMeansCoverEveryFrameAndGpuMeansThoseWithOne) {
  PlaybackStats stats({}, {30, 1});
  stats.add_frame(frame(1.0, 2.0, 1.0), t0);
  stats.add_frame(frame(2.0, 4.0), t0);
  stats.add_frame(frame(3.0, 6.0, 3.0), t0);
  const std::vector<core::StageRow> means = stats.stage_means();
  ASSERT_EQ(means.size(), 2u);
  EXPECT_STREQ(means[0].name, "read");
  EXPECT_STREQ(means[1].name, "decode");
  EXPECT_DOUBLE_EQ(means[0].cpu_ms, 2.0);
  EXPECT_FALSE(means[0].has_gpu);
  EXPECT_DOUBLE_EQ(means[1].cpu_ms, 4.0);
  ASSERT_TRUE(means[1].has_gpu);
  EXPECT_DOUBLE_EQ(means[1].gpu_ms, 2.0);
}

TEST(PlaybackStats, NoGpuInTheWindowHasNoGpuMean) {
  PlaybackStats stats({}, {3, 1});
  stats.add_frame(frame(1.0, 1.0, 5.0), t0);
  for (int i = 0; i < 3; ++i) {
    stats.add_frame(frame(1.0), t0);
  }
  EXPECT_FALSE(mean_of(stats, "decode").has_gpu);
}

// The window is the nominal rate's frames a second, rounded: 25 at 25 fps, 30
// at 30000/1001 and when unknown. Frame i reads for i ms.
TEST(PlaybackStats, TheWindowIsAsManyFramesAsASecondHolds) {
  for (const auto& [num, den, window] :
       {std::tuple{25u, 1u, 25}, std::tuple{30000u, 1001u, 30},
        std::tuple{0u, 1u, 30}}) {
    SCOPED_TRACE(::testing::Message() << num << "/" << den);
    PlaybackStats stats({}, {num, den});
    constexpr int kFrames = 40;
    for (int i = 1; i <= kFrames; ++i) {
      stats.add_frame(frame(i), t0);
    }
    // The mean of kFrames - window + 1 to kFrames.
    EXPECT_DOUBLE_EQ(mean_of(stats, "read").cpu_ms,
                     kFrames - 0.5 * (window - 1));
  }
}

// Paused, no frame is taken, so the means hold; a step moves the window by
// one frame.
TEST(PlaybackStats, PausedTheMeansHoldAndAStepMovesThemByOne) {
  PlaybackStats stats({}, {3, 1});
  for (const double read : {1.0, 2.0, 3.0}) {
    stats.add_frame(frame(read), t0);
  }
  EXPECT_DOUBLE_EQ(mean_of(stats, "read").cpu_ms, 2.0);
  EXPECT_DOUBLE_EQ(mean_of(stats, "read").cpu_ms, 2.0);
  stats.add_frame(frame(4.0), t0 + seconds(60));
  EXPECT_DOUBLE_EQ(mean_of(stats, "read").cpu_ms, 3.0);
}

TEST(PlaybackStats, TheRateIsFramesTakenOverTheLastSecond) {
  PlaybackStats stats({}, {30, 1});
  EXPECT_EQ(stats.frames_per_s(t0), 0.0);
  stats.add_frame(frame(1.0), t0);
  EXPECT_EQ(stats.frames_per_s(t0), 0.0) << "below 2 frames";
  // 10 frames 100 ms apart: 9 steps over 0.9 s.
  for (int i = 1; i < 10; ++i) {
    stats.add_frame(frame(1.0), t0 + milliseconds(100 * i));
  }
  EXPECT_DOUBLE_EQ(stats.frames_per_s(t0 + milliseconds(900)), 10.0);
  // Half a second on, the first 5 have left the last second.
  EXPECT_DOUBLE_EQ(stats.frames_per_s(t0 + milliseconds(1400)), 10.0);
  EXPECT_EQ(stats.frames_per_s(t0 + milliseconds(1899)), 0.0) << "one left";
  EXPECT_EQ(stats.frames_per_s(t0 + milliseconds(2000)), 0.0)
      << "a second with none";
  // Two frames 50 ms apart, after the gap.
  stats.add_frame(frame(1.0), t0 + milliseconds(3000));
  stats.add_frame(frame(1.0), t0 + milliseconds(3050));
  EXPECT_DOUBLE_EQ(stats.frames_per_s(t0 + milliseconds(3050)), 20.0);
}

}  // namespace
