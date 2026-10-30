// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// A live stream's audio on the live clock (live_audio.hpp), on the host with
// synthetic records: no device, no sound. Audio's need is a geometry period,
// the output's latency and the margin, at least the floor, plus what
// underruns raised it, at most the ceiling, never below the base; the drift
// trim is a step past its band each way. With no track it plays nothing,
// states no need and makes no output; an output of the track's format is
// kept across connections and one of another made; a maker's failure leaves
// no track; the gain and a park reach an output made later. A connection's
// first record is heard at its present time, and its end drops what the
// output holds, so an unpark plays nothing stale and the position is none.
// Simulated, on a manual clock: geometry and audio records muxed by pts,
// with network jitter, the sender's clock, its mic and the viewer's output
// each up to 100 ppm off, and a 200 ms stall: the audio is heard within its
// band of the clock before the stall and again soon after it, following the
// clock's point in moves of kFollowBand or more, few a minute, the applied D
// rising by at most kSlewRise a kSlewEvery while audio plays and falling
// back slowly, the stall's underruns raising audio's need; an output
// 1000 ppm fast or slow is trimmed back into the band, D never moving;
// underruns raise the need a step a run of them, up to its ceiling, and it
// falls a step each kLiveWindow without; a join's settling fall past
// kAudioReach places the audio afresh; and through a route of more latency
// than the clamp the audio is heard late by the excess, still the master on
// the clock's pts. On threads, under ThreadSanitizer: what is heard, the
// need and the stats are read while the worker feeds the audio and the null
// output renders on its own thread.

#include "volumetric_kit/player/live_audio.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "fake_audio_device.hpp"
#include "manual_clock.hpp"
#include "volumetric_kit/grid/stream/audio.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace stream = volumetric_kit::grid::stream;
namespace vkp = volumetric_kit::player;
using namespace std::chrono_literals;
using std::chrono::milliseconds;
using std::chrono::nanoseconds;
using Clock = vkp::Playback::Clock;
using vkp::AudioOut;
using vkp::AudioTrackChoice;
using vkp::DelayNeed;
using vkp::LiveAudio;
using vkp::LiveClock;
using vkp::NullAudioOut;
using vkp::Playout;

constexpr std::uint32_t kRate = 48000;
constexpr std::size_t kChunk = 960;  // 20 ms
constexpr nanoseconds kChunkTime = 20ms;
constexpr nanoseconds kPeriod = 40ms;  // the geometry's, 25 a second
constexpr std::uint64_t kPts0 = 50'000'000'000;

AudioTrackChoice mono_track() {
  AudioTrackChoice track;
  track.index = 1;
  track.params.channels = 1;
  track.params.sample_rate = kRate;
  return track;
}

// A chunk of a 440 Hz tone, s16 little-endian.
std::vector<std::uint8_t> tone() {
  std::vector<std::uint8_t> pcm;
  for (std::size_t i = 0; i < kChunk; ++i) {
    const auto sample = static_cast<std::int16_t>(
        8192.0 * std::sin(2.0 * 3.14159265358979 * 440.0 *
                          static_cast<double>(i) / kRate));
    const auto bits = static_cast<std::uint16_t>(sample);
    pcm.push_back(static_cast<std::uint8_t>(bits & 0xff));
    pcm.push_back(static_cast<std::uint8_t>(bits >> 8));
  }
  return pcm;
}

TEST(AudioNeed, APeriodTheLatencyAndTheMarginAtLeastTheFloor) {
  EXPECT_EQ(vkp::kAudioMargin, 20ms);
  EXPECT_EQ(vkp::kAudioNeedLeast, 80ms);
  EXPECT_EQ(vkp::kAudioNeedMost, 300ms);
  EXPECT_EQ(vkp::audio_need(40ms, 0ms, 0ms), 80ms) << "60 ms, floored";
  EXPECT_EQ(vkp::audio_need(40ms, 30ms, 0ms), 90ms);
  EXPECT_EQ(vkp::audio_need(40ms, 0ms, 20ms), 100ms) << "a raise above it";
  EXPECT_EQ(vkp::audio_need(40ms, 30ms, 40ms), 130ms);
  EXPECT_EQ(vkp::audio_need(40ms, 0ms, 1000ms), 300ms) << "the ceiling";
  EXPECT_EQ(vkp::audio_need(40ms, 200ms, 100ms), 300ms);
  EXPECT_EQ(vkp::audio_need(40ms, 1200ms, 100ms), 1260ms)
      << "never below the base";
}

