// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// PlaybackStats on the host, with the time passed in: the track's mean
// bitrate, and none below 2 records, over a pts span of 0 or from no track (a
// live stream's); the nominal rate, and none when unknown; the stage means
// over the last frames, as many as the nominal rate's frames a second (30
// when unknown, at most 240), host and wait over every frame that has the row
// and GPU over those that measured it, its state ranked, a row only timed
// frames have kept under its stage, and the frames timed counted; held while
// no frame comes; the rate frames are taken at over the
// last second; and the bitrate and records a second their records brought,
// over the frames published in the last second, so a pause in taking does
// not crowd them into a moment; and a frame's figures (its device time, its
// breakdowns aside, its queue idle and latency, its latency legs, live the
// wait as bytes first and the publish leg from the release), each averaged
// over the window's frames that have it. A paused toggle's rebuild
// of the frame before changes none of them.

#include "volumetric_kit/player/playback_stats.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "volumetric_kit/core/base/stage_metrics.hpp"
#include "volumetric_kit/grid/stream/session.hpp"
#include "volumetric_kit/player/stream_player.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace stream = volumetric_kit::grid::stream;
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
  EXPECT_EQ(mean_of(stats, "decode").device, core::DeviceTime::None);
}

// Sampled: a timed frame's breakdowns keep their place under their stage
// though the newest frame has none, each mean over the frames that have it;
// the window counts its frames and those timed.
TEST(PlaybackStats, RowsSomeFramesHaveStayUnderTheirStageMeanOverThoseFrames) {
  PlaybackStats stats({}, {30, 1});
  FrameStamp timed = frame(1.0, 4.0, 2.0);
  timed.timed = true;
  timed.stages.add_cpu("  ..rans decode", 1.5);
  timed.stages.add_gpu("  ..rans decode", 1.0);
  timed.stages.add_cpu("mesh", 3.0);
  timed.stages.add_cpu("  ..dispatch", 2.0);
  stats.add_frame(timed, t0);
  for (int i = 0; i < 3; ++i) {
    FrameStamp untimed = frame(1.0, 2.0);
    untimed.stages.add_cpu("mesh", 1.0);
    stats.add_frame(untimed, t0);
  }
  const std::vector<core::StageRow> means = stats.stage_means();
  std::vector<std::string> names;
  for (const core::StageRow& row : means) {
    names.emplace_back(row.name);
  }
  EXPECT_EQ(names,
            (std::vector<std::string>{"read", "decode", "  ..rans decode",
                                      "mesh", "  ..dispatch"}));
  EXPECT_DOUBLE_EQ(mean_of(stats, "decode").cpu_ms, 2.5);
  EXPECT_DOUBLE_EQ(mean_of(stats, "decode").gpu_ms, 2.0);
  EXPECT_DOUBLE_EQ(mean_of(stats, "  ..rans decode").cpu_ms, 1.5);
  EXPECT_DOUBLE_EQ(mean_of(stats, "  ..rans decode").gpu_ms, 1.0);
  EXPECT_DOUBLE_EQ(mean_of(stats, "mesh").cpu_ms, 1.5);
  EXPECT_DOUBLE_EQ(mean_of(stats, "  ..dispatch").cpu_ms, 2.0);
  EXPECT_EQ(stats.window_frames(), 4u);
  EXPECT_EQ(stats.timed_frames(), 1u);
}

// A row's wait is its mean over the frames; its device state the highest of
// theirs, so measured only when every frame that timed it measured it, its
// time over those measured.
TEST(PlaybackStats, WaitsAverageAndDeviceStatesRank) {
  PlaybackStats stats({}, {30, 1});
  for (const auto& [wait_ms, device] :
       {std::pair{1.0, core::DeviceTime::Measured},
        std::pair{3.0, core::DeviceTime::Measured},
        std::pair{2.0, core::DeviceTime::None}}) {
    FrameStamp stamp = frame(1.0, 4.0);
    stamp.stages.add_wait("decode", wait_ms);
    stamp.stages.add_device("decode", device, wait_ms);
    stats.add_frame(stamp, t0);
  }
  core::StageRow decode = mean_of(stats, "decode");
  EXPECT_DOUBLE_EQ(decode.wait_ms, 2.0);
  EXPECT_EQ(decode.device, core::DeviceTime::Measured);
  EXPECT_TRUE(decode.has_gpu);
  EXPECT_DOUBLE_EQ(decode.gpu_ms, 2.0) << "over the two measured";
  for (const core::DeviceTime worse :
       {core::DeviceTime::NotTimed, core::DeviceTime::Unavailable}) {
    FrameStamp stamp = frame(1.0, 4.0);
    stamp.stages.add_device("decode", worse);
    stats.add_frame(stamp, t0);
    decode = mean_of(stats, "decode");
    EXPECT_EQ(decode.device, worse);
    EXPECT_FALSE(decode.has_gpu);
  }
}

