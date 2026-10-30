// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// What a device output hears when, as pure functions, on the host. The host
// clock's ticks convert to ns by its timebase with no overflow; the output
// latency is the presentation latency plus the session's, a part that is
// negative or not finite counting as 0. The re-anchor rule: a render that
// took nothing anchors nothing; a flush, a device start, a closed run, pts
// that do not follow on, or a time more than kFollowNs off the run's end
// start a new run, at the render's latency; else the render extends the
// run, at its own time. The pts heard: none before a flush, the flush's pts
// before any run, within a run its end less what is queued and its latency,
// clamped to the run, the previous run's, at its own latency, until the
// current one's first frame is heard, so never back across a restart onto a
// longer latency, and held once the frames stop. A clock's rate against
// another is the least-squares slope, and its largest residual the reading
// farthest off that line.

#include "volumetric_kit/player/audio_clock.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace {

using volumetric_kit::player::after_render;
using volumetric_kit::player::audio_ns;
using volumetric_kit::player::clock_fit;
using volumetric_kit::player::ClockSample;
using volumetric_kit::player::heard_pts;
using volumetric_kit::player::HeardClock;
using volumetric_kit::player::HeardRun;
using volumetric_kit::player::host_ticks_to_ns;
using volumetric_kit::player::kFollowNs;
using volumetric_kit::player::output_latency_ns;
using volumetric_kit::player::reanchors;
using volumetric_kit::player::Rendered;
using volumetric_kit::player::RenderTime;

constexpr std::uint32_t kRate = 8000;           // a frame is 125 us
constexpr std::size_t kFrames = 80;             // a render: 10 ms
constexpr std::uint64_t kMs = 1'000'000;        // ns
constexpr std::uint64_t kPts = 5'000 * kMs;     // a flush's pts
constexpr std::uint64_t kHost = 900'000 * kMs;  // a render's host time

// A render that took `frames` frames from `pts` on; flushing first, if so.
Rendered took(std::uint64_t pts, std::size_t frames, bool flushed = false) {
  Rendered rendered;
  rendered.based = true;
  rendered.flushed = flushed;
  rendered.frames = frames;
  rendered.pts_ns = pts;
  rendered.end_pts_ns = pts + audio_ns(frames, kRate);
  return rendered;
}

// A render of kFrames frames whose first passes the output at `host_ns`, on
// a route of `latency_ns`.
RenderTime at(std::uint64_t host_ns, bool restarted = false,
              std::uint64_t latency_ns = 0) {
  RenderTime time;
  time.frames = kFrames;
  time.host_ns = host_ns;
  time.restarted = restarted;
  time.latency_ns = latency_ns;
  return time;
}

// A clock flushed to kPts with one full render at kHost, on a route of
// `latency_ns`.
HeardClock one_run(std::uint64_t latency_ns = 0) {
  return after_render(HeardClock{}, took(kPts, kFrames, true),
                      at(kHost, false, latency_ns), kRate);
}