TEST(DriftTrim, AStepPastItsBandEachWay) {
  EXPECT_EQ(vkp::kTrimBand, 20ms);
  EXPECT_EQ(vkp::kTrimStep, 10ms);
  EXPECT_EQ(vkp::drift_trim(0ms), 0ms);
  EXPECT_EQ(vkp::drift_trim(20ms), 0ms);
  EXPECT_EQ(vkp::drift_trim(-20ms), 0ms);
  EXPECT_EQ(vkp::drift_trim(20ms + 1ns), 10ms) << "heard early: an insert";
  EXPECT_EQ(vkp::drift_trim(-20ms - 1ns), -10ms) << "heard late: a drop";
  EXPECT_EQ(vkp::drift_trim(500ms), 10ms);
}

// The outputs a maker made, a null output each on the test's clock, ticked
// by the test.
struct Outputs {
  vkp::AudioOutMaker maker(NullAudioOut::Clock clock) {
    return [this, clock](const vkp::AudioFormat& format)
               -> core::Result<std::unique_ptr<AudioOut>> {
      if (fail) {
        return core::Status::io_error("no output today");
      }
      VKC_ASSIGN(
          std::unique_ptr<NullAudioOut> out,
          NullAudioOut::create(format, clock, NullAudioOut::Ticks::kByCaller));
      made.push_back(out.get());
      return std::unique_ptr<AudioOut>(std::move(out));
    };
  }
  std::vector<NullAudioOut*> made;
  bool fail = false;
};

// A manual clock's reading, as a NullAudioOut::Clock and LiveAudio::Now.
std::function<Clock::time_point()> reader_of(const vp_test::ManualClock& now) {
  return [&now] { return now(); };
}

// A maker of device outputs over a fake device (fake_audio_device.hpp),
// which keeps the last it made in `made`, for the test to render.
vkp::AudioOutMaker device_maker(std::shared_ptr<vp_test::FakeDeviceState> state,
                                vkp::DeviceAudioOut** made) {
  return [state, made](const vkp::AudioFormat& format)
             -> core::Result<std::unique_ptr<AudioOut>> {
    VKC_ASSIGN(
        std::unique_ptr<vkp::DeviceAudioOut> out,
        vkp::DeviceAudioOut::create(format, vp_test::fake_device(state)));
    *made = out.get();
    return std::unique_ptr<AudioOut>(std::move(out));
  };
}

// The samples of `samples` that are not silence.
std::ptrdiff_t sounded(const std::vector<float>& samples) {
  return std::count_if(samples.begin(), samples.end(),
                       [](float sample) { return sample != 0.0f; });
}

TEST(LiveAudio, WithNoTrackItPlaysNothingAndStatesNoNeed) {
  vp_test::ManualClock now;
  Outputs outputs;
  LiveClock clock;
  LiveAudio audio(clock, outputs.maker(reader_of(now)), reader_of(now));
  ASSERT_TRUE(audio.begin(std::nullopt, kPeriod).ok());
  clock.arrived(kPts0, now(), 0ns, audio.need());
  const std::vector<std::uint8_t> pcm = tone();
  stream::RecordInfo info;
  info.track = 1;
  info.pts_ns = kPts0;
  audio.arrived(info, pcm.data(), pcm.size(), now());
  audio.look(now());
  EXPECT_TRUE(outputs.made.empty());
  EXPECT_EQ(audio.need(), 0ns);
  EXPECT_FALSE(audio.playing());
  EXPECT_FALSE(audio.heard_pts(now()).has_value());
  EXPECT_FALSE(audio.position().has_value());
  EXPECT_EQ(audio.stats().records, 0u);
  EXPECT_FALSE(clock.audio_playing());
}

