// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The audio ring and the outputs, on the host with synthetic frames. The
// ring rounds its capacity up to a power of two, keeps frames in order
// across its wrap, writes silence for a null source, refuses what it has no
// room for (an overrun) and gives what it holds (an underrun), skips to an
// index, and passes every frame in order from one thread to another. An
// output opens paused with no position; a flush places the next frame
// written at its pts, drops what was written before, and lands paused or
// with no time passed; paused, it renders silence and takes nothing, so the
// position holds; a short ring renders silence, counted as an underrun once
// flushed, the position stopping at its last frame; a render says what it
// took, from which pts, and whether it flushed; the gain scales what
// goes out, and at 0 the position runs on. The null output renders what its
// clock says played, carrying the remainder of a frame, at most a second a
// tick, on a manual clock by the caller's ticks or on steady_clock by its
// own thread, and refuses a rate of 0 or other than 1 or 2 channels; it is
// the platform's output off Apple, and on Apple under the tests' backstop,
// where the Apple output refuses. The conversions between frames and time
// round as stated and do not overflow.

#include "volumetric_kit/player/audio_out.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include "manual_clock.hpp"
#include "volumetric_kit/player/audio_ring.hpp"
#if defined(__APPLE__)
#include "volumetric_kit/player/device_audio_out.hpp"
#endif

namespace {

namespace core = volumetric_kit::core;
using namespace std::chrono_literals;
using volumetric_kit::player::audio_frames;
using volumetric_kit::player::audio_ns;
using volumetric_kit::player::AudioFormat;
using volumetric_kit::player::AudioOut;
using volumetric_kit::player::AudioRing;
using volumetric_kit::player::NullAudioOut;
using volumetric_kit::player::Rendered;
using vp_test::ManualClock;
using SteadyClock = std::chrono::steady_clock;

constexpr std::uint32_t kRate = 8000;            // so a frame is 125 us
constexpr std::uint64_t kStart = 1'000'000'000;  // a flush's pts

// `frames` mono frames counting up from `first`.
std::vector<float> ramp(float first, std::size_t frames) {
  std::vector<float> out(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    out[i] = first + static_cast<float>(i);
  }
  return out;
}

// An output a test renders itself, as a device's render thread would.
class TestOut final : public AudioOut {
 public:
  explicit TestOut(const AudioFormat& format) : AudioOut(format) {}
  using AudioOut::render;

  // The next `frames` frames rendered.
  std::vector<float> next(std::size_t frames) {
    std::vector<float> out(frames * format().channels, -99.0f);
    render(out.data(), frames);
    return out;
  }
};

TEST(AudioRing, ItsCapacityIsAPowerOfTwoAtLeastTheFramesAsked) {
  EXPECT_EQ(AudioRing(1, 1).capacity(), 1u);
  EXPECT_EQ(AudioRing(8000, 2).capacity(), 8192u);
  EXPECT_EQ(AudioRing(8192, 2).capacity(), 8192u);
  EXPECT_EQ(AudioRing(8000, 2).channels(), 2u);
}

TEST(AudioRing, FramesComeOutInOrderAcrossItsWrap) {
  AudioRing ring(8, 2);
  std::vector<float> out(16);
  float next = 0.0f;  // the next sample expected
  float value = 0.0f;
  for (int round = 0; round < 5; ++round) {
    const std::vector<float> in = ramp(value, 10);  // 5 stereo frames
    ASSERT_EQ(ring.write(in.data(), 5), 5u);
    value += 10.0f;
    ASSERT_EQ(ring.read(out.data(), 5), 5u);
    for (std::size_t i = 0; i < 10; ++i) {
      EXPECT_EQ(out[i], next++) << "round " << round;
    }
  }
  EXPECT_EQ(ring.written(), 25u);
  EXPECT_EQ(ring.read_index(), 25u);
}

TEST(AudioRing, ANullSourceWritesSilence) {
  AudioRing ring(4, 1);
  std::vector<float> out(4, 1.0f);
  ASSERT_EQ(ring.write(nullptr, 3), 3u);
  ASSERT_EQ(ring.read(out.data(), 4), 3u);
  EXPECT_EQ(out, (std::vector<float>{0.0f, 0.0f, 0.0f, 1.0f}));
}

TEST(AudioRing, AFullRingTakesWhatFitsAndAnEmptyOneGivesWhatItHas) {
  AudioRing ring(8, 1);
  const std::vector<float> in = ramp(0.0f, 12);
  EXPECT_EQ(ring.write(in.data(), 12), 8u) << "an overrun takes what fits";
  EXPECT_EQ(ring.write(in.data() + 8, 4), 0u);
  std::vector<float> out(12, -1.0f);
  EXPECT_EQ(ring.read(out.data(), 3), 3u);
  EXPECT_EQ(ring.write(in.data() + 8, 4), 3u) << "room for what was read";
  EXPECT_EQ(ring.read(out.data() + 3, 12), 8u) << "an underrun: what it has";
  EXPECT_EQ(out, (std::vector<float>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, -1}));
}

