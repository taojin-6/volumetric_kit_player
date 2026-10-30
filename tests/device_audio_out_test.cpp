// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// A device output over a fake device (fake_audio_device.hpp), on the host:
// no test opens an audio device. It starts its device as it is made, refuses
// what the null output refuses, a maker that made none and a first start
// that fails, and says its route: audible while the device plays, the
// presentation latency plus the session's. Its position maps the frames
// rendered, less those queued and the latency, onto the host clock's now; a
// mute keeps that clock running. A park stops the device and gives the route
// back, stopped or not, the position holding at what was heard as it
// stopped, and an unpark starts it again, its next render a new run. An
// interruption stops it and asks the player to pause, once; its end starts
// it unless parked, and an unpark ends one that never ended, trying a start.
// A configuration change starts it again, re-reading the route; a device
// gone asks the player to pause. A shift's insert holds what is heard
// through its silence, the frames after it heard as they pass the output.
// Onto a route of longer latency the position never goes back; a restart
// begins a new run even where its frames follow on; and a start's renders
// before its route is read play silence and take nothing. The route is read
// while a start is under way without waiting for it. A start that fails
// leaves it stopped until the next. The render thread and a reader of the
// position pass the clock between them under ThreadSanitizer, the position
// never going back.

#include "volumetric_kit/player/device_audio_out.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "fake_audio_device.hpp"

namespace {

namespace core = volumetric_kit::core;
using volumetric_kit::player::audio_ns;
using volumetric_kit::player::AudioFormat;
using volumetric_kit::player::AudioRoute;
using volumetric_kit::player::DeviceAudioOut;
using vp_test::FakeDeviceState;
using Code = core::Status::Code;

constexpr std::uint32_t kRate = 8000;     // a frame is 125 us
constexpr std::size_t kFrames = 80;       // a render: 10 ms
constexpr std::uint64_t kMs = 1'000'000;  // ns
constexpr std::uint64_t kPts = 5'000 * kMs;

class DeviceAudioOutTest : public ::testing::Test {
 protected:
  void SetUp() override {
    state_ = std::make_shared<FakeDeviceState>();
    core::Result<std::unique_ptr<DeviceAudioOut>> made =
        DeviceAudioOut::create(format_, vp_test::fake_device(state_));
    ASSERT_TRUE(made.ok()) << made.status().message();
    out_ = std::move(made).value();
  }

  // The host clock's now.
  std::uint64_t now() const { return state_->host_ns.load(); }
  void advance(std::uint64_t ns) { state_->host_ns += ns; }

  // A second of frames counting up, written after a flush to kPts, playing.
  void fill() {
    std::vector<float> ramp(kRate);
    for (std::size_t i = 0; i < ramp.size(); ++i) {
      ramp[i] = static_cast<float>(i + 1);
    }
    out_->flush(kPts);
    ASSERT_EQ(out_->write(ramp.data(), ramp.size()), ramp.size());
    out_->play();
  }

  // A render of kFrames going out now, as a device does ahead of the ear.
  std::vector<float> render_now() {
    return vp_test::render(*out_, kFrames, now());
  }

  int starts() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->starts;
  }
  int releases() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->releases;
  }
  bool running() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->running;
  }

  AudioFormat format_{kRate, 1};
  std::shared_ptr<FakeDeviceState> state_;
  std::unique_ptr<DeviceAudioOut> out_;
};

TEST_F(DeviceAudioOutTest, ItStartsItsDeviceAndSaysItsRoute) {
  EXPECT_EQ(starts(), 1);
  EXPECT_TRUE(running());
  EXPECT_FALSE(out_->playing());
  const AudioRoute route = out_->route();
  EXPECT_TRUE(route.audible);
  EXPECT_EQ(route.latency_ns, 10 * kMs);
  EXPECT_EQ(route.name, "Fake Speakers");
  EXPECT_EQ(out_->device_route().presentation_latency_s, 0.010);
}