// Across connections an output of the track's format is kept and one of
// another made; a maker that fails leaves no track; the gain and a park set
// before reach an output made later.
TEST(LiveAudio, AnOutputOfTheFormatIsKeptAndAnotherMade) {
  vp_test::ManualClock now;
  Outputs outputs;
  LiveClock clock;
  LiveAudio audio(clock, outputs.maker(reader_of(now)), reader_of(now));
  audio.set_gain(0.0f);
  audio.park();
  ASSERT_TRUE(audio.begin(mono_track(), kPeriod).ok());
  ASSERT_EQ(outputs.made.size(), 1u);
  EXPECT_EQ(outputs.made[0]->gain(), 0.0f) << "muted before";
  EXPECT_TRUE(outputs.made[0]->playing()) << "live, it always plays";
  EXPECT_EQ(audio.need(), 80ms);
  audio.end();
  EXPECT_EQ(audio.need(), 0ns);
  ASSERT_TRUE(audio.begin(mono_track(), kPeriod).ok());
  EXPECT_EQ(outputs.made.size(), 1u) << "the same format: kept";
  AudioTrackChoice stereo = mono_track();
  stereo.params.channels = 6;  // played as two
  ASSERT_TRUE(audio.begin(stereo, kPeriod).ok());
  ASSERT_EQ(outputs.made.size(), 2u);
  EXPECT_EQ(outputs.made[1]->format().channels, 2u);
  outputs.fail = true;
  EXPECT_FALSE(audio.begin(mono_track(), kPeriod).ok());
  EXPECT_EQ(audio.need(), 0ns) << "no track";
  audio.unpark();
}

// The connection's first record is placed so its first frame is heard at
// its present time: pts + offset + D.
TEST(LiveAudio, AConnectionsFirstRecordIsHeardAtItsPresentTime) {
  vp_test::ManualClock now;
  Outputs outputs;
  LiveClock clock;
  LiveAudio audio(clock, outputs.maker(reader_of(now)), reader_of(now));
  ASSERT_TRUE(audio.begin(mono_track(), kPeriod).ok());
  NullAudioOut& out = *outputs.made.at(0);
  clock.arrived(kPts0, now(), 0ns, audio.need());
  const Playout playout = clock.time().value();
  ASSERT_EQ(playout.delay(), 80ms);
  ASSERT_EQ(playout.need, DelayNeed::kAudio);
  const std::vector<std::uint8_t> pcm = tone();
  stream::RecordInfo info;
  info.track = 1;
  info.key = true;
  info.pts_ns = kPts0;
  audio.arrived(info, pcm.data(), pcm.size(), now());
  out.tick();  // takes the flush, with no time passed
  EXPECT_EQ(audio.position(), kPts0 - 80'000'000);
  EXPECT_EQ(out.queued(), kChunk * 5) << "80 ms of silence, then the chunk";
  now.advance(80ms);
  out.tick();
  EXPECT_EQ(audio.position(), kPts0) << "its first frame heard now";
  EXPECT_EQ(Clock::time_point(now()), vkp::present(playout.point, kPts0));
  EXPECT_EQ(audio.stats().placements, 1u);
  EXPECT_EQ(audio.stats().records, 1u);
}

// A connection's end drops what the output holds: a parked device output
// keeps its ring and an unpark starts it at once, so it would play the last
// connection's audio stale. Its position is then none. Mutant: an end that
// leaves the output's frames.
TEST(LiveAudio, AnEndDropsWhatTheOutputHoldsSoAnUnparkPlaysNothingStale) {
  auto state = std::make_shared<vp_test::FakeDeviceState>();
  vkp::DeviceAudioOut* device = nullptr;
  LiveClock clock;
  LiveAudio audio(clock, device_maker(state, &device));
  ASSERT_TRUE(audio.begin(mono_track(), kPeriod).ok());
  ASSERT_NE(device, nullptr);
  const Clock::time_point at = Clock::now();
  clock.arrived(kPts0, at, 0ns, audio.need());
  const std::vector<std::uint8_t> pcm = tone();
  for (std::uint64_t i = 0; i < 3; ++i) {
    stream::RecordInfo info;
    info.track = 1;
    info.key = true;
    info.pts_ns = kPts0 + i * 20'000'000;
    audio.arrived(info, pcm.data(), pcm.size(), at);
  }
  // 70 ms of silence before them (D less the route's 10 ms), then 10 ms of
  // the tone; 50 ms of it left.
  ASSERT_GT(sounded(vp_test::render(*device, kChunk * 4, state->host_ns)), 0);
  ASSERT_TRUE(audio.position().has_value());
  audio.park();
  audio.end();
  audio.unpark();
  EXPECT_EQ(sounded(vp_test::render(*device, kChunk * 10, state->host_ns)), 0)
      << "nothing of the last connection";
  EXPECT_FALSE(audio.position().has_value()) << "none without a track";
}