TEST(AudioRing, ASkipDropsTheFramesBeforeItsIndex) {
  AudioRing ring(8, 1);
  const std::vector<float> in = ramp(0.0f, 6);
  ASSERT_EQ(ring.write(in.data(), 6), 6u);
  ring.skip_to(4);
  ring.skip_to(2);  // behind: nothing
  std::vector<float> out(2);
  ASSERT_EQ(ring.read(out.data(), 8), 2u);
  EXPECT_EQ(out, (std::vector<float>{4.0f, 5.0f}));
}

// One thread writes a count, another reads it, through a ring much shorter
// than the count: every frame arrives, once and in order. Under
// ThreadSanitizer, the ring's handoff. The ring copies with memmove, which
// macOS's sanitizer does not watch, so the first frame of each batch also
// goes in a plain array beside the ring: written before the ring takes the
// batch and read after the ring gives that frame, so only the ring orders
// the two. One store a batch, as a loop of them is vectorized at -O2 into
// stores the sanitizer does not watch either.
TEST(AudioRing, OneThreadWritesAndAnotherReadsEveryFrameInOrder) {
  constexpr std::size_t kTotal = 1 << 18;  // exact in a float's 24 bits
  constexpr std::size_t kBatch = 64;
  AudioRing ring(256, 2);
  std::vector<std::size_t> firsts(kTotal / kBatch, 0);  // each batch's
  std::thread producer([&] {
    std::vector<float> frames(2 * kBatch);
    std::size_t sent = 0;
    while (sent < kTotal) {
      for (std::size_t i = 0; i < kBatch; ++i) {
        frames[2 * i] = static_cast<float>(sent + i);
        frames[2 * i + 1] = -static_cast<float>(sent + i);
      }
      firsts[sent / kBatch] = sent;
      std::size_t done = 0;
      while (done < kBatch) {
        done += ring.write(frames.data() + 2 * done, kBatch - done);
        if (done < kBatch) {
          std::this_thread::yield();
        }
      }
      sent += kBatch;
    }
  });
  std::vector<float> out(2 * 100);
  std::size_t got = 0;
  bool in_order = true;
  while (got < kTotal) {
    const std::size_t n = ring.read(out.data(), 100);
    for (std::size_t i = 0; i < n && in_order; ++i) {
      const std::size_t frame = got + i;
      in_order = out[2 * i] == static_cast<float>(frame) &&
                 out[2 * i + 1] == -static_cast<float>(frame) &&
                 (frame % kBatch != 0 || firsts[frame / kBatch] == frame);
    }
    got += n;
    if (n == 0) {
      std::this_thread::yield();
    }
  }
  producer.join();
  EXPECT_TRUE(in_order);
  EXPECT_EQ(ring.read_index(), kTotal);
}

