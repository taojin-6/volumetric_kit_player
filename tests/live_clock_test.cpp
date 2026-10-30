// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The live playout clock (live_clock.hpp) on given times, host only: present,
// earliest and pts_at exactly, a negative offset included; the release rule
// to the nanosecond, and with no lead as the take's gate; the mesh rule's
// cases, kMeshEvery to the microsecond; the point's step, rising at once and
// falling exactly kPointFall a second, never below its target, and at once
// while settling. LiveClock: the offset is the least over the window, a
// sample exactly kLiveWindow old kept and one a nanosecond older gone; the
// arrival need raises the target at once by a late record's lateness, on top
// of the lead, and a join's burst adds nothing; arrival jitter is the
// lateness over the window, which expires, so the point then falls at
// kPointFall a second once the connection's first 10 s are past, and at once
// within them; a fast record entering or leaving moves no steady record's
// present time later, nor earlier by more than the fall; a 1 s GOP arriving
// together at a join lands at the steady point; a program's need sets the
// target, a tie named arrival; the clamp at kMaxLiveDelay; the lead is the
// largest over the window; point() is time()'s point after each write; reset
// forgets everything; and after a warm-up, ten thousand records and
// publishes allocate nothing.

#include "volumetric_kit/player/live_clock.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

#include "test_allocation_failure.hpp"