// A sender and a viewer, simulated on the test's clock in 1 ms steps: the
// sender's geometry 25 a second and its audio in 20 ms chunks, muxed by pts
// (each chunk right after the first frame at or past its pts) and sent over
// a network of 25 ms with jitter; the viewer feeds the clock and the audio,
// looks every 5 ms and renders the output every 10 ms.
struct Scenario {
  double sender_ppm = 0;  // the sender's clock, its pts, against the viewer's
  double mic_ppm = 0;     // the mic's chunks against true time
  double output_ppm = 0;  // the viewer's output against its steady clock
  nanoseconds jitter{0};  // the network's, up to
  std::vector<nanoseconds> stalls;  // 200 ms each, from these times
  nanoseconds slow_start{0};        // more latency for the first second
  nanoseconds length = 60s;
};

// A follow (LiveAudio::look): when, and how far the point it followed had
// moved since the audio was placed or last followed it.
struct Follow {
  nanoseconds at;  // since the start
  nanoseconds moved;
};

// One sample, after each render of the output.
struct Sample {
  nanoseconds at;        // since the start
  Playout playout;       // the clock's
  nanoseconds ahead;     // what is heard less the clock's now
  bool playing = false;  // the audio plays in step
  bool master = false;   // now_pts is what is heard
  nanoseconds need;      // audio's
};

class Sim {
 public:
  explicit Sim(const Scenario& scenario)
      : scenario_(scenario),
        audio_(clock_, outputs_.maker([this] { return output_clock_(); }),
               [this] { return now_(); }),
        pcm_(tone()) {
    output_clock_.set(start_);
    clock_.set_audio_master(&audio_);
    build();
  }

  // Run it all, calling `each` after each render.
  void run(const std::function<void(const Sample&)>& each) {
    ASSERT_TRUE(audio_.begin(mono_track(), kPeriod).ok());
    ASSERT_EQ(outputs_.made.size(), 1u);
    NullAudioOut& out = *outputs_.made[0];
    std::size_t next = 0;
    const auto steps = static_cast<std::size_t>(scenario_.length / 1ms);
    for (std::size_t step = 0; step < steps; ++step, now_.advance(1ms)) {
      output_clock_.set(start_ + drifted(since_start()));
      const Clock::time_point now = now_();
      for (; next < events_.size() && events_[next].at <= since_start();
           ++next) {
        deliver(events_[next], now);
      }
      if (step % 5 == 0) {
        const std::uint64_t before = audio_.stats().follows;
        audio_.look(now);
        if (audio_.stats().follows > before) {
          const nanoseconds point = followed();
          follows_.push_back({since_start(), point - followed_});
          followed_ = point;
        }
      }
      if (step % 10 == 0) {
        out.tick();
        each(sample(now));
      }
    }
  }

  const LiveAudio& audio() const { return audio_; }
  const LiveClock& clock() const { return clock_; }
  std::uint64_t chunks_delivered() const { return chunks_; }
  const std::vector<Follow>& follows() const { return follows_; }

 private:
  struct Event {
    nanoseconds at;  // since the start
    bool geometry = false;
    std::uint64_t pts = 0;
    std::uint64_t seq = 0;
  };

  // A clock a test sets, for the output's.
  struct SetClock {
    Clock::time_point operator()() const {
      return Clock::time_point(nanoseconds(ns.load()));
    }
    void set(Clock::time_point at) { ns = at.time_since_epoch().count(); }
    std::atomic<std::int64_t> ns{0};
  };