TEST(HostTicks, ConvertByTheTimebaseWithNoOverflow) {
  EXPECT_EQ(host_ticks_to_ns(24'000'000, 125, 3), 1'000'000'000u);
  EXPECT_EQ(host_ticks_to_ns(7, 125, 3), 291u);  // 291.67, rounded down
  EXPECT_EQ(host_ticks_to_ns(123'456'789, 1, 1), 123'456'789u);
  // 3e17 ticks times 125 overflows 64 bits; the answer does not.
  EXPECT_EQ(host_ticks_to_ns(300'000'000'000'000'000ull, 125, 3),
            12'500'000'000'000'000'000ull);
}

TEST(OutputLatency, IsThePresentationLatencyPlusTheSessions) {
  EXPECT_EQ(output_latency_ns(0.010, 0.0), 10 * kMs);
  EXPECT_EQ(output_latency_ns(0.0123456789, 0.005), 17'345'679u);
  EXPECT_EQ(output_latency_ns(0.0, 0.180), 180 * kMs);
}

TEST(OutputLatency, APartNegativeOrNotFiniteCountsAsZero) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  EXPECT_EQ(output_latency_ns(-0.010, 0.005), 5 * kMs);
  EXPECT_EQ(output_latency_ns(nan, 0.002), 2 * kMs);
  EXPECT_EQ(output_latency_ns(0.003, inf), 3 * kMs);
  EXPECT_EQ(output_latency_ns(inf, nan), 0u);
}

TEST(Reanchors, ARenderThatTookNothingAnchorsNothing) {
  EXPECT_FALSE(reanchors(HeardClock{}, took(kPts, 0, true), at(kHost)));
  EXPECT_FALSE(reanchors(one_run(), took(kPts + 10 * kMs, 0), at(kHost, true)));
}

TEST(Reanchors, AFlushOrAStartAnchors) {
  const HeardClock clock = one_run();
  const std::uint64_t next = kHost + 10 * kMs;
  EXPECT_FALSE(reanchors(clock, took(kPts + 10 * kMs, kFrames), at(next)));
  EXPECT_TRUE(reanchors(clock, took(kPts + 10 * kMs, kFrames, true), at(next)));
  EXPECT_TRUE(reanchors(clock, took(kPts + 10 * kMs, kFrames), at(next, true)));
  EXPECT_TRUE(reanchors(HeardClock{}, took(kPts, kFrames), at(kHost)));
}

// Within kFollowNs of the run's end, early or late, a render follows on;
// past it, it anchors.
TEST(Reanchors, ATimeOffTheRunsEndByMoreThanTheToleranceAnchors) {
  const HeardClock clock = one_run();
  const std::uint64_t end = kHost + 10 * kMs;
  const Rendered next = took(kPts + 10 * kMs, kFrames);
  EXPECT_FALSE(reanchors(clock, next, at(end + kFollowNs)));
  EXPECT_FALSE(reanchors(clock, next, at(end - kFollowNs)));
  EXPECT_TRUE(reanchors(clock, next, at(end + kFollowNs + 1)));
  EXPECT_TRUE(reanchors(clock, next, at(end - kFollowNs - 1)));
}

TEST(Reanchors, PtsThatDoNotFollowOnOrAClosedRunAnchor) {
  const HeardClock clock = one_run();
  const std::uint64_t end = kHost + 10 * kMs;
  EXPECT_TRUE(reanchors(clock, took(kPts + 11 * kMs, kFrames), at(end)));
  // A short render closes the run: the next frames come after silence.
  RenderTime short_time = at(kHost);
  short_time.frames = kFrames + 1;
  const HeardClock closed =
      after_render(HeardClock{}, took(kPts, kFrames, true), short_time, kRate);
  EXPECT_FALSE(closed.open);
  EXPECT_TRUE(reanchors(closed, took(kPts + 10 * kMs, kFrames), at(end)));
}

TEST(AfterRender, NothingPlacedLeavesTheClock) {
  Rendered unplaced = took(kPts, kFrames);
  unplaced.based = false;
  const HeardClock clock =
      after_render(HeardClock{}, unplaced, at(kHost), kRate);
  EXPECT_FALSE(clock.based);
  EXPECT_FALSE(clock.current.has_value());
}

TEST(AfterRender, AFlushClearsTheRunsToItsPts) {
  HeardClock clock = one_run();
  clock = after_render(clock, took(kPts + 10 * kMs, kFrames),
                       at(kHost + 10 * kMs), kRate);
  clock = after_render(clock, took(kPts + 777 * kMs, 0, true),
                       at(kHost + 20 * kMs), kRate);
  EXPECT_TRUE(clock.based);
  EXPECT_EQ(clock.floor_pts_ns, kPts + 777 * kMs);
  EXPECT_FALSE(clock.current.has_value());
  EXPECT_FALSE(clock.previous.has_value());
}

// Extended, the run takes each render's own time, so it keeps the device's
// rate rather than the nominal one.
TEST(AfterRender, ARunExtendsAtEachRendersOwnTime) {
  HeardClock clock = one_run();
  ASSERT_TRUE(clock.current.has_value());
  EXPECT_EQ(clock.current->first_pts_ns, kPts);
  EXPECT_EQ(clock.current->end_pts_ns, kPts + 10 * kMs);
  EXPECT_EQ(clock.current->end_host_ns, kHost + 10 * kMs);
  EXPECT_TRUE(clock.open);
  const std::uint64_t late = kHost + 10 * kMs + 3'000;  // 3 us late
  clock = after_render(clock, took(kPts + 10 * kMs, kFrames), at(late), kRate);
  EXPECT_FALSE(clock.previous.has_value());
  EXPECT_EQ(clock.current->first_pts_ns, kPts);
  EXPECT_EQ(clock.current->end_pts_ns, kPts + 20 * kMs);
  EXPECT_EQ(clock.current->end_host_ns, late + 10 * kMs);
}

TEST(AfterRender, ANewRunKeepsTheOneBefore) {
  HeardClock clock = one_run();
  clock = after_render(clock, took(kPts + 10 * kMs, kFrames),
                       at(kHost + 50 * kMs, true), kRate);
  ASSERT_TRUE(clock.previous.has_value());
  EXPECT_EQ(clock.previous->end_pts_ns, kPts + 10 * kMs);
  EXPECT_EQ(clock.current->first_pts_ns, kPts + 10 * kMs);
  EXPECT_EQ(clock.current->end_host_ns, kHost + 60 * kMs);
}

TEST(AfterRender, ARenderThatTookNothingClosesTheRun) {
  HeardClock clock = one_run();
  clock = after_render(clock, took(kPts + 10 * kMs, 0), at(kHost + 10 * kMs),
                       kRate);
  EXPECT_FALSE(clock.open);
  ASSERT_TRUE(clock.current.has_value());
  EXPECT_EQ(clock.current->end_pts_ns, kPts + 10 * kMs);
}

TEST(HeardPts, NoneBeforeAFlushThenItsPtsUntilARun) {
  EXPECT_FALSE(heard_pts(HeardClock{}, kHost).has_value());
  const HeardClock flushed =
      after_render(HeardClock{}, took(kPts, 0, true), at(kHost), kRate);
  EXPECT_EQ(heard_pts(flushed, kHost + 999 * kMs), kPts);
}

// What is heard at a time is what passed the device's output the run's
// latency before it: the run's end less what is still queued, clamped to
// the run.
TEST(HeardPts, IsTheFramesRenderedLessTheQueuedAndTheLatency) {
  const std::uint64_t latency = 5 * kMs;
  const HeardClock clock = one_run(latency);
  ASSERT_TRUE(clock.current.has_value());
  EXPECT_EQ(clock.current->latency_ns, latency) << "its first render's";
  EXPECT_EQ(heard_pts(clock, kHost - kMs), kPts);  // clamped
  EXPECT_EQ(heard_pts(clock, kHost + latency), kPts);
  EXPECT_EQ(heard_pts(clock, kHost + latency + 4 * kMs), kPts + 4 * kMs);
  EXPECT_EQ(heard_pts(clock, kHost + latency + 40 * kMs),
            kPts + 10 * kMs);  // held: nothing more was rendered
  // Three renders, 30 ms out; 25 ms on, 5 ms of them still queued.
  for (const std::uint64_t ms : {std::uint64_t{0}, latency}) {
    HeardClock three = one_run(ms);
    for (std::uint64_t i = 1; i < 3; ++i) {
      three = after_render(three, took(kPts + i * 10 * kMs, kFrames),
                           at(kHost + i * 10 * kMs, false, ms), kRate);
    }
    EXPECT_EQ(heard_pts(three, kHost + 25 * kMs), kPts + 25 * kMs - ms);
  }
}

// A restart onto a route of longer latency (speakers at 10 ms, AirPods at
// 180 ms): the run before keeps its own, so the pts heard never go back,
// holding at its end until the new run's first frame is heard. Onto a
// shorter one they jump ahead. Mutant: one latency for every run.
TEST(HeardPts, EachRunIsHeardAtItsOwnLatency) {
  const auto restarted_onto = [](std::uint64_t before, std::uint64_t after) {
    HeardClock clock = one_run(before);  // [0, 10 ms) from kHost
    clock = after_render(clock, took(kPts + 10 * kMs, kFrames),
                         at(kHost + 10 * kMs, false, before), kRate);
    return after_render(clock, took(kPts + 20 * kMs, kFrames),
                        at(kHost + 25 * kMs, true, after), kRate);
  };
  const HeardClock longer = restarted_onto(10 * kMs, 180 * kMs);
  ASSERT_TRUE(longer.previous.has_value());
  EXPECT_EQ(longer.previous->latency_ns, 10 * kMs);
  EXPECT_EQ(longer.current->latency_ns, 180 * kMs);
  EXPECT_EQ(heard_pts(longer, kHost + 25 * kMs), kPts + 15 * kMs);
  EXPECT_EQ(heard_pts(longer, kHost + 26 * kMs), kPts + 16 * kMs);
  EXPECT_EQ(heard_pts(longer, kHost + 100 * kMs), kPts + 20 * kMs) << "held";
  EXPECT_EQ(heard_pts(longer, kHost + 205 * kMs), kPts + 20 * kMs);
  EXPECT_EQ(heard_pts(longer, kHost + 210 * kMs), kPts + 25 * kMs);
  const HeardClock shorter = restarted_onto(180 * kMs, 10 * kMs);
  EXPECT_EQ(heard_pts(shorter, kHost + 34 * kMs), kPts) << "before's";
  EXPECT_EQ(heard_pts(shorter, kHost + 35 * kMs), kPts + 20 * kMs);
  for (const HeardClock& clock : {longer, shorter}) {
    std::uint64_t last = 0;
    for (std::uint64_t ms = 0; ms <= 300; ++ms) {
      const std::uint64_t heard = *heard_pts(clock, kHost + ms * kMs);
      EXPECT_GE(heard, last) << ms << " ms on";
      last = heard;
    }
  }
}

// After 5 ms of silence (an underrun), the next run's frames are heard 5 ms
// later; until its first is, the run before holds at its end.
TEST(HeardPts, ThePreviousRunPlaysUntilTheCurrentOnesFirstFrame) {
  RenderTime short_time = at(kHost);
  short_time.frames = kFrames + 40;  // 5 ms of it silence
  HeardClock clock =
      after_render(HeardClock{}, took(kPts, kFrames, true), short_time, kRate);
  clock = after_render(clock, took(kPts + 10 * kMs, kFrames),
                       at(kHost + 15 * kMs), kRate);
  ASSERT_TRUE(clock.previous.has_value());
  EXPECT_EQ(heard_pts(clock, kHost + 8 * kMs), kPts + 8 * kMs);
  EXPECT_EQ(heard_pts(clock, kHost + 12 * kMs), kPts + 10 * kMs);
  EXPECT_EQ(heard_pts(clock, kHost + 17 * kMs), kPts + 12 * kMs);
}

// The rate averages jitter out; the largest residual shows it.
TEST(ClockFit, IsTheSlopeOfTheReadingsAndTheFarthestOffIt) {
  std::vector<ClockSample> same;
  std::vector<ClockSample> fast;
  std::vector<ClockSample> jittered;
  for (std::int64_t i = 0; i < 100; ++i) {
    const std::int64_t t = 1'000'000'000'000 + i * 100'000'000;  // 100 ms
    same.push_back({t, t + 7});
    // 50 ppm fast.
    fast.push_back({t, 42 + static_cast<std::int64_t>(std::llround(
                                static_cast<double>(t) * 1.00005))});
    jittered.push_back({t, t + (i % 2 == 0 ? 1'000 : -1'000)});
  }
  EXPECT_DOUBLE_EQ(clock_fit(same)->rate, 1.0);
  EXPECT_NEAR(clock_fit(same)->max_residual_ns, 0.0, 1e-3);
  EXPECT_NEAR((clock_fit(fast)->rate - 1.0) * 1e6, 50.0, 1e-3);
  EXPECT_NEAR(clock_fit(fast)->max_residual_ns, 0.0, 1.0);
  EXPECT_NEAR((clock_fit(jittered)->rate - 1.0) * 1e6, 0.0, 0.1);
  // The line tilts toward the jitter by 0.006 ppm: 30 ns at the ends.
  EXPECT_NEAR(clock_fit(jittered)->max_residual_ns, 1'000.0, 50.0);
}

TEST(ClockFit, NoneForFewerThanTwoTimes) {
  EXPECT_FALSE(clock_fit({}).has_value());
  EXPECT_FALSE(clock_fit({{5, 5}}).has_value());
  EXPECT_FALSE(clock_fit({{5, 5}, {5, 9}}).has_value());
}

}  // namespace