TEST(DeviceAudioOut, RefusesBadFormatsANullDeviceAndAFailedFirstStart) {
  auto state = std::make_shared<FakeDeviceState>();
  for (const AudioFormat format :
       {AudioFormat{0, 1}, AudioFormat{kRate, 0}, AudioFormat{kRate, 3}}) {
    EXPECT_EQ(DeviceAudioOut::create(format, vp_test::fake_device(state))
                  .status()
                  .domain(),
              Code::InvalidArgument);
  }
  EXPECT_EQ(state->starts, 0) << "no device made for a bad format";
  const auto none = [](DeviceAudioOut&)
      -> core::Result<std::unique_ptr<volumetric_kit::player::AudioDevice>> {
    return std::unique_ptr<volumetric_kit::player::AudioDevice>();
  };
  EXPECT_EQ(DeviceAudioOut::create({kRate, 1}, none).status().domain(),
            Code::InvalidArgument);
  state->fail_starts = true;
  EXPECT_EQ(DeviceAudioOut::create({kRate, 2}, vp_test::fake_device(state))
                .status()
                .domain(),
            Code::IoError);
}

// 10 ms of latency: what is heard now passed the device's output 10 ms ago.
TEST_F(DeviceAudioOutTest, ThePositionIsTheRenderedLessTheQueuedAndLatency) {
  EXPECT_FALSE(out_->position().has_value());
  fill();
  const std::uint64_t start = now();
  render_now();  // frames [0, 10 ms) pass the output from start on
  advance(10 * kMs);
  render_now();                       // [10, 20 ms) from start + 10 ms
  EXPECT_EQ(out_->position(), kPts);  // the first frame is being heard
  advance(5 * kMs);
  EXPECT_EQ(out_->position(), kPts + 5 * kMs);
  advance(20 * kMs);
  EXPECT_EQ(out_->position(), kPts + 20 * kMs) << "held at what was rendered";
  EXPECT_EQ(now(), start + 35 * kMs);
}

TEST_F(DeviceAudioOutTest, AMuteKeepsTheClockRunning) {
  fill();
  out_->set_gain(0.0f);
  const std::vector<float> first = render_now();
  EXPECT_TRUE(std::all_of(first.begin(), first.end(),
                          [](float sample) { return sample == 0.0f; }));
  advance(10 * kMs);
  render_now();
  advance(15 * kMs);
  EXPECT_EQ(out_->position(), kPts + 15 * kMs);
  EXPECT_EQ(out_->stats().played_frames, 2 * kFrames);
}

// Frames rendered but not yet heard when the device stops are never heard:
// the position holds at the stop.
TEST_F(DeviceAudioOutTest, AParkStopsTheDeviceAndThePositionHolds) {
  fill();
  render_now();
  advance(10 * kMs);
  render_now();
  advance(13 * kMs);  // 3 ms of the second render heard
  out_->park();
  EXPECT_FALSE(running());
  EXPECT_EQ(releases(), 1) << "a park gives the route back";
  EXPECT_FALSE(out_->route().audible);
  advance(500 * kMs);
  EXPECT_EQ(out_->position(), kPts + 13 * kMs);
  out_->unpark();
  EXPECT_EQ(starts(), 2);
  EXPECT_TRUE(running());
  EXPECT_TRUE(out_->route().audible);
  // Unparked, the position passes the frames the stopped device never
  // played; the next render begins a new run, heard after the latency.
  render_now();
  EXPECT_EQ(out_->position(), kPts + 20 * kMs);
  advance(14 * kMs);
  EXPECT_EQ(out_->position(), kPts + 24 * kMs);
}

TEST_F(DeviceAudioOutTest, AnInterruptionStopsItAndAsksThePlayerToPauseOnce) {
  EXPECT_FALSE(out_->take_pause_request());
  out_->interruption_began();
  EXPECT_FALSE(running());
  EXPECT_EQ(releases(), 0) << "the system took the route already";
  EXPECT_FALSE(out_->route().audible);
  EXPECT_TRUE(out_->take_pause_request());
  EXPECT_FALSE(out_->take_pause_request());
  out_->interruption_ended();
  EXPECT_TRUE(running());
  EXPECT_EQ(starts(), 2);
  EXPECT_FALSE(out_->take_pause_request());
  // Parked when it ends, it waits for the unpark.
  out_->interruption_began();
  out_->park();
  EXPECT_EQ(releases(), 1) << "a park gives the route back, stopped or not";
  out_->interruption_ended();
  EXPECT_FALSE(running());
  out_->unpark();
  EXPECT_TRUE(running());
}