  nanoseconds since_start() const { return now_() - start_; }
  // The point the audio follows: the clock's, plus any lateness.
  nanoseconds followed() const {
    return clock_.point().value_or(0ns) + audio_.late();
  }
  nanoseconds drifted(nanoseconds since) const {
    return since + nanoseconds(static_cast<std::int64_t>(
                       static_cast<double>(since.count()) *
                       scenario_.output_ppm * 1e-6));
  }
  std::uint64_t pts_of(nanoseconds captured) const {
    return kPts0 +
           static_cast<std::uint64_t>(static_cast<double>(captured.count()) *
                                      (1.0 + scenario_.sender_ppm * 1e-6));
  }
  // A deterministic jitter for record `i`, in [0, scenario_.jitter].
  nanoseconds jitter_of(std::uint64_t i) const {
    const std::uint64_t mixed = (i * 2654435761u) % 1000;
    return nanoseconds(scenario_.jitter.count() *
                       static_cast<std::int64_t>(mixed) / 1000);
  }

  void build() {
    const std::int64_t frames = scenario_.length / kPeriod + 1;
    std::uint64_t chunk = 0;
    nanoseconds last{0};
    for (std::int64_t n = 0; n < frames; ++n) {
      const nanoseconds captured = kPeriod * n;
      const std::uint64_t pts = pts_of(captured);
      nanoseconds at =
          captured + 25ms + jitter_of(static_cast<std::uint64_t>(n));
      if (captured < 1s) {
        at += scenario_.slow_start;
      }
      for (const nanoseconds stall : scenario_.stalls) {
        if (at >= stall && at < stall + 200ms) {
          at = stall + 200ms;
        }
      }
      at = std::max(at, last);  // TCP keeps the order
      last = at;
      events_.push_back({at, true, pts, static_cast<std::uint64_t>(n)});
      // The chunks muxed behind it: every one at or before its pts.
      for (;; ++chunk) {
        const nanoseconds chunk_captured(static_cast<std::int64_t>(
            static_cast<double>(
                (kChunkTime * static_cast<std::int64_t>(chunk)).count()) *
            (1.0 + scenario_.mic_ppm * 1e-6)));
        const std::uint64_t chunk_pts = pts_of(chunk_captured);
        if (chunk_pts > pts) {
          break;
        }
        events_.push_back({at, false, chunk_pts, chunk});
      }
    }
  }

  void deliver(const Event& event, Clock::time_point now) {
    if (event.geometry) {
      clock_.arrived(event.pts, now, 0ns, audio_.need());
      return;
    }
    stream::RecordInfo info;
    info.track = 1;
    info.key = true;
    info.pts_ns = event.pts;
    info.seq = event.seq;
    const std::uint64_t placements = audio_.stats().placements;
    audio_.arrived(info, pcm_.data(), pcm_.size(), now);
    if (audio_.stats().placements > placements) {
      followed_ = followed();  // placed at the point as it is
    }
    ++chunks_;
  }

  Sample sample(Clock::time_point now) const {
    Sample s;
    s.at = since_start();
    s.playout = clock_.time().value_or(Playout{});
    s.playing = audio_.playing();
    s.need = audio_.need();
    if (const std::optional<std::uint64_t> heard = audio_.position()) {
      s.ahead = nanoseconds(static_cast<std::int64_t>(*heard) -
                            vkp::pts_at(s.playout.point, now));
    }
    if (const std::optional<std::int64_t> heard = audio_.heard_pts(now)) {
      s.master = clock_.now_pts(s.playout, now) == *heard;
    }
    return s;
  }

  Scenario scenario_;
  vp_test::ManualClock now_;
  SetClock output_clock_;
  const Clock::time_point start_ = now_();
  Outputs outputs_;
  LiveClock clock_;
  LiveAudio audio_;
  std::vector<std::uint8_t> pcm_;
  std::vector<Event> events_;
  std::uint64_t chunks_ = 0;
  nanoseconds followed_{0};  // the point as placed or last followed
  std::vector<Follow> follows_;
};

