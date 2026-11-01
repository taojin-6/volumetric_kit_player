// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The live playout clock (live_clock.hpp) on given times, host only: present,
// earliest and pts_at exactly, a negative offset included; the release rule
// to the nanosecond, and with no lead as the take's gate; the mesh rule's
// cases, kMeshEvery to the microsecond; the point's step, rising at once and
// falling exactly kPointFall a second, never below its target. LiveClock:
// the offset is the least over the window, a sample exactly kLiveWindow old
// kept and one a nanosecond older gone, followed down at once and up at
// kPointFall a second, never more than kMaxLiveDelay below it; the arrival
// need raises the target at once by a late record's lateness, on top of the
// lead, and a join's burst adds nothing; arrival jitter is the lateness over
// the window, which expires, so the point then falls at kPointFall a second
// once the connection's first 10 s are past, and within them the offset's
// fall carries it at once while a fall of D is stepped; a fast record
// entering or leaving moves no steady record's present time later, nor
// earlier by more than the fall, at the arrival need and at a program's; a
// 1 s GOP arriving together at a join lands at the steady point; a program's
// need sets the target, a tie named arrival; the clamp at kMaxLiveDelay; the
// lead is the largest over the window; point() is time()'s point after each
// write; a spike, a record late past the clamp and the backlog draining
// behind it, a large record in it included, raises no D and is flagged, a
// record exactly at the clamp counting and one a nanosecond past a spike's;
// a lasting slower link still raises D, within the clamp at once, past it
// once the offset brings its records within, and after a spike once no
// record from before it is left; repeated spikes cost their records late,
// never D; reset forgets everything; and after a warm-up, ten thousand records
// and publishes allocate nothing. While audio plays the point slews, up
// kSlewRise a kSlewEvery and at most that a step, down only once the target
// has not risen for kLiveWindow, kSlewFall a second and at most that a step,
// never past its target; a clock told of audio that does not play is the
// clock it was; audio's need sets the target when it is the largest, named
// audio and clamped; now_pts is what is heard while audio plays and is heard;
// and point() is read on another thread while the worker writes, under
// ThreadSanitizer.

