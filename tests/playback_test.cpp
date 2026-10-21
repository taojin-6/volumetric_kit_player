// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/playback.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <utility>

namespace {

using std::chrono::milliseconds;
using std::chrono::nanoseconds;
using std::chrono::seconds;
using volumetric_kit::player::kFallbackPeriod;
using volumetric_kit::player::nominal_period;
using volumetric_kit::player::Playback;
using TimePoint = Playback::Clock::time_point;

constexpr std::uint64_t ms(std::uint64_t n) { return n * 1'000'000; }

const TimePoint t0{seconds(100)};

TEST(Playback, TheFirstFrameIsDueAtOnce) {
  Playback clock(30, 1);
  EXPECT_TRUE(clock.playing());
  EXPECT_EQ(clock.due(ms(500), t0), t0);
}

TEST(Playback, DueTimesFollowPts) {
  Playback clock(30, 1);
  clock.shown(ms(1000), t0);
  EXPECT_EQ(clock.due(ms(1040), t0), t0 + milliseconds(40));
  clock.shown(ms(1040), t0 + milliseconds(40));
  EXPECT_EQ(clock.due(ms(1100), t0), t0 + milliseconds(100));
}

TEST(Playback, AFrameLateByUnderAPeriodKeepsTheSchedule) {
  Playback clock(30, 1);
  clock.shown(0, t0);
  clock.shown(ms(33), t0 + milliseconds(50));
  EXPECT_EQ(clock.due(ms(66), t0), t0 + milliseconds(66));
}

TEST(Playback, AFrameExactlyAPeriodLateKeepsTheSchedule) {
  Playback clock(25, 1);
  clock.shown(0, t0);
  clock.shown(ms(40), t0 + milliseconds(80));
  EXPECT_EQ(clock.due(ms(80), t0), t0 + milliseconds(80));
}

TEST(Playback, AFrameOverAPeriodLateReanchors) {
  Playback clock(30, 1);
  clock.shown(0, t0);
  clock.shown(ms(33), t0 + milliseconds(73));
  EXPECT_EQ(clock.due(ms(66), t0), t0 + milliseconds(106));
}

TEST(Playback, PauseFreezes) {
  Playback clock(30, 1);
  clock.shown(0, t0);
  clock.pause();
  EXPECT_FALSE(clock.playing());
  EXPECT_FALSE(clock.due(ms(33), t0 + milliseconds(33)).has_value());
  EXPECT_FALSE(clock.due(ms(33), t0 + seconds(3600)).has_value());
}

TEST(Playback, ResumeDoesNotJump) {
  Playback clock(30, 1);
  clock.shown(0, t0);
  clock.pause();
  clock.play();
  const TimePoint resumed = t0 + seconds(10);
  EXPECT_EQ(clock.due(ms(33), resumed), resumed);
  clock.shown(ms(33), resumed);
  EXPECT_EQ(clock.due(ms(66), resumed), resumed + milliseconds(33));
}

TEST(Playback, AStepReleasesExactlyOneFrame) {
  Playback clock(30, 1);
  clock.shown(0, t0);
  clock.step();
  EXPECT_FALSE(clock.playing());
  const TimePoint t1 = t0 + seconds(1);
  EXPECT_EQ(clock.due(ms(33), t1), t1);
  clock.shown(ms(33), t1);
  EXPECT_FALSE(clock.due(ms(66), t1 + seconds(1)).has_value());

  clock.step();
  clock.step();
  clock.shown(ms(66), t1);
  EXPECT_TRUE(clock.due(ms(100), t1).has_value());
  clock.shown(ms(100), t1);
  EXPECT_FALSE(clock.due(ms(133), t1).has_value());
}

TEST(Playback, PlayWhilePlayingKeepsTheSchedule) {
  Playback clock(30, 1);
  clock.shown(0, t0);
  clock.play();
  EXPECT_EQ(clock.due(ms(33), t0 + milliseconds(5)), t0 + milliseconds(33));
}

TEST(Playback, PlayDropsStepsNotTaken) {
  Playback clock(30, 1);
  clock.step();
  clock.step();
  clock.play();
  clock.pause();
  EXPECT_FALSE(clock.due(0, t0).has_value());
}

TEST(Playback, PtsThatDoNotIncreaseFallBackToTheNominalPeriod) {
  Playback clock(25, 1);
  clock.shown(ms(100), t0);
  EXPECT_EQ(clock.due(ms(100), t0), t0 + milliseconds(40));
  clock.shown(ms(100), t0 + milliseconds(40));
  EXPECT_EQ(clock.due(ms(50), t0), t0 + milliseconds(80));
}

TEST(Playback, AGapOverASecondFallsBackToTheNominalPeriod) {
  Playback clock(25, 1);
  clock.shown(0, t0);
  EXPECT_EQ(clock.due(ms(1000), t0), t0 + seconds(1));
  EXPECT_EQ(clock.due(ms(1000) + 1, t0), t0 + milliseconds(40));
  EXPECT_EQ(clock.due(UINT64_MAX, t0), t0 + milliseconds(40));
}

TEST(Playback, AnUnknownRateIsThirtyFramesASecond) {
  for (const auto& [num, den] : {std::pair{0u, 1u}, std::pair{30u, 0u}}) {
    Playback clock(num, den);
    clock.shown(7, t0);
    EXPECT_EQ(clock.due(7, t0), t0 + nanoseconds(33'333'333));
  }
}

TEST(Playback, AFractionalRateKeepsItsPeriod) {
  Playback clock(30000, 1001);
  clock.shown(0, t0);
  EXPECT_EQ(clock.due(0, t0), t0 + nanoseconds(33'366'666));
}

TEST(NominalPeriod, IsWholeNanosecondsOfAKnownRate) {
  EXPECT_EQ(nominal_period(25, 1), nanoseconds(40'000'000));
  EXPECT_EQ(nominal_period(30000, 1001), nanoseconds(33'366'666));
}

TEST(NominalPeriod, IsEmptyForAnUnknownRateWhichFallsBackToThirty) {
  EXPECT_FALSE(nominal_period(0, 1).has_value());
  EXPECT_FALSE(nominal_period(30, 0).has_value());
  EXPECT_EQ(kFallbackPeriod, nanoseconds(33'333'333));
}

TEST(Playback, ALoopRestartAnchorsAtTheFirstPts) {
  Playback clock(30, 1);
  clock.shown(ms(10), t0);
  clock.shown(ms(43), t0 + milliseconds(33));
  const TimePoint wrap = t0 + milliseconds(33) + nanoseconds(33'333'333);
  EXPECT_EQ(clock.due(ms(10), t0), wrap);
  clock.shown(ms(10), wrap);
  EXPECT_EQ(clock.due(ms(43), t0), wrap + milliseconds(33));
}

}  // namespace