TEST(LiveAudioSim, JitterDriftAndAStallStayInStep) {
  for (const double ppm : {100.0, -100.0}) {
    SCOPED_TRACE(ppm);
    Scenario scenario;
    scenario.sender_ppm = ppm;
    scenario.mic_ppm = -ppm;
    scenario.output_ppm = -ppm;
    scenario.jitter = 8ms;
    scenario.stalls = {20s};
    Sim sim(scenario);
    nanoseconds worst_before{0};
    nanoseconds worst_after{0};
    std::optional<Sample> last;
    nanoseconds peak_target{0};
    nanoseconds final_delay{0};
    std::size_t in_step = 0;
    std::size_t mastered = 0;
    sim.run([&](const Sample& s) {
      const nanoseconds error = s.ahead < 0ns ? -s.ahead : s.ahead;
      if (s.at >= 2s && s.at < 20s) {
        worst_before = std::max(worst_before, error);
        in_step += s.playing ? 1 : 0;
        mastered += s.master ? 1 : 0;
      }
      if (s.at >= 23s) {
        worst_after = std::max(worst_after, error);
        in_step += s.playing ? 1 : 0;
      }
      if (s.at >= 20s && s.at < 21s) {
        peak_target = std::max(peak_target, s.playout.target);
      }
      // While audio plays, the applied D moves at most a step between two
      // renders, as one record's update may come between them.
      if (last && last->playing && s.playing) {
        const nanoseconds rise = s.playout.delay() - last->playout.delay();
        EXPECT_LE(rise, vkp::kSlewRise) << "at " << s.at.count();
        EXPECT_GE(rise, -vkp::kSlewFall) << "at " << s.at.count();
      }
      last = s;
      final_delay = s.playout.delay();
    });
    EXPECT_LE(worst_before, vkp::kTrimBand + 2ms);
    EXPECT_LE(worst_after, vkp::kTrimBand + 2ms);
    EXPECT_GT(in_step, 5'000u) << "in step nearly throughout";
    EXPECT_GT(mastered, 1'500u) << "what is heard the master";
    // The stall less the network's 25 ms, which the offset holds.
    EXPECT_GE(peak_target, 170ms) << "the target rose at once";
    EXPECT_GE(sim.audio().stats().underruns, 1u);
    EXPECT_GE(sim.audio().stats().placements, 2u) << "placed afresh";
    const double final_ms =
        std::chrono::duration<double, std::milli>(final_delay).count();
    EXPECT_NEAR(final_ms, 80.0, 2.0)
        << "D back at audio's need once the stall's lateness and raise expire";
    const vkp::LiveAudioStats stats = sim.audio().stats();
    EXPECT_EQ(stats.records + stats.dropped, sim.chunks_delivered());
    // The point's slight moves (the slew at each write, the offset's drift)
    // add up: each follow is at least kFollowBand, less a frame's rounding,
    // and few come a minute, none in the steady run before the stall. D's
    // fall after it is a 10 ms drop a second.
    std::size_t steady = 0;
    for (const Follow& follow : sim.follows()) {
      const nanoseconds moved =
          follow.moved < 0ns ? -follow.moved : follow.moved;
      EXPECT_GE(moved, vkp::kFollowBand - 50us) << "at " << follow.at.count();
      steady += follow.at >= 2s && follow.at < 20s ? 1 : 0;
    }
    EXPECT_EQ(steady, 0u);
    EXPECT_LE(sim.follows().size(), 12u);
    EXPECT_EQ(stats.follows, sim.follows().size());
  }
}

// Mutant: the drift trim moving D (a need raised, or the clock's point).
TEST(LiveAudioSim, TheDriftTrimHoldsTheErrorInItsBandWithoutTouchingD) {
  for (const double ppm : {1000.0, -1000.0}) {
    SCOPED_TRACE(ppm);
    Scenario scenario;
    scenario.output_ppm = ppm;
    Sim sim(scenario);
    nanoseconds worst{0};
    sim.run([&](const Sample& s) {
      if (s.at < 1s) {
        return;
      }
      worst = std::max(worst, s.ahead < 0ns ? -s.ahead : s.ahead);
      ASSERT_EQ(s.playout.target, 80ms) << "at " << s.at.count();
      ASSERT_EQ(s.playout.delay(), 80ms) << "at " << s.at.count();
      ASSERT_EQ(s.playout.need, DelayNeed::kAudio);
      ASSERT_TRUE(s.playing) << "at " << s.at.count();
    });
    EXPECT_LE(worst, vkp::kTrimBand + 2ms);
    // 1 ms a second over 59 s: a 10 ms step each 10 ms past the band.
    EXPECT_GE(sim.audio().stats().trims, 4u);
    EXPECT_EQ(sim.audio().stats().placements, 1u);
    EXPECT_EQ(sim.audio().stats().underruns, 0u);
  }
}

// Each stall's run of underruns raises audio's need a step, once; it falls
// a step each kLiveWindow without, and the clock's target follows it once
// the stalls' lateness has left its window.
TEST(LiveAudioSim, UnderrunsRaiseTheNeedAStepARunAndItFallsBack) {
  Scenario scenario;
  scenario.stalls = {5s, 7s};
  scenario.length = 32s;
  Sim sim(scenario);
  std::optional<nanoseconds> before;
  std::optional<nanoseconds> raised;
  std::optional<Sample> settled;
  std::optional<Sample> fallen;
  sim.run([&](const Sample& s) {
    if (s.at == 4s) {
      before = s.need;
    } else if (s.at == 8s) {
      raised = s.need;
    } else if (s.at == 20s) {
      settled = s;
    } else if (s.at == 31s) {
      fallen = s;
    }
  });
  ASSERT_TRUE(before && raised && settled && fallen);
  EXPECT_EQ(*before, 80ms);
  EXPECT_EQ(*raised, 80ms + 2 * vkp::kAudioRaise) << "a step a stall";
  EXPECT_EQ(sim.audio().stats().underruns, 2u);
  EXPECT_EQ(settled->need, 80ms + vkp::kAudioRaise) << "a step down";
  EXPECT_EQ(settled->playout.target, settled->need);
  EXPECT_EQ(settled->playout.need, DelayNeed::kAudio);
  EXPECT_EQ(fallen->need, 80ms);
  EXPECT_EQ(fallen->playout.target, 80ms);
}

// A join's settling: the network 125 ms faster after the first second, so
// the offset falls at once and carries the point past kAudioReach; the
// audio is placed afresh, in step again.
TEST(LiveAudioSim, ASettlingFallPastTheReachPlacesTheAudioAfresh) {
  Scenario scenario;
  scenario.slow_start = 125ms;
  scenario.length = 5s;
  Sim sim(scenario);
  nanoseconds worst{0};
  sim.run([&](const Sample& s) {
    if (s.at >= 2s) {
      worst = std::max(worst, s.ahead < 0ns ? -s.ahead : s.ahead);
    }
  });
  EXPECT_EQ(sim.audio().stats().placements, 2u);
  EXPECT_LE(worst, vkp::kTrimBand + 2ms);
  EXPECT_TRUE(sim.audio().playing());
}

// A route of more latency than D's clamp: audio's need passes it, the
// clock clamps D, and the audio is heard late by the excess, which
// heard_pts adds back onto the clock's pts, so what is heard is still the
// master. Simulated on the test's clock, which is the fake device's too:
// geometry 25 a second and the audio in 20 ms chunks, each arriving as its
// pts comes; a look every 5 ms and a render every 10 ms. Mutant: heard_pts
// without the lateness.
TEST(LiveAudio, ARoutePastTheClampLeavesTheAudioLateByTheExcess) {
  auto state = std::make_shared<vp_test::FakeDeviceState>();
  state->route.presentation_latency_s = 1.2;
  vp_test::ManualClock now;
  const auto host = [&] {
    state->host_ns =
        static_cast<std::uint64_t>(now().time_since_epoch().count());
  };
  host();
  vkp::DeviceAudioOut* device = nullptr;
  LiveClock clock;
  LiveAudio audio(clock, device_maker(state, &device), reader_of(now));
  clock.set_audio_master(&audio);
  ASSERT_TRUE(audio.begin(mono_track(), kPeriod).ok());
  EXPECT_EQ(audio.need(), 1260ms);
  EXPECT_EQ(audio.late(), 260ms);
  const std::vector<std::uint8_t> pcm = tone();
  std::uint64_t chunk = 0;
  for (std::uint64_t ms = 0; ms <= 1500; ++ms, now.advance(1ms)) {
    host();
    if (ms % 40 == 0) {
      const std::uint64_t pts = kPts0 + ms * 1'000'000;
      clock.arrived(pts, now(), 0ns, audio.need());
      for (; kPts0 + chunk * 20'000'000 <= pts; ++chunk) {
        stream::RecordInfo info;
        info.track = 1;
        info.key = true;
        info.pts_ns = kPts0 + chunk * 20'000'000;
        info.seq = chunk;
        audio.arrived(info, pcm.data(), pcm.size(), now());
      }
    }
    if (ms % 5 == 0) {
      audio.look(now());
    }
    if (ms % 10 == 0) {
      vp_test::render(*device, kChunk / 2, state->host_ns);
    }
  }
  host();
  const Playout playout = clock.time().value();
  EXPECT_EQ(playout.target, vkp::kMaxLiveDelay);
  EXPECT_TRUE(playout.clamped);
  EXPECT_EQ(playout.need, DelayNeed::kAudio);
  // What is heard runs late() behind the picture's now ...
  const std::int64_t picture = vkp::pts_at(playout.point, now());
  const std::optional<std::uint64_t> heard = audio.position();
  ASSERT_TRUE(heard.has_value());
  EXPECT_NEAR(static_cast<double>(static_cast<std::int64_t>(*heard) - picture),
              -260e6, 1e6);
  // ... and on the clock's pts it is the master.
  EXPECT_TRUE(audio.playing());
  const std::optional<std::int64_t> master = audio.heard_pts(now());
  ASSERT_TRUE(master.has_value()) << "not heard in step";
  EXPECT_LE(std::abs(*master - picture), vkp::kTrimBand.count());
  EXPECT_EQ(clock.now_pts(playout, now()), *master);
  EXPECT_EQ(audio.stats().placements, 1u);
  EXPECT_EQ(audio.stats().underruns, 0u);
  audio.end();
  EXPECT_EQ(audio.late(), 0ns);
}

// Under ThreadSanitizer: the take's and a stats reader's reads of what is
// heard, the need and the stats, while the worker feeds the audio and the
// null output renders on its own thread, all on steady_clock.
TEST(LiveAudio, WhatIsHeardIsReadOnOtherThreadsWhileTheWorkerFeedsIt) {
  LiveClock clock;
  LiveAudio audio(clock);
  clock.set_audio_master(&audio);
  ASSERT_TRUE(audio.begin(mono_track(), kPeriod).ok());
  std::atomic<bool> done{false};
  std::atomic<std::uint64_t> reads{0};
  std::thread reader([&] {
    while (!done.load()) {
      const Clock::time_point now = Clock::now();
      if (const std::optional<Playout> playout = clock.time()) {
        (void)clock.now_pts(*playout, now);
      }
      (void)audio.heard_pts(now);
      (void)audio.need();
      (void)audio.stats();
      (void)audio.position();
      ++reads;
      std::this_thread::sleep_for(1ms);
    }
  });
  const std::vector<std::uint8_t> pcm = tone();
  const Clock::time_point start = Clock::now();
  std::uint64_t chunk = 0;
  bool played = false;
  for (std::uint64_t n = 0; Clock::now() - start < 1500ms; ++n) {
    const std::uint64_t pts = kPts0 + n * 40'000'000;
    clock.arrived(pts, Clock::now(), 0ns, audio.need());
    for (; kPts0 + chunk * 20'000'000 <= pts; ++chunk) {
      stream::RecordInfo info;
      info.track = 1;
      info.key = true;
      info.pts_ns = kPts0 + chunk * 20'000'000;
      info.seq = chunk;
      audio.arrived(info, pcm.data(), pcm.size(), Clock::now());
    }
    for (int i = 0; i < 8; ++i) {
      audio.look(Clock::now());
      played = played || audio.playing();
      std::this_thread::sleep_until(
          start + 40ms * static_cast<std::int64_t>(n) + 5ms * (i + 1));
    }
  }
  done = true;
  reader.join();
  EXPECT_GT(reads.load(), 0u);
  EXPECT_GT(audio.stats().records, 0u);
  EXPECT_TRUE(played) << "in step on steady_clock";
}

}  // namespace
