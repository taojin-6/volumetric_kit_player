// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file fake_audio_device.hpp
/// A device for DeviceAudioOut with no OS behind it: it counts its starts and
/// stops, fails a start when told, runs what the test gives it within its
/// next start (a render before the start returns, as an engine's may, or a
/// wait), says the route it is given, keeps a host clock the test moves, and
/// renders when the test asks, as a device's render thread would. No test
/// opens an audio device.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/player/device_audio_out.hpp"

namespace vp_test {

namespace vkp = volumetric_kit::player;

// What a fake device was asked and is told, shared with the test, which
// keeps it past the output.
struct FakeDeviceState {
  std::mutex mutex;  // over all but the clock
  int starts = 0;
  int stops = 0;
  int releases = 0;          // stops that gave the route back
  bool running = false;      // between a start and a stop
  bool fail_starts = false;  // starts fail
  // Run once, within the next start that succeeds, outside the mutex.
  std::function<void()> during_start;
  vkp::DeviceRoute route{"Fake Speakers", 0.010, 0.0};
  std::atomic<std::uint64_t> host_ns{1'000'000'000};  // the host clock
  vkp::DeviceAudioOut* out = nullptr;
};

class FakeDevice final : public vkp::AudioDevice {
 public:
  FakeDevice(vkp::DeviceAudioOut& out, std::shared_ptr<FakeDeviceState> state)
      : state_(std::move(state)) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->out = &out;
  }

  volumetric_kit::core::Status start() override {
    std::function<void()> during;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      ++state_->starts;
      if (state_->fail_starts) {
        return volumetric_kit::core::Status::io_error(
            "the fake will not start");
      }
      state_->running = true;
      during = std::exchange(state_->during_start, {});
    }
    if (during) {
      during();
    }
    return {};
  }
  void stop(bool release) noexcept override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    ++state_->stops;
    state_->releases += release ? 1 : 0;
    state_->running = false;
  }
  vkp::DeviceRoute route() const override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->route;
  }
  std::uint64_t host_now_ns() const noexcept override {
    return state_->host_ns.load();
  }

 private:
  std::shared_ptr<FakeDeviceState> state_;
};

// A maker of fake devices over `state`.
inline vkp::DeviceAudioOut::DeviceMaker fake_device(
    std::shared_ptr<FakeDeviceState> state) {
  return
      [state](vkp::DeviceAudioOut& out)
          -> volumetric_kit::core::Result<std::unique_ptr<vkp::AudioDevice>> {
        return std::unique_ptr<vkp::AudioDevice>(new FakeDevice(out, state));
      };
}

// `frames` interleaved frames of `out` rendered as its device would, the
// first passing the device's output at `host_ns`.
inline std::vector<float> render(vkp::DeviceAudioOut& out, std::size_t frames,
                                 std::uint64_t host_ns) {
  std::vector<float> samples(frames * out.format().channels, -99.0f);
  out.render_at(samples.data(), frames, host_ns);
  return samples;
}

}  // namespace vp_test