namespace {

namespace vkp = volumetric_kit::player;
using std::chrono::milliseconds;
using std::chrono::nanoseconds;
using std::chrono::seconds;
using Clock = vkp::Playback::Clock;
using vkp::DelayNeed;
using vkp::LiveClock;
using vkp::Playout;

// A steady time, far from the clock's epoch.
const Clock::time_point t0{seconds(1'000)};
constexpr std::uint64_t kPeriodNs = 40'000'000;  // 25 records a second
constexpr nanoseconds kPeriod{kPeriodNs};

// `ms` as nanoseconds.
constexpr nanoseconds ms(double value) {
  return nanoseconds(static_cast<std::int64_t>(value * 1e6));
}

// The clock's state, which must exist.
Playout now_of(const LiveClock& clock) {
  const std::optional<Playout> playout = clock.time();
  EXPECT_TRUE(playout.has_value());
  return playout.value_or(Playout{});
}

TEST(LiveClockTimes, PresentEarliestAndPtsAtAreExact) {
  Playout playout;
  playout.offset = ms(-250);  // the sender's clock ahead of the receiver's
  playout.point = ms(-130);   // D 120 ms
  constexpr std::uint64_t pts = 5'000'000'000;
  EXPECT_EQ(vkp::present(playout.point, pts).time_since_epoch(),
            nanoseconds(5'000'000'000) + ms(-130));
  EXPECT_EQ(vkp::earliest(playout, pts).time_since_epoch(),
            nanoseconds(5'000'000'000) + ms(-250));
  EXPECT_EQ(vkp::present(playout.point, pts) - vkp::earliest(playout, pts),
            playout.delay());
  EXPECT_EQ(playout.delay(), ms(120));
  EXPECT_EQ(vkp::pts_at(playout.point, vkp::present(playout.point, pts)),
            static_cast<std::int64_t>(pts));
  // Before pts 0, signed.
  EXPECT_EQ(vkp::pts_at(ms(10), Clock::time_point(ms(4))), -6'000'000);
}

TEST(LiveClockTimes, TheReleaseRuleIsExactToTheNanosecond) {
  constexpr std::uint64_t pts = 2'000'000'000;
  const nanoseconds lead = ms(15);
  const std::int64_t due = 2'000'000'000 - 15'000'000;
  EXPECT_EQ(vkp::release_wait(pts, due - 1, lead), nanoseconds(1));
  EXPECT_EQ(vkp::release_wait(pts, due, lead), nanoseconds(0));
  EXPECT_EQ(vkp::release_wait(pts, due + 1, lead), nanoseconds(0));
  EXPECT_EQ(vkp::release_wait(pts, due - 40'000'000, lead), ms(40));
  // With no lead, the take's gate: due at the pts itself.
  EXPECT_EQ(vkp::release_wait(pts, 2'000'000'000 - 1, nanoseconds(0)),
            nanoseconds(1));
  EXPECT_EQ(vkp::release_wait(pts, 2'000'000'000, nanoseconds(0)),
            nanoseconds(0));
}

TEST(LiveClockTimes, TheMeshRule) {
  constexpr std::uint64_t next = 1'000'000'000;
  const std::int64_t due = 1'000'000'000;  // the next record is due now
  const nanoseconds lead(0);
  const std::optional<nanoseconds> recent = ms(5);
  EXPECT_FALSE(vkp::mesh_now(false, std::nullopt, std::nullopt, due, lead))
      << "not free";
  EXPECT_TRUE(vkp::mesh_now(true, std::nullopt, next, due, lead))
      << "the connection's first frame";
  EXPECT_FALSE(vkp::mesh_now(true, recent, next, due, lead)) << "next due";
  EXPECT_TRUE(vkp::mesh_now(true, recent, next, due - 1, lead))
      << "next not due";
  EXPECT_TRUE(vkp::mesh_now(true, recent, std::nullopt, due, lead))
      << "none held";
  EXPECT_FALSE(
      vkp::mesh_now(true, std::chrono::microseconds(99'999), next, due, lead));
  EXPECT_TRUE(vkp::mesh_now(true, vkp::kMeshEvery, next, due, lead));
  EXPECT_FALSE(vkp::mesh_now(false, vkp::kMeshEvery, next, due, lead));
}

TEST(LiveClockTimes, ThePointRisesAtOnceAndFallsAtKPointFallASecond) {
  // Rises at once, however little time passed.
  EXPECT_EQ(vkp::step_point(ms(100), ms(250), nanoseconds(0), false), ms(250));
  // Falls exactly kPointFall a second, never below its target.
  EXPECT_EQ(vkp::step_point(ms(250), ms(100), seconds(1), false),
            ms(250) - vkp::kPointFall);
  EXPECT_EQ(vkp::step_point(ms(250), ms(100), milliseconds(40), false),
            ms(249));
  EXPECT_EQ(vkp::step_point(ms(250), ms(100), milliseconds(500), false),
            ms(237.5));
  EXPECT_EQ(vkp::step_point(ms(250), ms(100), seconds(100), false), ms(100));
  EXPECT_EQ(vkp::step_point(ms(250), ms(100), seconds(3'600), false), ms(100))
      << "no overflow";
  // Settling, a fall applies at once.
  EXPECT_EQ(vkp::step_point(ms(250), ms(100), nanoseconds(0), true), ms(100));
  EXPECT_EQ(vkp::kPointFall, ms(25));
}

TEST(LiveClockNames, EachNeedHasItsName) {
  EXPECT_STREQ(vkp::to_string(DelayNeed::kNone), "none");
  EXPECT_STREQ(vkp::to_string(DelayNeed::kArrival), "arrival");
  EXPECT_STREQ(vkp::to_string(DelayNeed::kProgram), "program");
}

// Record `k` of a steady stream, its pts k periods from 0, arriving `late`
// after its steady time t0 + k periods.
void steady(LiveClock& clock, std::uint64_t k, nanoseconds late = {},
            nanoseconds program = {}) {
  clock.arrived(k * kPeriodNs, t0 + kPeriod * static_cast<std::int64_t>(k) +
                                   late,
                program);
}

TEST(LiveClock, NoneBeforeTheFirstRecordAndAfterAReset) {
  LiveClock clock;
  EXPECT_FALSE(clock.time().has_value());
  EXPECT_FALSE(clock.point().has_value());
  clock.published(ms(5), t0);  // a publish alone makes no clock
  EXPECT_FALSE(clock.time().has_value());
  steady(clock, 0);
  ASSERT_TRUE(clock.time().has_value());
  ASSERT_TRUE(clock.point().has_value());
  clock.reset();
  EXPECT_FALSE(clock.time().has_value());
  EXPECT_FALSE(clock.point().has_value());
  // It forgot the lead too.
  clock.arrived(0, t0, nanoseconds(0));
  EXPECT_EQ(now_of(clock).lead, nanoseconds(0));
  EXPECT_EQ(now_of(clock).need, DelayNeed::kNone);
  EXPECT_EQ(now_of(clock).delay(), nanoseconds(0)) << "no fixed floor";
}

TEST(LiveClock, TheOffsetIsTheLeastOverTheWindow) {
  LiveClock clock;
  steady(clock, 0, ms(-3));  // the fastest: 3 ms early
  steady(clock, 1);
  steady(clock, 2, ms(2));
  const Playout playout = now_of(clock);
  EXPECT_EQ(playout.offset, t0.time_since_epoch() - ms(3));
  EXPECT_EQ(clock.now_pts(playout, t0), vkp::pts_at(playout.point, t0));
  // Exactly kLiveWindow after the fastest, it stays...
  clock.arrived(249 * kPeriodNs, t0 - ms(3) + vkp::kLiveWindow,
                nanoseconds(0));
  EXPECT_EQ(now_of(clock).offset, t0.time_since_epoch() - ms(3));
  // ...and a nanosecond later it is gone: the least of the rest.
  clock.arrived(249 * kPeriodNs, t0 - ms(3) + vkp::kLiveWindow + nanoseconds(1),
                nanoseconds(0));
  EXPECT_EQ(now_of(clock).offset, t0.time_since_epoch());
}

TEST(LiveClock, ALateRecordRaisesTheTargetAtOnceByItsLateness) {
  LiveClock clock;
  for (std::uint64_t k = 0; k < 10; ++k) {
    steady(clock, k);
  }
  clock.published(ms(6), t0 + 10 * kPeriod);
  clock.published(ms(8), t0 + 10 * kPeriod);
  clock.published(ms(7), t0 + 10 * kPeriod);
  Playout playout = now_of(clock);
  EXPECT_EQ(playout.lead, ms(8)) << "the largest over the window";
  EXPECT_EQ(playout.target, ms(8)) << "on time: the lead alone";
  EXPECT_EQ(playout.need, DelayNeed::kArrival);
  steady(clock, 10, ms(37));  // jitter: 37 ms late
  playout = now_of(clock);
  EXPECT_EQ(playout.target, ms(45));
  EXPECT_EQ(playout.delay(), ms(45)) << "risen at once";
  EXPECT_EQ(playout.offset, t0.time_since_epoch());
  // A record at present − lead is released, one a nanosecond earlier not.
  const std::uint64_t pts = 11 * kPeriodNs;
  const Clock::time_point release =
      vkp::present(playout.point, pts) - playout.lead;
  EXPECT_EQ(vkp::release_wait(pts, clock.now_pts(playout, release),
                              playout.lead),
            nanoseconds(0));
  EXPECT_EQ(vkp::release_wait(pts,
                              clock.now_pts(playout, release - nanoseconds(1)),
                              playout.lead),
            nanoseconds(1));
}

TEST(LiveClock, AJoinsBurstAddsNothing) {
  // 25 records, pts 40 ms apart, arriving together: each is a new least.
  LiveClock clock;
  for (std::uint64_t k = 0; k < 25; ++k) {
    clock.arrived(k * kPeriodNs, t0, nanoseconds(0));
  }
  const Playout playout = now_of(clock);
  EXPECT_EQ(playout.target, nanoseconds(0));
  EXPECT_EQ(playout.need, DelayNeed::kNone);
  EXPECT_EQ(playout.offset, t0.time_since_epoch() - 24 * kPeriod);
  // The newest is presented as it came, the oldest a second before.
  EXPECT_EQ(vkp::present(playout.point, 24 * kPeriodNs), t0);
  EXPECT_EQ(vkp::present(playout.point, 0), t0 - 24 * kPeriod);
}

// Jitter a record of 60 ms, then steady records: the target holds for the
// window, then falls; after the connection's first kLiveWindow the point
// follows at kPointFall a second, and within it at once.
TEST(LiveClock, LatenessExpiresAndThePointFallsAt25MsASecondPastTheJoin) {
  LiveClock clock;
  steady(clock, 0);
  // Past the join's settling: a record late by 60 ms at 12 s.
  const std::uint64_t late_at = 300;  // 12 s
  for (std::uint64_t k = 1; k < late_at; ++k) {
    steady(clock, k);
  }
  steady(clock, late_at, ms(60));
  EXPECT_EQ(now_of(clock).delay(), ms(60));
  // Steady records while it is in the window, 10 s from its arrival: the
  // target stays.
  std::uint64_t k = late_at + 1;
  for (; k <= late_at + 251; ++k) {
    steady(clock, k);
    ASSERT_EQ(now_of(clock).target, ms(60)) << "record " << k;
  }
  // The next leaves it behind: the target falls to 0 at once, the applied D
  // by kPointFall a second, so 1 ms a record at 25 Hz.
  steady(clock, k);
  EXPECT_EQ(now_of(clock).target, nanoseconds(0));
  EXPECT_EQ(now_of(clock).delay(), ms(59));
  steady(clock, ++k);
  EXPECT_EQ(now_of(clock).delay(), ms(58));
  for (int i = 0; i < 100; ++i) {
    steady(clock, ++k);
  }
  EXPECT_EQ(now_of(clock).delay(), nanoseconds(0)) << "never below its target";
  // Within the first kLiveWindow, a fall is at once: a first record 60 ms
  // late, then one on time, which is the least found so far.
  LiveClock joining;
  steady(joining, 0, ms(60));
  EXPECT_EQ(now_of(joining).point, t0.time_since_epoch() + ms(60));
  steady(joining, 1);
  EXPECT_EQ(now_of(joining).offset, t0.time_since_epoch());
  EXPECT_EQ(now_of(joining).point, t0.time_since_epoch())
      << "at once: the first record was the least as it arrived";
  EXPECT_EQ(now_of(joining).delay(), nanoseconds(0));
}

// A record 30 ms fast enters the window, then leaves it 10 s later: the
// point is never later than the steady one, nor earlier by more than the fall
// over a record, and each steady record after it finds the target point
// (offset + target) where it was.
TEST(LiveClock, AFastRecordEnteringOrLeavingMovesNoPresentLater) {
  LiveClock clock;
  std::uint64_t k = 0;
  for (; k < 300; ++k) {  // past the join's settling
    steady(clock, k);
  }
  const auto target_point = [&clock] {
    const Playout playout = now_of(clock);
    return playout.offset + playout.target;
  };
  const nanoseconds steady_point = now_of(clock).point;
  ASSERT_EQ(target_point(), steady_point);
  // 30 ms fast: 10 ms after the record before.
  steady(clock, k, ms(-30));
  ++k;
  EXPECT_EQ(now_of(clock).offset, t0.time_since_epoch() - ms(30));
  EXPECT_LE(now_of(clock).point, steady_point);
  for (int i = 0; i < 300; ++i, ++k) {
    steady(clock, k);
    const nanoseconds point = now_of(clock).point;
    ASSERT_LE(point, steady_point) << "record " << k << ": later";
    ASSERT_GE(point, steady_point - ms(1)) << "record " << k;
    ASSERT_EQ(target_point(), steady_point) << "record " << k;
  }
  EXPECT_EQ(now_of(clock).offset, t0.time_since_epoch()) << "it left";
  EXPECT_EQ(now_of(clock).point, steady_point);
}

// At a join a 1 s GOP arrives together, then the stream runs steady at the
// newest record's pace: by the burst's end the point is the steady one.
TEST(LiveClock, AOneSecondGopAtAJoinLandsAtTheSteadyPoint) {
  LiveClock clock;
  for (std::uint64_t k = 0; k < 25; ++k) {
    clock.arrived(k * kPeriodNs, t0, nanoseconds(0));
  }
  const nanoseconds burst_end = now_of(clock).point;
  for (std::uint64_t k = 25; k < 100; ++k) {
    clock.arrived(k * kPeriodNs, t0 + kPeriod * static_cast<std::int64_t>(k - 24),
                  nanoseconds(0));
  }
  const Playout playout = now_of(clock);
  EXPECT_EQ(playout.point, burst_end);
  EXPECT_EQ(playout.point, playout.offset);
  EXPECT_EQ(vkp::present(playout.point, 99 * kPeriodNs),
            t0 + kPeriod * (99 - 24));
}

TEST(LiveClock, AProgramsNeedSetsTheTargetATieIsNamedArrival) {
  LiveClock clock;
  steady(clock, 0, {}, ms(120));
  EXPECT_EQ(now_of(clock).target, ms(120));
  EXPECT_EQ(now_of(clock).need, DelayNeed::kProgram);
  EXPECT_EQ(now_of(clock).delay(), ms(120));
  // Its answer at each record: it rises at once.
  steady(clock, 1, {}, ms(150));
  EXPECT_EQ(now_of(clock).delay(), ms(150));
  // A late record's need of 150 ties it: named arrival.
  steady(clock, 2, ms(150), ms(150));
  EXPECT_EQ(now_of(clock).target, ms(150));
  EXPECT_EQ(now_of(clock).need, DelayNeed::kArrival);
  // A need below 0 is none.
  LiveClock negative;
  steady(negative, 0, {}, ms(-5));
  EXPECT_EQ(now_of(negative).target, nanoseconds(0));
  EXPECT_EQ(now_of(negative).need, DelayNeed::kNone);
}

TEST(LiveClock, ANeedPastOneSecondIsClamped) {
  for (const std::int64_t us : {999'999, 1'000'000, 1'000'001}) {
    SCOPED_TRACE(us);
    const nanoseconds need = std::chrono::microseconds(us);
    LiveClock clock;
    steady(clock, 0, {}, need);
    const Playout playout = now_of(clock);
    EXPECT_EQ(playout.target, std::min(need, vkp::kMaxLiveDelay));
    EXPECT_EQ(playout.clamped, us > 1'000'000);
    EXPECT_EQ(playout.delay(), playout.target);
  }
}

TEST(LiveClock, TheLeadIsTheLargestOverTheWindow) {
  LiveClock clock;
  steady(clock, 0);
  clock.published(ms(20), t0);
  clock.published(ms(4), t0 + seconds(5));
  EXPECT_EQ(now_of(clock).lead, ms(20));
  // Leads expire as records do; the arrival need is the lead alone.
  clock.published(ms(4), t0 + vkp::kLiveWindow + nanoseconds(1));
  EXPECT_EQ(now_of(clock).lead, ms(4));
  EXPECT_EQ(now_of(clock).target, ms(4));
}

TEST(LiveClock, PointIsTimesPointAfterEachWrite) {
  LiveClock clock;
  for (std::uint64_t k = 0; k < 50; ++k) {
    steady(clock, k, ms(static_cast<double>(k % 7)), ms(k % 3 == 0 ? 30 : 0));
    ASSERT_EQ(clock.point(), clock.time()->point) << "record " << k;
    clock.published(ms(static_cast<double>(k % 5)),
                    t0 + kPeriod * static_cast<std::int64_t>(k) + ms(9));
    ASSERT_EQ(clock.point(), clock.time()->point) << "publish " << k;
  }
}

TEST(LiveClock, RecordsAndPublishesAllocateNothing) {
  LiveClock clock;
  std::uint64_t k = 0;
  // Warm-up: both rings full, as an hour's run keeps them.
  for (; k < 2 * vkp::kLiveSamples; ++k) {
    steady(clock, k, ms(static_cast<double>(k % 11)), ms(50));
    clock.published(ms(6), t0 + kPeriod * static_cast<std::int64_t>(k));
  }
  auto& allocations = vp_test::allocation_failure;
  allocations.largest = 0;
  allocations.measure = true;
  for (int i = 0; i < 10'000; ++i, ++k) {
    steady(clock, k, ms(static_cast<double>(k % 11)), ms(50));
    clock.published(ms(6), t0 + kPeriod * static_cast<std::int64_t>(k));
    (void)clock.time();
    (void)clock.point();
  }
  allocations.measure = false;
  EXPECT_EQ(allocations.largest, 0u) << "an allocation per record";
}

}  // namespace