// Apple documents that an interruption may never end: the unpark, the app
// active again, ends it and tries a start; one that fails, as during a call,
// leaves it stopped.
TEST_F(DeviceAudioOutTest, AnUnparkEndsAnInterruptionThatNeverEnded) {
  out_->interruption_began();
  out_->park();
  out_->unpark();
  EXPECT_TRUE(running());
  EXPECT_EQ(starts(), 2);
  EXPECT_TRUE(out_->route().audible);
  out_->interruption_ended();  // late: it plays already
  EXPECT_EQ(starts(), 2);
  out_->interruption_began();
  out_->park();
  state_->fail_starts = true;
  out_->unpark();
  EXPECT_FALSE(running());
  EXPECT_FALSE(out_->route().audible);
}

TEST_F(DeviceAudioOutTest, AConfigurationChangeStartsItAgainOnTheNewRoute) {
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->route = {"AirPods", 0.150, 0.030};
  }
  out_->configuration_changed();
  EXPECT_EQ(starts(), 2);
  EXPECT_TRUE(running());
  const AudioRoute route = out_->route();
  EXPECT_EQ(route.name, "AirPods");
  EXPECT_EQ(route.latency_ns, 180 * kMs);
  EXPECT_FALSE(out_->take_pause_request());
  out_->device_went_away();
  EXPECT_TRUE(out_->take_pause_request());
  // Parked, a change starts nothing.
  out_->park();
  out_->configuration_changed();
  EXPECT_FALSE(running());
  EXPECT_EQ(starts(), 2);
}

// A route change to a longer latency, speakers at 10 ms to AirPods at
// 180 ms: the frames rendered before it keep the old route's latency, so the
// position never goes back; it holds at their end until the new route's
// first frame is heard. Mutant: one latency for every run.
TEST_F(DeviceAudioOutTest, ARouteChangeToALongerLatencyNeverMovesItBack) {
  fill();
  render_now();
  advance(10 * kMs);
  render_now();
  advance(15 * kMs);
  ASSERT_EQ(out_->position(), kPts + 15 * kMs);
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->route = {"AirPods", 0.150, 0.030};
  }
  out_->configuration_changed();
  // The new route renders every 10 ms from the change on.
  std::uint64_t last = 0;
  for (std::uint64_t ms = 0; ms <= 400; ++ms) {
    if (ms % 10 == 0) {
      render_now();
    }
    const std::uint64_t heard = out_->position().value_or(0);
    EXPECT_GE(heard, last) << ms << " ms after the change";
    if (ms == 100) {
      EXPECT_EQ(heard, kPts + 20 * kMs) << "held at the old route's end";
    }
    last = heard;
    advance(kMs);
  }
  EXPECT_EQ(last, kPts + 240 * kMs) << "the new route's first frame was "
                                       "heard 180 ms after the change";
}

// A shift's insert of 10 ms, begun in the render at 20 ms and leading the
// next: what is heard holds through its silence, then the frames after it
// are heard from when they pass the output, a new run.
TEST_F(DeviceAudioOutTest, AnInsertHoldsWhatIsHeardThroughItsSilence) {
  fill();
  const std::uint64_t start = now();
  render_now();
  advance(10 * kMs);
  render_now();
  advance(10 * kMs);
  out_->shift(static_cast<std::int64_t>(kFrames));
  render_now();  // 5 ms of frames, faded out, then 5 ms of silence
  advance(10 * kMs);
  render_now();  // 5 ms of silence, then 5 ms of frames
  EXPECT_EQ(out_->position(), kPts + 20 * kMs);
  advance(10 * kMs);
  EXPECT_EQ(out_->position(), kPts + 25 * kMs) << "held through the silence";
  advance(5 * kMs);
  EXPECT_EQ(out_->position(), kPts + 25 * kMs);
  advance(2 * kMs);
  EXPECT_EQ(out_->position(), kPts + 27 * kMs);
  EXPECT_EQ(now(), start + 47 * kMs);
  EXPECT_EQ(out_->stats().underruns, 0u);
}