// The window is the nominal rate's frames a second, rounded: 25 at 25 fps, 30
// at 30000/1001 and when unknown, and at most kMaxWindow, as for a 90 kHz
// timebase written as the rate or a rate whose period rounds to 0 ns. Frame i
// reads for i ms.
TEST(PlaybackStats, TheWindowIsAsManyFramesAsASecondHolds) {
  constexpr int kMost = static_cast<int>(PlaybackStats::kMaxWindow);
  for (const auto& [num, den, window] :
       {std::tuple{25u, 1u, 25}, std::tuple{30000u, 1001u, 30},
        std::tuple{0u, 1u, 30}, std::tuple{90'000u, 1u, kMost},
        std::tuple{4'000'000'000u, 1u, kMost}}) {
    SCOPED_TRACE(::testing::Message() << num << "/" << den);
    PlaybackStats stats({}, {num, den});
    constexpr int kFrames = 300;
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

// Paused, as a deblock toggle's rebuilds come: the means and the rate hold.
TEST(PlaybackStats, ARebuildOfTheFrameBeforeIsNoFrame) {
  PlaybackStats stats({}, {3, 1});
  for (int i = 0; i < 3; ++i) {
    stats.add_frame(frame(1.0 + i), t0 + milliseconds(100 * i));
  }
  const TimePoint now = t0 + milliseconds(300);
  ASSERT_DOUBLE_EQ(stats.frames_per_s(now), 10.0);
  FrameStamp rebuild = frame(0.0, 50.0);
  rebuild.rebuild = true;
  rebuild.published = now;
  stats.add_frame(rebuild, now);
  stats.add_frame(rebuild, now + milliseconds(5));
  EXPECT_DOUBLE_EQ(stats.frames_per_s(now + milliseconds(5)), 10.0);
  EXPECT_DOUBLE_EQ(mean_of(stats, "read").cpu_ms, 2.0);
  EXPECT_DOUBLE_EQ(mean_of(stats, "decode").cpu_ms, 1.0);
  EXPECT_EQ(stats.window_frames(), 3u);
}

// A live stream's view passes no track and no rate.
TEST(PlaybackStats, NoMeanFromAnEmptyTrack) {
  const PlaybackStats stats({}, {});
  EXPECT_FALSE(stats.mean_mbit_per_s().has_value());
  EXPECT_FALSE(stats.nominal_fps().has_value());
}

// A frame published at `at` after `records` records of `bytes` bytes in all.
FrameStamp after(std::uint64_t records, std::uint64_t bytes, TimePoint at) {
  FrameStamp stamp;
  stamp.records = records;
  stamp.record_bytes = bytes;
  stamp.published = at;
  return stamp;
}

// Over the frames published in the last second, each but the first, which
// counts the records before the window; here each is taken as it is
// published.
TEST(PlaybackStats, TheRollingBitrateAndRecordsCoverTheLastSecond) {
  PlaybackStats stats({}, {});
  EXPECT_FALSE(stats.rolling_mbit_per_s(t0).has_value());
  EXPECT_EQ(stats.records_per_s(t0), 0.0);
  stats.add_frame(after(50, 1'000'000, t0), t0);
  EXPECT_FALSE(stats.rolling_mbit_per_s(t0).has_value()) << "below 2 frames";
  EXPECT_EQ(stats.records_per_s(t0), 0.0);
  // 4 more 200 ms apart, of 2 records and 25 kB each: 0.8 Mbit and 8 records
  // over 0.8 s.
  for (int i = 1; i <= 4; ++i) {
    const TimePoint at = t0 + milliseconds(200 * i);
    stats.add_frame(after(2, 25'000, at), at);
  }
  const TimePoint now = t0 + milliseconds(800);
  ASSERT_TRUE(stats.rolling_mbit_per_s(now).has_value());
  EXPECT_NEAR(*stats.rolling_mbit_per_s(now), 1.0, 1e-9);
  EXPECT_NEAR(stats.records_per_s(now), 10.0, 1e-9);
  // Once the first has left the last second, the second is the first: 0.6
  // Mbit and 6 records over 0.6 s.
  const TimePoint later = t0 + milliseconds(1100);
  ASSERT_TRUE(stats.rolling_mbit_per_s(later).has_value());
  EXPECT_NEAR(*stats.rolling_mbit_per_s(later), 1.0, 1e-9);
  EXPECT_NEAR(stats.records_per_s(later), 10.0, 1e-9);
  // A second with none: no figure.
  EXPECT_FALSE(stats.rolling_mbit_per_s(t0 + seconds(3)).has_value());
  EXPECT_EQ(stats.records_per_s(t0 + seconds(3)), 0.0);
  // Published all at once: no time to divide by.
  PlaybackStats burst({}, {});
  burst.add_frame(after(1, 1'000, t0), t0);
  burst.add_frame(after(1, 1'000, t0), t0);
  EXPECT_FALSE(burst.rolling_mbit_per_s(t0).has_value());
  EXPECT_EQ(burst.records_per_s(t0), 0.0);
}

// A pause in taking, as a minimized window's: the frame that waited is taken
// 10 s late, and the next, 2 ms after it, stands for every record of the
// pause. Over when the frames were published, the figures stay the records'
// own, 10 a second of 1 kB (0.08 Mbit/s), rather than the pause's records
// over those 2 ms.
TEST(PlaybackStats, APauseInTakingDoesNotCrowdItsRecordsIntoAMoment) {
  constexpr double kRecords = 10.0;  // a second
  constexpr double kMbit = kRecords * 8'000 * 1e-6;
  PlaybackStats stats({}, {});
  // A second of frames of a record each, taken as they are published.
  for (int i = 0; i <= 10; ++i) {
    const TimePoint at = t0 + milliseconds(100 * i);
    stats.add_frame(after(1, 1'000, at), at);
  }
  const TimePoint restored = t0 + milliseconds(11'100);
  stats.add_frame(after(1, 1'000, t0 + milliseconds(1'100)), restored);
  stats.add_frame(after(100, 100'000, restored + milliseconds(1)),
                  restored + milliseconds(2));
  EXPECT_LE(stats.records_per_s(restored + milliseconds(2)), kRecords + 1e-9);
  EXPECT_LE(stats.rolling_mbit_per_s(restored + milliseconds(2)).value_or(0.0),
            kMbit + 1e-9);
  // Then a record each 100 ms again.
  for (int i = 1; i <= 10; ++i) {
    SCOPED_TRACE(::testing::Message() << "frame " << i << " after the pause");
    const TimePoint at = restored + milliseconds(1 + 100 * i);
    stats.add_frame(after(1, 1'000, at), at);
    EXPECT_NEAR(stats.records_per_s(at), kRecords, 1e-9);
    ASSERT_TRUE(stats.rolling_mbit_per_s(at).has_value());
    EXPECT_NEAR(*stats.rolling_mbit_per_s(at), kMbit, 1e-9);
  }
}

// A frame's figures: its device time, timed and measured, its stage rows'
// but its breakdowns', whose tiles its stages hold; its queue idle and
// latency as stamped; each latency leg whose ends it has, in order; and with
// all three, its latency, due to presented.
TEST(FrameFigures, AreTheStampsEachAsTheFrameHasIt) {
  using volumetric_kit::player::frame_figures;
  FrameStamp stamp = frame(0.5, 2.0, 5.0);
  stamp.stages.add_gpu("  ..rans decode", 4.0);
  stamp.stages.add_gpu("mesh", 1.0);
  EXPECT_FALSE(frame_figures(stamp).gpu_ms.has_value()) << "untimed";
  stamp.timed = true;
  stamp.device_idle_ms = 20.0;
  stamp.queue_ms = 0.5;
  stamp.due = t0;
  stamp.published = t0 + milliseconds(2);
  stamp.taken = t0 + milliseconds(10);
  stamp.presented = t0 + milliseconds(13);
  const volumetric_kit::player::FrameFigures figures = frame_figures(stamp);
  EXPECT_DOUBLE_EQ(figures.gpu_ms.value_or(0.0), 6.0);
  EXPECT_DOUBLE_EQ(figures.device_idle_ms.value_or(0.0), 20.0);
  EXPECT_DOUBLE_EQ(figures.queue_ms.value_or(0.0), 0.5);
  EXPECT_DOUBLE_EQ(figures.latency_ms.value_or(0.0), 13.0);
  EXPECT_DOUBLE_EQ(figures.publish_ms.value_or(0.0), 2.0);
  EXPECT_DOUBLE_EQ(figures.take_ms.value_or(0.0), 8.0);
  EXPECT_DOUBLE_EQ(figures.present_ms.value_or(0.0), 3.0);
  // No present: its other legs, and no latency.
  stamp.presented.reset();
  const volumetric_kit::player::FrameFigures unpresented = frame_figures(stamp);
  EXPECT_TRUE(unpresented.publish_ms.has_value());
  EXPECT_TRUE(unpresented.take_ms.has_value());
  EXPECT_FALSE(unpresented.latency_ms.has_value());
  // No present, no due, a leg out of order: none of those legs.
  stamp.due = {};
  stamp.taken = t0;  // before it was published
  const volumetric_kit::player::FrameFigures some = frame_figures(stamp);
  EXPECT_FALSE(some.publish_ms.has_value());
  EXPECT_FALSE(some.take_ms.has_value());
  EXPECT_FALSE(some.present_ms.has_value());
  EXPECT_FALSE(some.latency_ms.has_value());
  // Timed with nothing measured, as on a queue that cannot time: none.
  FrameStamp unmeasured = frame(0.5);
  unmeasured.timed = true;
  unmeasured.stages.add_device("decode", core::DeviceTime::Unavailable);
  EXPECT_FALSE(frame_figures(unmeasured).gpu_ms.has_value());
}

// Live (taken with a playout), the wait as bytes is the buffer leg, due to
// released, and the publish leg runs from the release; the latency, due to
// presented, is their sum with the take and the present. A file's has no
// buffer leg, its release its due.
TEST(FrameFigures, LiveTheBufferLegLeadsAndThePublishRunsFromTheRelease) {
  using volumetric_kit::player::frame_figures;
  FrameStamp stamp = frame(0.5);
  stamp.due = t0;
  stamp.released = t0 + milliseconds(120);
  stamp.published = t0 + milliseconds(129);
  stamp.taken = t0 + milliseconds(137);
  stamp.presented = t0 + milliseconds(138);
  stamp.playout = volumetric_kit::player::Playout{};
  const volumetric_kit::player::FrameFigures live = frame_figures(stamp);
  EXPECT_DOUBLE_EQ(live.buffer_ms.value_or(0.0), 120.0);
  EXPECT_DOUBLE_EQ(live.publish_ms.value_or(0.0), 9.0);
  EXPECT_DOUBLE_EQ(live.take_ms.value_or(0.0), 8.0);
  EXPECT_DOUBLE_EQ(live.latency_ms.value_or(0.0), 138.0);
  stamp.playout.reset();
  stamp.released = stamp.due;  // a file's
  const volumetric_kit::player::FrameFigures file = frame_figures(stamp);
  EXPECT_FALSE(file.buffer_ms.has_value());
  EXPECT_DOUBLE_EQ(file.publish_ms.value_or(0.0), 129.0);
  // The window's mean is over the frames that have it.
  PlaybackStats stats({}, {});
  stamp.playout = volumetric_kit::player::Playout{};
  stamp.released = t0 + milliseconds(100);
  stats.add_frame(stamp, *stamp.taken);
  stamp.released = t0 + milliseconds(110);
  stats.add_frame(stamp, *stamp.taken);
  EXPECT_DOUBLE_EQ(stats.frame_means().buffer_ms.value_or(0.0), 105.0);
}

// The window's per-frame figures, each a mean over the frames that have it,
// as their stage means are; a rebuild changes none of them.
TEST(PlaybackStats, FrameMeansCoverTheFramesThatHaveEachFigure) {
  PlaybackStats stats({}, {30, 1});
  EXPECT_FALSE(stats.frame_means().gpu_ms.has_value()) << "no frame yet";
  for (int i = 0; i < 4; ++i) {
    FrameStamp stamp = frame(
        0.5, 1.0, i % 2 == 0 ? std::optional<double>(2.0 * i) : std::nullopt);
    stamp.timed = i % 2 == 0;
    if (i > 0) {
      stamp.device_idle_ms = 10.0 * i;
    }
    stamp.due = t0 + milliseconds(40 * i);
    stamp.published = stamp.due + milliseconds(1 + i);
    stamp.taken = stamp.published + milliseconds(5);
    if (i < 2) {
      stamp.presented = *stamp.taken + milliseconds(1);
    }
    stats.add_frame(stamp, *stamp.taken);
  }
  FrameStamp rebuild = frame(9.0, 9.0, 99.0);
  rebuild.timed = true;
  rebuild.rebuild = true;
  rebuild.device_idle_ms = 999.0;
  stats.add_frame(rebuild, t0 + seconds(1));
  const volumetric_kit::player::FrameFigures means = stats.frame_means();
  EXPECT_DOUBLE_EQ(means.gpu_ms.value_or(-1.0), 2.0) << "frames 0 and 2";
  EXPECT_DOUBLE_EQ(means.device_idle_ms.value_or(-1.0), 20.0) << "1 to 3";
  EXPECT_FALSE(means.queue_ms.has_value()) << "none calibrated";
  EXPECT_DOUBLE_EQ(means.latency_ms.value_or(-1.0), 7.5) << "frames 0 and 1";
  EXPECT_DOUBLE_EQ(means.publish_ms.value_or(-1.0), 2.5);
  EXPECT_DOUBLE_EQ(means.take_ms.value_or(-1.0), 5.0);
  EXPECT_DOUBLE_EQ(means.present_ms.value_or(-1.0), 1.0) << "frames 0 and 1";
}

}  // namespace