TEST(AudioTime, FramesAndTimeRoundAsStatedWithoutOverflow) {
  EXPECT_EQ(audio_ns(8000, kRate), 1'000'000'000u);
  EXPECT_EQ(audio_ns(1, kRate), 125'000u);
  EXPECT_EQ(audio_ns(1, 48000), 20'833u) << "rounded down";
  EXPECT_EQ(audio_frames(125'000, kRate), 1u);
  EXPECT_EQ(audio_frames(62'500, kRate), 1u) << "half a frame rounds up";
  EXPECT_EQ(audio_frames(62'499, kRate), 0u);
  // A day at 384 kHz, past what one product of either would hold.
  constexpr std::uint64_t kDayNs = 86'400ull * 1'000'000'000ull;
  constexpr std::uint64_t kDayFrames = 86'400ull * 384'000ull;
  EXPECT_EQ(audio_frames(kDayNs, 384000), kDayFrames);
  EXPECT_EQ(audio_ns(kDayFrames, 384000), kDayNs);
}

TEST(AudioOut, ItOpensPausedWithNoPosition) {
  TestOut out({kRate, 1});
  EXPECT_FALSE(out.playing());
  EXPECT_EQ(out.gain(), 1.0f);
  EXPECT_FALSE(out.position().has_value());
  EXPECT_EQ(out.capacity(), 8192u) << "about a second";
}

TEST(AudioOut, AFlushPlacesTheNextFrameAndDropsWhatCameBefore) {
  TestOut out({kRate, 1});
  const std::vector<float> a = ramp(1.0f, 400);
  const std::vector<float> b = ramp(1000.0f, 50);
  out.flush(kStart);
  ASSERT_EQ(out.write(a.data(), 400), 400u);
  out.play();
  const std::vector<float> first = out.next(100);
  EXPECT_EQ(first, std::vector<float>(a.begin(), a.begin() + 100));
  EXPECT_EQ(out.position(), kStart + audio_ns(100, kRate));
  constexpr std::uint64_t kAgain = 7'000'000'000;
  out.flush(kAgain);
  EXPECT_EQ(out.queued(), 0u) << "what came before is dropped";
  ASSERT_EQ(out.write(b.data(), 50), 50u);
  EXPECT_EQ(out.queued(), 50u);
  const std::vector<float> second = out.next(100);
  std::vector<float> want = b;
  want.resize(100, 0.0f);
  EXPECT_EQ(second, want) << "the frames after the flush, then silence";
  EXPECT_EQ(out.position(), kAgain + audio_ns(50, kRate));
  EXPECT_EQ(out.stats().flushes, 2u);
  EXPECT_EQ(out.stats().played_frames, 150u);
  EXPECT_EQ(out.stats().underrun_frames, 50u);
}

TEST(AudioOut, AFlushLandsWhilePausedAndWithNoFramesRendered) {
  TestOut out({kRate, 1});
  const std::vector<float> a = ramp(1.0f, 10);
  out.flush(kStart);
  out.write(a.data(), 10);
  out.next(0);
  EXPECT_EQ(out.position(), kStart) << "paused, from the flush's pts";
  out.flush(kStart + 5);
  out.next(0);
  EXPECT_EQ(out.position(), kStart + 5);
}

// Mutant: a render that takes frames while paused moves the position.
TEST(AudioOut, PausedItRendersSilenceAndThePositionHolds) {
  TestOut out({kRate, 2});
  const std::vector<float> a = ramp(1.0f, 2 * 100);
  out.flush(kStart);
  out.write(a.data(), 100);
  out.play();
  out.next(10);
  const std::optional<std::uint64_t> at = out.position();
  out.pause();
  EXPECT_EQ(out.next(30), std::vector<float>(60, 0.0f));
  EXPECT_EQ(out.position(), at);
  EXPECT_EQ(out.queued(), 90u);
  EXPECT_EQ(out.stats().underrun_frames, 0u) << "a pause is no underrun";
  out.play();
  const std::vector<float> resumed = out.next(5);
  EXPECT_EQ(resumed, std::vector<float>(a.begin() + 20, a.begin() + 30))
      << "it resumes where it paused";
}

TEST(AudioOut, AShortRingRendersSilenceCountedAsAnUnderrun) {
  TestOut out({kRate, 1});
  out.play();
  out.next(40);
  EXPECT_EQ(out.stats().underrun_frames, 0u) << "not until flushed";
  const std::vector<float> a = ramp(1.0f, 100);
  out.flush(kStart);
  out.write(a.data(), 100);
  std::vector<float> want = a;
  want.resize(300, 0.0f);
  EXPECT_EQ(out.next(300), want);
  EXPECT_EQ(out.stats().underrun_frames, 200u);
  EXPECT_EQ(out.stats().underruns, 1u);
  EXPECT_EQ(out.position(), kStart + audio_ns(100, kRate))
      << "the position stops at the last frame";
  out.next(10);
  EXPECT_EQ(out.stats().underruns, 2u) << "a render short again";
}

// A device output's clock is built from what each render says it took.
TEST(AudioOut, ARenderSaysWhatItTookAndItsPts) {
  TestOut out({kRate, 1});
  std::vector<float> samples(100);
  out.play();
  Rendered took = out.render(samples.data(), 10);
  EXPECT_FALSE(took.based) << "nothing placed before a flush";
  const std::vector<float> a = ramp(1.0f, 30);
  out.flush(kStart);
  out.write(a.data(), 30);
  took = out.render(samples.data(), 20);
  EXPECT_TRUE(took.based);
  EXPECT_TRUE(took.flushed);
  EXPECT_EQ(took.frames, 20u);
  EXPECT_EQ(took.pts_ns, kStart);
  EXPECT_EQ(took.end_pts_ns, kStart + audio_ns(20, kRate));
  took = out.render(samples.data(), 20);
  EXPECT_FALSE(took.flushed);
  EXPECT_EQ(took.frames, 10u) << "short";
  EXPECT_EQ(took.pts_ns, kStart + audio_ns(20, kRate));
  EXPECT_EQ(took.end_pts_ns, kStart + audio_ns(30, kRate));
  out.pause();
  took = out.render(samples.data(), 20);
  EXPECT_EQ(took.frames, 0u);
  EXPECT_EQ(took.pts_ns, took.end_pts_ns);
}

// Off Apple, and on it under the tests' backstop (VP_NO_AUDIO_DEVICE, set
// here too for a run outside ctest), the platform's output is the null one:
// silent, routed nowhere. The Apple output refuses, touching no device.
TEST(AudioOut, TheDefaultUnderTheBackstopOrOffAppleIsTheNullOutput) {
  ASSERT_EQ(setenv("VP_NO_AUDIO_DEVICE", "1", 1), 0);
#if defined(__APPLE__)
  EXPECT_EQ(
      volumetric_kit::player::open_apple_audio({kRate, 1}).status().domain(),
      core::Status::Code::Unsupported);
#endif
  core::Result<std::unique_ptr<AudioOut>> out =
      AudioOut::open_default({kRate, 1});
  ASSERT_TRUE(out.ok()) << out.status().message();
  EXPECT_FALSE((*out)->route().audible);
  EXPECT_TRUE((*out)->route().name.empty());
  EXPECT_FALSE((*out)->take_pause_request());
}

TEST(AudioOut, AnOverrunTakesWhatFitsAndTheRestFollowsInOrder) {
  TestOut out({kRate, 1});
  const std::size_t capacity = out.capacity();
  const std::vector<float> a = ramp(1.0f, capacity + 100);
  out.flush(kStart);
  EXPECT_EQ(out.write(a.data(), capacity + 100), capacity);
  out.play();
  EXPECT_EQ(out.next(100), std::vector<float>(a.begin(), a.begin() + 100));
  EXPECT_EQ(out.write(a.data() + capacity, 100), 100u);
  out.next(capacity - 100);
  EXPECT_EQ(out.next(100), std::vector<float>(a.end() - 100, a.end()));
}

// Mute: at gain 0 the frames still go, so the position runs on.
TEST(AudioOut, TheGainScalesWhatGoesOutAndAtZeroThePositionRunsOn) {
  TestOut out({kRate, 1});
  const std::vector<float> a = ramp(2.0f, 20);
  out.flush(kStart);
  out.write(a.data(), 20);
  out.play();
  out.set_gain(0.5f);
  EXPECT_EQ(out.next(2), (std::vector<float>{1.0f, 1.5f}));
  out.set_gain(0.0f);
  EXPECT_EQ(out.next(8), std::vector<float>(8, 0.0f));
  EXPECT_EQ(out.position(), kStart + audio_ns(10, kRate));
  out.set_gain(1.0f);
  EXPECT_EQ(out.next(1), std::vector<float>{12.0f});
}

std::unique_ptr<NullAudioOut> by_caller(const ManualClock& clock,
                                        std::uint16_t channels = 1) {
  core::Result<std::unique_ptr<NullAudioOut>> out = NullAudioOut::create(
      {kRate, channels}, [&clock] { return clock(); },
      NullAudioOut::Ticks::kByCaller);
  EXPECT_TRUE(out.ok()) << out.status().message();
  return out.ok() ? std::move(out).value() : nullptr;
}

// The null output's position moves as its clock does, as frames go out.
TEST(NullAudioOut, ThePositionFollowsItsClock) {
  ManualClock clock;
  std::unique_ptr<NullAudioOut> out = by_caller(clock, 2);
  ASSERT_NE(out, nullptr);
  const std::vector<float> a(2 * 800, 0.25f);  // 100 ms
  out->flush(kStart);
  ASSERT_EQ(out->write(a.data(), 800), 800u);
  out->play();
  out->tick();
  EXPECT_EQ(out->position(), kStart) << "no time passed";
  clock.advance(25ms);
  out->tick();
  EXPECT_EQ(out->position(), kStart + 25'000'000);
  EXPECT_EQ(out->stats().played_frames, 200u);
  EXPECT_EQ(out->queued(), 600u);
  // Less than a frame at a time still adds up.
  for (int i = 0; i < 1000; ++i) {
    clock.advance(10us);
    out->tick();
  }
  EXPECT_EQ(out->position(), kStart + 35'000'000);
}

TEST(NullAudioOut, PausedItsPositionHolds) {
  ManualClock clock;
  std::unique_ptr<NullAudioOut> out = by_caller(clock);
  ASSERT_NE(out, nullptr);
  const std::vector<float> a(800, 0.25f);
  out->flush(kStart);
  out->write(a.data(), 800);
  out->play();
  clock.advance(10ms);
  out->tick();
  out->pause();
  clock.advance(50ms);
  out->tick();
  EXPECT_EQ(out->position(), kStart + 10'000'000);
  out->play();
  clock.advance(5ms);
  out->tick();
  EXPECT_EQ(out->position(), kStart + 15'000'000) << "the pause not caught up";
}

TEST(NullAudioOut, ATickRendersAtMostASecond) {
  ManualClock clock;
  std::unique_ptr<NullAudioOut> out = by_caller(clock);
  ASSERT_NE(out, nullptr);
  out->flush(kStart);
  out->write(nullptr, out->capacity());
  out->play();
  clock.advance(5s);
  out->tick();
  EXPECT_EQ(out->stats().played_frames, kRate);
  EXPECT_EQ(out->stats().underrun_frames, 0u);
}

TEST(NullAudioOut, ItsOwnThreadPlaysBySteadyClock) {
  core::Result<std::unique_ptr<NullAudioOut>> made =
      NullAudioOut::create({kRate, 1});
  ASSERT_TRUE(made.ok()) << made.status().message();
  NullAudioOut& out = **made;
  out.flush(kStart);
  out.write(nullptr, out.capacity());
  const SteadyClock::time_point began = SteadyClock::now();
  out.play();
  std::this_thread::sleep_for(100ms);
  const auto passed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      SteadyClock::now() - began);
  const std::optional<std::uint64_t> at = out.position();
  ASSERT_TRUE(at.has_value());
  EXPECT_GT(*at, kStart);
  // A tick renders the time since the last, so the first after play may
  // count time before it: a period on time, at most a second (a tick's
  // most) on a runner slow to wake it.
  const auto most = passed + std::chrono::nanoseconds(1s);
  EXPECT_LE(*at, kStart + static_cast<std::uint64_t>(most.count()));
}

TEST(NullAudioOut, ItRefusesARateOfZeroOrOtherThanOneOrTwoChannels) {
  for (const AudioFormat format :
       {AudioFormat{0, 1}, AudioFormat{kRate, 0}, AudioFormat{kRate, 3}}) {
    const core::Result<std::unique_ptr<NullAudioOut>> out =
        NullAudioOut::create(format, {}, NullAudioOut::Ticks::kByCaller);
    EXPECT_EQ(out.status().domain(), core::Status::Code::InvalidArgument)
        << format.sample_rate << " Hz, " << format.channels << " channels";
  }
  const core::Result<std::unique_ptr<NullAudioOut>> standard =
      NullAudioOut::create({48000, 2}, {}, NullAudioOut::Ticks::kByCaller);
  EXPECT_TRUE(standard.ok()) << standard.status().message();
}

}  // namespace