// A restart begins a new run even where its first render follows on, at the
// old run's end with the pts after it, so the new frames are heard at the
// new route's latency. Mutant: render_at that misses the restart.
TEST_F(DeviceAudioOutTest, ARestartBeginsANewRunThoughItsFramesFollowOn) {
  fill();
  const std::uint64_t start = now();
  render_now();
  advance(10 * kMs);
  render_now();  // the run ends at start + 20 ms
  EXPECT_EQ(out_->runs(), 1u);
  advance(5 * kMs);
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->route = {"Fake Headphones", 0.030, 0.0};
  }
  out_->configuration_changed();
  advance(5 * kMs);
  render_now();  // at start + 20 ms, from 20 ms on
  EXPECT_EQ(out_->runs(), 2u);
  advance(35 * kMs);
  EXPECT_EQ(out_->position(), kPts + 25 * kMs) << "heard 30 ms after it went";
  EXPECT_EQ(now(), start + 55 * kMs);
}

// An engine may render before its start returns: until the start has read
// its route, those renders play silence and take nothing, and the next
// begins the new run at the new route's latency. Mutant: no wait.
TEST_F(DeviceAudioOutTest, AStartsRendersWaitForItsRoute) {
  fill();
  render_now();
  advance(10 * kMs);
  std::vector<float> early;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->route = {"AirPods", 0.150, 0.030};
    state_->during_start = [this, &early] { early = render_now(); };
  }
  out_->configuration_changed();
  ASSERT_EQ(early.size(), kFrames);
  EXPECT_TRUE(std::all_of(early.begin(), early.end(),
                          [](float sample) { return sample == 0.0f; }));
  EXPECT_EQ(out_->stats().played_frames, kFrames) << "nothing taken";
  EXPECT_EQ(out_->runs(), 1u);
  render_now();  // [10, 20 ms) from now, heard 180 ms on
  EXPECT_EQ(out_->runs(), 2u);
  advance(185 * kMs);
  EXPECT_EQ(out_->position(), kPts + 15 * kMs);
}

// The frame thread's status line reads the route while a start, an engine's
// or the session's, is under way: route() answers as of the stop before it,
// waiting for none. Mutant: route() under the lifecycle's mutex.
TEST_F(DeviceAudioOutTest, ARouteReadWaitsForNoDeviceCall) {
  std::promise<void> entered;
  std::promise<void> go;
  std::shared_future<void> released = go.get_future().share();
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->during_start = [&entered, released] {
      entered.set_value();
      released.wait();
    };
  }
  std::thread changer([this] { out_->configuration_changed(); });
  entered.get_future().wait();
  std::future<AudioRoute> read =
      std::async(std::launch::async, [this] { return out_->route(); });
  const bool prompt =
      read.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  go.set_value();
  changer.join();
  ASSERT_TRUE(prompt) << "route() waited for the device's start";
  EXPECT_FALSE(read.get().audible);
  EXPECT_TRUE(out_->route().audible) << "once it started";
}

TEST_F(DeviceAudioOutTest, AStartThatFailsLeavesItStoppedUntilTheNext) {
  out_->park();
  state_->fail_starts = true;
  out_->unpark();
  EXPECT_FALSE(running());
  EXPECT_FALSE(out_->route().audible);
  state_->fail_starts = false;
  out_->configuration_changed();
  EXPECT_TRUE(running());
  EXPECT_TRUE(out_->route().audible);
}

// The render thread renders back to back, each render 10 ms on the host
// clock; the reader's position never goes back.
TEST_F(DeviceAudioOutTest, TheClockPassesFromTheRenderThreadToAReader) {
  constexpr int kRenders = 2'000;
  std::vector<float> silence(kFrames * kRenders);
  out_->flush(kPts);
  out_->play();
  std::atomic<bool> done{false};
  std::thread renderer([&] {
    std::vector<float> scratch(kFrames);
    std::uint64_t host = now();
    std::size_t written = 0;
    for (int i = 0; i < kRenders; ++i) {
      written +=
          out_->write(silence.data() + written,
                      std::min<std::size_t>(kFrames, silence.size() - written));
      out_->render_at(scratch.data(), kFrames, host);
      host += audio_ns(kFrames, kRate);
      state_->host_ns.store(host);
    }
    done = true;
  });
  std::uint64_t last = 0;
  while (!done) {
    const std::optional<std::uint64_t> heard = out_->position();
    if (heard) {
      EXPECT_GE(*heard, last);
      last = *heard;
    }
  }
  renderer.join();
  EXPECT_EQ(out_->position(), kPts + kRenders * 10 * kMs - 10 * kMs)
      << "the last render's frames, still queued, less the latency's";
}

}  // namespace
