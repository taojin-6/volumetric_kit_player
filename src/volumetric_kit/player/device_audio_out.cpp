// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/device_audio_out.hpp"

#include <algorithm>
#include <new>
#include <thread>
#include <utility>

namespace volumetric_kit::player {

core::Result<std::unique_ptr<DeviceAudioOut>> DeviceAudioOut::create(
    const AudioFormat& format, const DeviceMaker& make) {
  if (format.sample_rate == 0 || format.channels < 1 || format.channels > 2) {
    return core::Status::invalid_argument(
        "DeviceAudioOut: a rate of 1 or more and 1 or 2 channels");
  }
  std::unique_ptr<DeviceAudioOut> out;
  try {
    out.reset(new DeviceAudioOut(format));
  } catch (const std::bad_alloc&) {
    return core::Status::out_of_memory("DeviceAudioOut: no room for its ring");
  }
  VKC_ASSIGN(std::unique_ptr<AudioDevice> device, make(*out));
  if (device == nullptr) {
    return core::Status::invalid_argument(
        "DeviceAudioOut: the device maker made none");
  }
  std::lock_guard<std::mutex> lock(out->control_mutex_);
  out->device_ = std::move(device);
  VKC_TRY(out->run_device_locked());
  return out;
}

DeviceAudioOut::DeviceAudioOut(const AudioFormat& format) : AudioOut(format) {}

DeviceAudioOut::~DeviceAudioOut() {
  std::unique_ptr<AudioDevice> device;
  {
    std::lock_guard<std::mutex> lock(control_mutex_);
    stop_device_locked(true);
    device = std::move(device_);
  }
  // Outside the lock: the device waits out its notices in flight, which
  // take it, and find no device.
  device.reset();
}

void DeviceAudioOut::render_at(float* out, std::size_t frames,
                               std::uint64_t host_ns) noexcept {
  // A start's renders before its route is read play silence and take
  // nothing, as if it had not begun: its runs need its latency.
  const std::uint64_t ready = ready_.load(std::memory_order_acquire);
  if (ready != starts_.load(std::memory_order_acquire)) {
    std::fill(out, out + frames * format().channels, 0.0f);
    return;
  }
  const Rendered rendered = render(out, frames);
  RenderTime time;
  time.frames = frames;
  // Its first frame taken passes the output after an insert's silence.
  time.host_ns =
      host_ns + audio_ns(rendered.silence_before, format().sample_rate);
  time.restarted = ready != seen_starts_;
  time.latency_ns = latency_ns_.load(std::memory_order_relaxed);
  seen_starts_ = ready;
  if (rendered.based && reanchors(clock_, rendered, time)) {
    runs_.fetch_add(1, std::memory_order_relaxed);
  }
  clock_ = after_render(clock_, rendered, time, format().sample_rate);
  publish(clock_);
}

void DeviceAudioOut::publish(const HeardClock& clock) noexcept {
  // A sequence lock's write: odd while the clock changes.
  const std::uint64_t seq = seq_.load(std::memory_order_relaxed);
  seq_.store(seq + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  based_.store(clock.based, std::memory_order_relaxed);
  floor_.store(clock.floor_pts_ns, std::memory_order_relaxed);
  has_current_.store(clock.current.has_value(), std::memory_order_relaxed);
  if (clock.current) {
    current_first_.store(clock.current->first_pts_ns,
                         std::memory_order_relaxed);
    current_end_.store(clock.current->end_pts_ns, std::memory_order_relaxed);
    current_end_host_.store(clock.current->end_host_ns,
                            std::memory_order_relaxed);
    current_latency_.store(clock.current->latency_ns,
                           std::memory_order_relaxed);
  }
  has_previous_.store(clock.previous.has_value(), std::memory_order_relaxed);
  if (clock.previous) {
    previous_first_.store(clock.previous->first_pts_ns,
                          std::memory_order_relaxed);
    previous_end_.store(clock.previous->end_pts_ns, std::memory_order_relaxed);
    previous_end_host_.store(clock.previous->end_host_ns,
                             std::memory_order_relaxed);
    previous_latency_.store(clock.previous->latency_ns,
                            std::memory_order_relaxed);
  }
  seq_.store(seq + 2, std::memory_order_release);
}

HeardClock DeviceAudioOut::published() const noexcept {
  // A sequence lock's read: again while the render thread writes.
  for (;;) {
    const std::uint64_t seq = seq_.load(std::memory_order_acquire);
    if ((seq & 1) != 0) {
      std::this_thread::yield();
      continue;
    }
    HeardClock clock;
    clock.based = based_.load(std::memory_order_relaxed);
    clock.floor_pts_ns = floor_.load(std::memory_order_relaxed);
    if (has_current_.load(std::memory_order_relaxed)) {
      clock.current =
          HeardRun{current_first_.load(std::memory_order_relaxed),
                   current_end_.load(std::memory_order_relaxed),
                   current_end_host_.load(std::memory_order_relaxed),
                   current_latency_.load(std::memory_order_relaxed)};
    }
    if (has_previous_.load(std::memory_order_relaxed)) {
      clock.previous =
          HeardRun{previous_first_.load(std::memory_order_relaxed),
                   previous_end_.load(std::memory_order_relaxed),
                   previous_end_host_.load(std::memory_order_relaxed),
                   previous_latency_.load(std::memory_order_relaxed)};
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (seq_.load(std::memory_order_relaxed) == seq) {
      return clock;
    }
  }
}

std::optional<std::uint64_t> DeviceAudioOut::position() const noexcept {
  // Stopped, nothing after the stop is heard.
  const std::uint64_t stopped = stopped_at_.load(std::memory_order_acquire);
  const std::uint64_t now = device_->host_now_ns();
  return heard_pts(published(), stopped != 0 ? std::min(now, stopped) : now);
}

AudioRoute DeviceAudioOut::route() const {
  std::lock_guard<std::mutex> lock(route_mutex_);
  AudioRoute route;
  route.audible = audible_;
  route.latency_ns = output_latency_ns(route_.presentation_latency_s,
                                       route_.session_latency_s);
  route.name = route_.name;
  return route;
}

DeviceRoute DeviceAudioOut::device_route() const {
  std::lock_guard<std::mutex> lock(route_mutex_);
  return route_;
}

void DeviceAudioOut::park() {
  std::lock_guard<std::mutex> lock(control_mutex_);
  parked_ = true;
  stop_device_locked(true);
}

void DeviceAudioOut::unpark() {
  std::lock_guard<std::mutex> lock(control_mutex_);
  parked_ = false;
  // An interruption may never end; the app active again tries anyway. A
  // failure, as during a call, leaves it stopped: route() says so, until
  // the next start.
  interrupted_ = false;
  static_cast<void>(run_device_locked());
}

bool DeviceAudioOut::take_pause_request() noexcept {
  return pause_requested_.exchange(false, std::memory_order_relaxed);
}

void DeviceAudioOut::interruption_began() {
  {
    std::lock_guard<std::mutex> lock(control_mutex_);
    interrupted_ = true;
    stop_device_locked(false);
  }
  pause_requested_.store(true, std::memory_order_relaxed);
}

void DeviceAudioOut::interruption_ended() {
  std::lock_guard<std::mutex> lock(control_mutex_);
  interrupted_ = false;
  static_cast<void>(run_device_locked());
}

void DeviceAudioOut::configuration_changed() {
  std::lock_guard<std::mutex> lock(control_mutex_);
  // The engine stopped itself; stop it here too, then start it on the new
  // route.
  stop_device_locked(false);
  static_cast<void>(run_device_locked());
}

void DeviceAudioOut::device_went_away() noexcept {
  pause_requested_.store(true, std::memory_order_relaxed);
}

core::Status DeviceAudioOut::run_device_locked() {
  if (device_ == nullptr || running_ || parked_ || interrupted_) {
    return {};
  }
  // Before the start, so its renders wait for its route (render_at).
  const std::uint64_t start =
      starts_.fetch_add(1, std::memory_order_acq_rel) + 1;
  VKC_TRY(device_->start());
  running_ = true;
  const DeviceRoute route = device_->route();
  latency_ns_.store(
      output_latency_ns(route.presentation_latency_s, route.session_latency_s),
      std::memory_order_relaxed);
  stopped_at_.store(0, std::memory_order_release);
  // From here its renders begin a new run, at that latency.
  ready_.store(start, std::memory_order_release);
  std::lock_guard<std::mutex> lock(route_mutex_);
  audible_ = true;
  route_ = route;
  return {};
}

void DeviceAudioOut::stop_device_locked(bool release) {
  if (device_ == nullptr) {
    return;
  }
  if (running_) {
    // The first stop's time: what was heard by then holds.
    stopped_at_.store(std::max<std::uint64_t>(device_->host_now_ns(), 1),
                      std::memory_order_release);
  }
  // A device stopped by the system (an interruption, a configuration
  // change) is stopped here too; a park also gives the route back.
  if (running_ || release) {
    device_->stop(release);
  }
  running_ = false;
  std::lock_guard<std::mutex> lock(route_mutex_);
  audible_ = false;
}

}  // namespace volumetric_kit::player