#include "volumetric_kit/player/live_clock.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>

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
  EXPECT_EQ(vkp::step_point(ms(100), ms(250), nanoseconds(0)), ms(250));
  // Falls exactly kPointFall a second, never below its target.
  EXPECT_EQ(vkp::step_point(ms(250), ms(100), seconds(1)),
            ms(250) - vkp::kPointFall);
  EXPECT_EQ(vkp::step_point(ms(250), ms(100), milliseconds(40)), ms(249));
  EXPECT_EQ(vkp::step_point(ms(250), ms(100), milliseconds(500)), ms(237.5));
  EXPECT_EQ(vkp::step_point(ms(250), ms(100), seconds(100)), ms(100));
  EXPECT_EQ(vkp::step_point(ms(250), ms(100), seconds(3'600)), ms(100))
      << "no overflow";
  EXPECT_EQ(vkp::step_point(ms(250), ms(100), nanoseconds(0)), ms(250));
  EXPECT_EQ(vkp::kPointFall, ms(25));
}

TEST(LiveClockTimes, WhileAudioPlaysThePointRisesByStepsAndFallsOnceStable) {
  EXPECT_EQ(vkp::kSlewRise, ms(20));
  EXPECT_EQ(vkp::kSlewEvery, ms(100));
  EXPECT_EQ(vkp::kSlewFall, ms(10));
  const nanoseconds unstable(0);
  // Up: kSlewRise a kSlewEvery, at most kSlewRise a step, never past.
  EXPECT_EQ(vkp::slew_point(ms(100), ms(300), ms(40), unstable), ms(108));
  EXPECT_EQ(vkp::slew_point(ms(100), ms(300), ms(100), unstable), ms(120));
  EXPECT_EQ(vkp::slew_point(ms(100), ms(300), seconds(2), unstable), ms(120))
      << "a step at most, however long since the last";
  EXPECT_EQ(vkp::slew_point(ms(100), ms(105), ms(100), unstable), ms(105));
  EXPECT_EQ(vkp::slew_point(ms(100), ms(100), ms(100), unstable), ms(100));
  EXPECT_EQ(vkp::slew_point(ms(100), ms(300), nanoseconds(0), unstable),
            ms(100));
  EXPECT_EQ(vkp::slew_point(ms(100), ms(300), seconds(3'600), unstable),
            ms(120))
      << "no overflow";
  // Down: not until D's target has not risen for kLiveWindow; then
  // kSlewFall a second, at most kSlewFall a step, never past.
  const nanoseconds stable = vkp::kLiveWindow;
  EXPECT_EQ(
      vkp::slew_point(ms(300), ms(100), seconds(1), stable - nanoseconds(1)),
      ms(300));
  EXPECT_EQ(vkp::slew_point(ms(300), ms(100), seconds(1), stable), ms(290));
  EXPECT_EQ(vkp::slew_point(ms(300), ms(100), ms(40), stable), ms(299.6));
  EXPECT_EQ(vkp::slew_point(ms(300), ms(100), seconds(5), stable), ms(290))
      << "a step at most";
  EXPECT_EQ(vkp::slew_point(ms(105), ms(100), seconds(1), stable), ms(100));
}

TEST(LiveClockNames, EachNeedHasItsName) {
  EXPECT_STREQ(vkp::to_string(DelayNeed::kNone), "none");
  EXPECT_STREQ(vkp::to_string(DelayNeed::kArrival), "arrival");
  EXPECT_STREQ(vkp::to_string(DelayNeed::kProgram), "program");
  EXPECT_STREQ(vkp::to_string(DelayNeed::kAudio), "audio");
}

// Record `k` of a steady stream, its pts k periods from 0, arriving `late`
// after its steady time t0 + k periods.
void steady(LiveClock& clock, std::uint64_t k, nanoseconds late = {},
            nanoseconds program = {}) {
  clock.arrived(k * kPeriodNs,
                t0 + kPeriod * static_cast<std::int64_t>(k) + late, program);
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
  // The fastest record, 3 ms early, then two more; then one record seconds
  // later, so the offset may rise to the least of the rest at once.
  const auto first_three = [](LiveClock& clock) {
    steady(clock, 0, ms(-3));
    steady(clock, 1);
    steady(clock, 2, ms(2));
  };
  LiveClock clock;
  first_three(clock);
  const Playout playout = now_of(clock);
  EXPECT_EQ(playout.offset, t0.time_since_epoch() - ms(3));
  EXPECT_EQ(clock.now_pts(playout, t0), vkp::pts_at(playout.point, t0));
  // Exactly kLiveWindow after the fastest, it stays...
  clock.arrived(249 * kPeriodNs, t0 - ms(3) + vkp::kLiveWindow, nanoseconds(0));
  EXPECT_EQ(now_of(clock).offset, t0.time_since_epoch() - ms(3));
  // ...and a nanosecond later it is gone: the least of the rest.
  LiveClock later;
  first_three(later);
  later.arrived(249 * kPeriodNs, t0 - ms(3) + vkp::kLiveWindow + nanoseconds(1),
                nanoseconds(0));
  EXPECT_EQ(now_of(later).offset, t0.time_since_epoch());
}

// Past the join, the least rising by 30 ms: the offset follows at kPointFall
// a second, 1 ms a record, and at once down again; risen by 2 s, it is at
// most kMaxLiveDelay below it.
TEST(LiveClock, TheOffsetRisesAtKPointFallASecondNoFurtherThanTheClampBelow) {
  LiveClock clock;
  std::uint64_t k = 0;
  steady(clock, k++, ms(-30));  // the fastest, alone
  for (; k < 250; ++k) {
    steady(clock, k);
  }
  ASSERT_EQ(now_of(clock).offset, t0.time_since_epoch() - ms(30));
  // Record 250 comes 10.03 s after the fastest, which leaves: the offset
  // rises 1 ms a record from there.
  for (; k < 260; ++k) {
    steady(clock, k);
  }
  EXPECT_EQ(now_of(clock).offset, t0.time_since_epoch() - ms(20));
  for (; k < 300; ++k) {
    steady(clock, k);
  }
  EXPECT_EQ(now_of(clock).offset, t0.time_since_epoch()) << "never past it";
  steady(clock, k++, ms(-12));
  EXPECT_EQ(now_of(clock).offset, t0.time_since_epoch() - ms(12))
      << "down at once";
  // Every record 2 s later than its pts says, from record 500: once the last
  // before it has left, at record 700, the offset is kMaxLiveDelay below the
  // least, and rises from there.
  for (; k < 500; ++k) {
    steady(clock, k);
  }
  for (; k < 700; ++k) {
    steady(clock, k, seconds(2));
  }
  ASSERT_EQ(now_of(clock).offset, t0.time_since_epoch());
  for (; k <= 700; ++k) {
    steady(clock, k, seconds(2));
  }
  EXPECT_EQ(now_of(clock).offset,
            t0.time_since_epoch() + seconds(2) - vkp::kMaxLiveDelay);
  steady(clock, k++, seconds(2));
  EXPECT_EQ(now_of(clock).offset, t0.time_since_epoch() + seconds(1) + ms(1));
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
  EXPECT_EQ(
      vkp::release_wait(pts, clock.now_pts(playout, release), playout.lead),
      nanoseconds(0));
  EXPECT_EQ(
      vkp::release_wait(pts, clock.now_pts(playout, release - nanoseconds(1)),
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

// A program's need of 120 ms, steady past the join: a record 20 ms fast
// enters the window, then leaves it 10 s later. The point never moves more
// than kPointFall a second, 1 ms a record, either way, and ends where it
// was.
TEST(LiveClock, AFastRecordMovesAProgramsPointNoFasterThanTheFall) {
  LiveClock clock;
  const nanoseconds need = ms(120);
  std::uint64_t k = 0;
  for (; k < 300; ++k) {
    steady(clock, k, {}, need);
  }
  const nanoseconds steady_point = now_of(clock).point;
  ASSERT_EQ(steady_point, t0.time_since_epoch() + need);
  steady(clock, k++, ms(-20), need);
  nanoseconds before = now_of(clock).point;
  // The most either way: the fall over the 60 ms after the fast record.
  const nanoseconds most = ms(1.5);
  for (int i = 0; i < 400; ++i, ++k) {
    steady(clock, k, {}, need);
    const nanoseconds point = now_of(clock).point;
    ASSERT_LE(point - before, most) << "record " << k << ": a jump later";
    ASSERT_GE(point - before, -most) << "record " << k << ": a jump earlier";
    ASSERT_EQ(now_of(clock).need, DelayNeed::kProgram) << "record " << k;
    before = point;
  }
  EXPECT_EQ(now_of(clock).point, steady_point);
}

// Within the join's first kLiveWindow, a fall of the offset carries the point
// at once, while a program's need falling from 150 to 50 ms is stepped at
// kPointFall a second, so the records it held come due one by one.
TEST(LiveClock, SettlingTheOffsetsFallIsAtOnceAndDsFallIsStepped) {
  LiveClock clock;
  steady(clock, 0, {}, ms(150));
  ASSERT_EQ(now_of(clock).delay(), ms(150));
  steady(clock, 1, {}, ms(50));
  EXPECT_EQ(now_of(clock).target, ms(50));
  EXPECT_EQ(now_of(clock).delay(), ms(149)) << "1 ms a record";
  // Record 3 (none 2), 40 ms fast, so 40 ms after record 1: the offset
  // falls 40.
  steady(clock, 3, ms(-40), ms(50));
  EXPECT_EQ(now_of(clock).offset, t0.time_since_epoch() - ms(40));
  EXPECT_EQ(now_of(clock).delay(), ms(148))
      << "the offset's fall at once, D's stepped";
  for (std::uint64_t k = 4; k < 104; ++k) {
    steady(clock, k, ms(-40), ms(50));
  }
  EXPECT_EQ(now_of(clock).delay(), ms(50));
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
    clock.arrived(k * kPeriodNs,
                  t0 + kPeriod * static_cast<std::int64_t>(k - 24),
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

// The worker's lead in the spike tests: each record's publish takes it.
constexpr nanoseconds kLead = ms(20);

// Record `k`'s steady arrival: its time t0 + k periods, plus its jitter,
// k % 7 ms, so 6 ms the largest.
Clock::time_point steady_at(std::uint64_t k) {
  return t0 + kPeriod * static_cast<std::int64_t>(k) +
         ms(static_cast<double>(k % 7));
}

// A path that delivers in order, as TCP does: record k arrives `extra` after
// its steady arrival, but no sooner than `spacing` after the record before,
// the time its bytes take; each one's publish takes kLead.
class Path {
 public:
  explicit Path(LiveClock& clock) : clock_(clock) {}

  // Record k; its arrival.
  Clock::time_point send(std::uint64_t k, nanoseconds extra = {},
                         nanoseconds spacing = ms(5)) {
    Clock::time_point at = steady_at(k) + extra;
    if (last_) {
      at = std::max(at, *last_ + spacing);
    }
    last_ = at;
    clock_.arrived(k * kPeriodNs, at, nanoseconds(0));
    clock_.published(kLead, at);
    return at;
  }

 private:
  LiveClock& clock_;
  std::optional<Clock::time_point> last_;
};

// Whether record `k`, arrived at `at`, is released for its present time:
// on time.
bool on_time(const LiveClock& clock, std::uint64_t k, Clock::time_point at) {
  const Playout playout = now_of(clock);
  return vkp::present(playout.point, k * kPeriodNs) - playout.lead >= at;
}

// r2b's spike: every record sent in 2 s from 20 s comes 1.2 s late, so the
// records sent in the 1.2 s after drain behind them, 5 ms apart, one of them
// a large record of 120 ms. D never rises: N, the time from the drain's end
// until D is back, is 0. Without the rule D sat at the clamp for 10 s, then
// fell 25 ms a second. The spike's records are late, as at any D; from the
// drain's end every record is on time. Mutants: no record past the clamp a
// spike's (D 1 s); the drain counted (D 975 ms).
TEST(LiveClockSpike, ASpikePastTheClampAndItsDrainLeaveDWhereItWas) {
  LiveClock clock;
  Path path(clock);
  std::uint64_t k = 0;
  for (; k < 500; ++k) {
    path.send(k);
  }
  const nanoseconds before = now_of(clock).target;
  ASSERT_EQ(before, ms(26)) << "the jitter's 6 ms and the lead";
  for (; k < 1'100; ++k) {
    const Clock::time_point at = path.send(
        k, k < 550 ? ms(1'200) : nanoseconds(0), k == 570 ? ms(120) : ms(5));
    const Playout playout = now_of(clock);
    ASSERT_LE(playout.target, before) << "record " << k;
    ASSERT_LE(playout.delay(), before) << "record " << k;
    if (at - steady_at(k) + kLead > vkp::kMaxLiveDelay) {
      EXPECT_FALSE(on_time(clock, k, at)) << "record " << k;
      EXPECT_TRUE(playout.clamped) << "record " << k;
    }
    if (k >= 600) {
      ASSERT_TRUE(on_time(clock, k, at)) << "record " << k;
      ASSERT_FALSE(playout.clamped) << "record " << k;
    }
  }
}

// The clamp's edge, past the join: a record whose lateness plus the lead is
// kMaxLiveDelay exactly is covered and raises D to it, as ever; a
// nanosecond later it is a spike's and D stays. Mutant: >= for >.
TEST(LiveClockSpike, ARecordAtTheClampCountsANanosecondPastIsASpikes) {
  for (const nanoseconds past : {nanoseconds(0), nanoseconds(1)}) {
    SCOPED_TRACE(past.count());
    LiveClock clock;
    Path path(clock);
    std::uint64_t k = 0;
    for (; k < 300; ++k) {
      path.send(k);
    }
    // Record 300's jitter is 6 ms.
    const Clock::time_point at =
        path.send(k, vkp::kMaxLiveDelay - kLead - ms(6) + past);
    const Playout playout = now_of(clock);
    EXPECT_EQ(playout.target, past.count() == 0 ? vkp::kMaxLiveDelay : ms(26));
    EXPECT_EQ(playout.delay(), playout.target);
    EXPECT_EQ(playout.clamped, past.count() != 0);
    EXPECT_EQ(on_time(clock, k, at), past.count() == 0);
  }
}

// A link that stays slower raises D (latency gives). By 500 ms, within the
// clamp: at once, every record on time. By 1.2 s, past it: its records are
// late until the offset, risen once the window holds them alone, brings them
// within the clamp, 10.8 s on, as the clamp alone did; by 12 s, every record
// on time at D ≥ 900 ms. A spike that leaves the link 300 ms slower: D rises
// once the window holds no record from before it, by 10 s on, and every
// record is on time. Mutant: a spike that never ends with no steady record
// left.
TEST(LiveClockSpike, ALastingSlowerLinkStillRaisesD) {
  const auto run = [](nanoseconds extra, std::uint64_t spike_until,
                      std::uint64_t on_time_from, nanoseconds least_d) {
    LiveClock clock;
    Path path(clock);
    std::uint64_t k = 0;
    for (; k < 500; ++k) {
      path.send(k);
    }
    for (; k < 1'100; ++k) {
      const Clock::time_point at =
          path.send(k, k < spike_until ? ms(1'200) : extra);
      if (k >= on_time_from) {
        ASSERT_TRUE(on_time(clock, k, at)) << "record " << k;
      }
      if (k == on_time_from) {
        EXPECT_GE(now_of(clock).delay(), least_d);
      }
    }
  };
  {
    SCOPED_TRACE("500 ms slower");
    run(ms(500), 0, 500, ms(520));
  }
  {
    SCOPED_TRACE("1.2 s slower");
    run(ms(1'200), 0, 800, ms(900));
  }
  {
    SCOPED_TRACE("a spike, then 300 ms slower");
    run(ms(300), 550, 750, ms(300));
  }
}

// Spikes 6 s apart, each as r2b's: D stays, and each costs its own records,
// late at any D, while every record between the drains and after the last is
// on time. Repeated spikes cost late frames, held or skipped, each time,
// never latency.
TEST(LiveClockSpike, RepeatedSpikesCostLateRecordsNotLatency) {
  LiveClock clock;
  Path path(clock);
  std::uint64_t k = 0;
  for (; k < 500; ++k) {
    path.send(k);
  }
  const nanoseconds before = now_of(clock).target;
  for (; k < 1'100; ++k) {
    const bool spike = (k >= 500 && k < 550) || (k >= 650 && k < 700);
    const Clock::time_point at =
        path.send(k, spike ? ms(1'200) : nanoseconds(0));
    ASSERT_LE(now_of(clock).target, before) << "record " << k;
    if (spike) {
      EXPECT_FALSE(on_time(clock, k, at)) << "record " << k;
    }
    if ((k >= 600 && k < 650) || k >= 750) {
      EXPECT_TRUE(on_time(clock, k, at)) << "record " << k;
    }
  }
}

// What a test says is heard (LiveClock::set_audio_master).
class Heard final : public vkp::AudioMaster {
 public:
  std::optional<std::int64_t> heard_pts(
      Clock::time_point) const noexcept override {
    return pts;
  }
  std::optional<std::int64_t> pts;
};

// While audio plays, a need rising 200 ms at once raises D's target at once
// and the applied D 8 ms a record (kSlewRise a kSlewEvery, at 25 Hz); its
// fall holds it until the target has not risen for kLiveWindow, then lowers
// it 0.4 ms a record (kSlewFall a second). Without audio, the same records
// step it: up at once, down 1 ms a record. Mutant: the slew bypassed.
TEST(LiveClock, WhileAudioPlaysTheAppliedDSlews) {
  for (const bool audio : {true, false}) {
    SCOPED_TRACE(audio ? "audio plays" : "no audio");
    LiveClock clock;
    clock.set_audio_playing(audio);
    std::uint64_t k = 0;
    steady(clock, k++);
    EXPECT_EQ(now_of(clock).delay(), nanoseconds(0));
    for (; k <= 30; ++k) {
      steady(clock, k, {}, ms(200));
      const Playout playout = now_of(clock);
      ASSERT_EQ(playout.target, ms(200)) << "record " << k;
      ASSERT_EQ(playout.need, DelayNeed::kProgram);
      const nanoseconds slewed = std::min(
          nanoseconds(8'000'000 * static_cast<std::int64_t>(k)), ms(200));
      ASSERT_EQ(playout.delay(), audio ? slewed : ms(200)) << "record " << k;
    }
    for (; k <= 300; ++k) {
      steady(clock, k);
      const Playout playout = now_of(clock);
      ASSERT_EQ(playout.target, nanoseconds(0)) << "record " << k;
      // The target last rose at record 1, kLiveWindow before record 251.
      const auto past = [](std::uint64_t from, std::uint64_t k) {
        return static_cast<std::int64_t>(k > from ? k - from : 0);
      };
      const nanoseconds want =
          audio ? ms(200) - nanoseconds(400'000 * past(250, k))
                : std::max(ms(200) - ms(1) * past(30, k), nanoseconds(0));
      ASSERT_EQ(playout.delay(), want) << "record " << k;
    }
  }
}

// A clock told of audio that does not play, as a live stream with no Audio
// track, is the clock it was: the same playout at every write, over jittered
// records with a program's needs and publishes, and the picture's now.
TEST(LiveClock, WithNoAudioPlayingItIsTheClockItWas) {
  Heard heard;
  heard.pts = 123;
  LiveClock plain;
  LiveClock told;
  told.set_audio_master(&heard);
  for (std::uint64_t k = 0; k < 600; ++k) {
    const nanoseconds late = ms(static_cast<double>(k % 7 * 3));
    const nanoseconds program = ms(static_cast<double>(k / 50 % 3 * 40));
    const Clock::time_point at =
        t0 + kPeriod * static_cast<std::int64_t>(k) + late;
    plain.arrived(k * kPeriodNs, at, program);
    told.arrived(k * kPeriodNs, at, program, nanoseconds(0));
    plain.published(ms(static_cast<double>(k % 4)), at + ms(5));
    told.published(ms(static_cast<double>(k % 4)), at + ms(5));
    const Playout a = now_of(plain);
    const Playout b = now_of(told);
    ASSERT_EQ(a.offset, b.offset) << "record " << k;
    ASSERT_EQ(a.point, b.point) << "record " << k;
    ASSERT_EQ(a.target, b.target) << "record " << k;
    ASSERT_EQ(a.need, b.need) << "record " << k;
    ASSERT_EQ(a.clamped, b.clamped) << "record " << k;
    ASSERT_EQ(a.lead, b.lead) << "record " << k;
    ASSERT_EQ(told.now_pts(b, at), plain.now_pts(a, at)) << "record " << k;
  }
  EXPECT_FALSE(told.audio_playing());
}

// Audio's need sets the target as any need does: named audio when it is the
// largest, a larger program's winning, and clamped at kMaxLiveDelay.
TEST(LiveClock, AudiosNeedSetsTheTargetWhenItIsTheLargest) {
  LiveClock clock;
  clock.arrived(0, t0, ms(30), ms(80));
  EXPECT_EQ(now_of(clock).target, ms(80));
  EXPECT_EQ(now_of(clock).need, DelayNeed::kAudio);
  clock.arrived(kPeriodNs, t0 + kPeriod, ms(100), ms(80));
  EXPECT_EQ(now_of(clock).target, ms(100));
  EXPECT_EQ(now_of(clock).need, DelayNeed::kProgram);
  clock.arrived(2 * kPeriodNs, t0 + 2 * kPeriod, ms(100), ms(1'200));
  EXPECT_EQ(now_of(clock).target, vkp::kMaxLiveDelay);
  EXPECT_TRUE(now_of(clock).clamped);
  EXPECT_EQ(now_of(clock).need, DelayNeed::kAudio);
}

// now_pts is what is heard while audio plays and is heard in step; else
// the picture's. A reset ends the audio's play.
TEST(LiveClock, WhileAudioPlaysNowIsWhatIsHeard) {
  LiveClock clock;
  steady(clock, 0);
  const Playout playout = now_of(clock);
  const std::int64_t picture = vkp::pts_at(playout.point, t0);
  Heard heard;
  heard.pts = picture + 7'000'000;
  EXPECT_EQ(clock.now_pts(playout, t0), picture) << "no master";
  clock.set_audio_master(&heard);
  EXPECT_EQ(clock.now_pts(playout, t0), picture) << "not playing";
  clock.set_audio_playing(true);
  EXPECT_TRUE(clock.audio_playing());
  EXPECT_EQ(clock.now_pts(playout, t0), picture + 7'000'000);
  heard.pts.reset();
  EXPECT_EQ(clock.now_pts(playout, t0), picture) << "nothing heard in step";
  clock.reset();
  EXPECT_FALSE(clock.audio_playing());
}

// point() read on another thread, as an output's render thread reads it,
// while the worker writes with audio playing: under ThreadSanitizer no race,
// and every point read is one the clock could hold, never a torn value.
TEST(LiveClock, PointIsReadOnAnotherThreadWhileTheWorkerWrites) {
  LiveClock clock;
  clock.set_audio_playing(true);
  std::atomic<bool> done{false};
  std::atomic<std::uint64_t> reads{0};
  std::atomic<std::uint64_t> strays{0};
  const nanoseconds low = t0.time_since_epoch() - ms(50);
  const nanoseconds high = t0.time_since_epoch() + seconds(2);
  std::thread reader([&] {
    while (!done.load()) {
      if (const std::optional<nanoseconds> point = clock.point()) {
        if (*point < low || *point > high) {
          ++strays;
        }
      }
      ++reads;
    }
  });
  for (std::uint64_t k = 0; k < 5'000; ++k) {
    steady(clock, k, ms(static_cast<double>(k % 5)),
           ms(k % 100 < 50 ? 120 : 0));
    clock.published(ms(3), t0 + kPeriod * static_cast<std::int64_t>(k));
  }
  done = true;
  reader.join();
  EXPECT_GT(reads.load(), 0u);
  EXPECT_EQ(strays.load(), 0u);
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
