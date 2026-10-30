// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/audio_out.hpp"

#include <algorithm>
#include <new>
#include <utility>

#include "volumetric_kit/core/base/exception_status.hpp"
#if defined(__APPLE__)
#include "volumetric_kit/player/device_audio_out.hpp"
#endif

namespace volumetric_kit::player {
namespace {

constexpr std::uint64_t kNsPerSecond = 1'000'000'000;
// The null output renders in pieces of this many frames.
constexpr std::size_t kScratchFrames = 1024;

}  // namespace

std::uint64_t audio_ns(std::uint64_t frames, std::uint32_t sample_rate) {
  // In two parts, so neither product overflows for any rate.
  return frames / sample_rate * kNsPerSecond +
         frames % sample_rate * kNsPerSecond / sample_rate;
}

std::uint64_t audio_frames(std::uint64_t ns, std::uint32_t sample_rate) {
  return ns / kNsPerSecond * sample_rate +
         (ns % kNsPerSecond * sample_rate + kNsPerSecond / 2) / kNsPerSecond;
}

core::Result<std::unique_ptr<AudioOut>> AudioOut::open_default(
    const AudioFormat& format) {
#if defined(__APPLE__)
  core::Result<std::unique_ptr<DeviceAudioOut>> apple =
      open_apple_audio(format);
  if (apple.ok()) {
    return std::unique_ptr<AudioOut>(std::move(apple).value());
  }
  // No device plays: the frames play on, through the null output.
#endif
  VKC_ASSIGN(std::unique_ptr<NullAudioOut> out, NullAudioOut::create(format));
  return std::unique_ptr<AudioOut>(std::move(out));
}

AudioOut::AudioOut(const AudioFormat& format)
    : format_(format), ring_(format.sample_rate, format.channels) {}

void AudioOut::flush(std::uint64_t pts_ns) noexcept {
  // A sequence lock's write: odd while the mark changes.
  const std::uint64_t seq = mark_seq_.load(std::memory_order_relaxed);
  mark_seq_.store(seq + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  mark_index_.store(ring_.written(), std::memory_order_relaxed);
  mark_pts_.store(pts_ns, std::memory_order_relaxed);
  mark_seq_.store(seq + 2, std::memory_order_release);
}

std::uint64_t AudioOut::queued() const noexcept {
  const std::uint64_t written = ring_.written();
  const std::uint64_t from =
      std::max(ring_.read_index(), mark_index_.load(std::memory_order_relaxed));
  return written > from ? written - from : 0;
}

std::optional<std::uint64_t> AudioOut::position() const noexcept {
  const std::uint64_t at = position_.load(std::memory_order_acquire);
  if (at == kNoPosition) {
    return std::nullopt;
  }
  return at;
}

AudioOutStats AudioOut::stats() const noexcept {
  AudioOutStats stats;
  stats.played_frames = played_.load(std::memory_order_relaxed);
  stats.underrun_frames = underrun_.load(std::memory_order_relaxed);
  stats.underruns = underruns_.load(std::memory_order_relaxed);
  stats.flushes = flushes_.load(std::memory_order_relaxed);
  return stats;
}

bool AudioOut::apply_flush() noexcept {
  // A sequence lock's read that never waits: a mark being written is
  // applied at a later render.
  const std::uint64_t seq = mark_seq_.load(std::memory_order_acquire);
  if (seq == seen_seq_ || (seq & 1) != 0) {
    return false;
  }
  const std::uint64_t index = mark_index_.load(std::memory_order_relaxed);
  const std::uint64_t pts = mark_pts_.load(std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_acquire);
  if (mark_seq_.load(std::memory_order_relaxed) != seq) {
    return false;
  }
  seen_seq_ = seq;
  ring_.skip_to(index);
  based_ = true;
  base_index_ = index;
  base_pts_ = pts;
  flushes_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

Rendered AudioOut::render(float* out, std::size_t frames) noexcept {
  Rendered rendered;
  rendered.flushed = apply_flush();
  rendered.based = based_;
  // The ring's index can pass the base only by frames taken since.
  const auto pts_at = [this](std::uint64_t index) {
    return base_pts_ + audio_ns(index - base_index_, format_.sample_rate);
  };
  if (based_) {
    rendered.pts_ns = pts_at(ring_.read_index());
  }
  const std::size_t channels = format_.channels;
  std::size_t got = 0;
  if (playing_.load(std::memory_order_relaxed)) {
    got = ring_.read(out, frames);
    const float gain = gain_.load(std::memory_order_relaxed);
    if (gain != 1.0f) {
      std::for_each(out, out + got * channels,
                    [gain](float& sample) { sample *= gain; });
    }
    played_.fetch_add(got, std::memory_order_relaxed);
    if (based_ && got < frames) {
      underrun_.fetch_add(frames - got, std::memory_order_relaxed);
      underruns_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  std::fill(out + got * channels, out + frames * channels, 0.0f);
  rendered.frames = got;
  if (based_) {
    rendered.end_pts_ns = pts_at(ring_.read_index());
    position_.store(rendered.end_pts_ns, std::memory_order_release);
  }
  return rendered;
}

core::Result<std::unique_ptr<NullAudioOut>> NullAudioOut::create(
    const AudioFormat& format, Clock clock, Ticks ticks) {
  if (format.sample_rate == 0 || format.channels < 1 || format.channels > 2) {
    return core::Status::invalid_argument(
        "NullAudioOut: a rate of 1 or more and 1 or 2 channels");
  }
  std::unique_ptr<NullAudioOut> out;
  try {
    if (!clock) {
      clock = [] { return std::chrono::steady_clock::now(); };
    }
    out.reset(new NullAudioOut(format, std::move(clock)));
  } catch (const std::bad_alloc&) {
    return core::Status::out_of_memory("NullAudioOut: no room for its ring");
  }
  if (ticks == Ticks::kOwnThread) {
    try {
      NullAudioOut* const self = out.get();
      out->thread_ = std::thread([self] {
        std::unique_lock<std::mutex> lock(self->mutex_);
        while (!self->quit_) {
          lock.unlock();
          self->tick();
          lock.lock();
          self->quit_changed_.wait_for(lock, kNullPeriod,
                                       [self] { return self->quit_; });
        }
      });
    } catch (...) {
      return core::exception_status("NullAudioOut's thread",
                                    core::Status::Code::IoError);
    }
  }
  return out;
}

NullAudioOut::NullAudioOut(const AudioFormat& format, Clock clock)
    : AudioOut(format),
      clock_(std::move(clock)),
      last_(clock_()),
      scratch_(kScratchFrames * format.channels) {}

NullAudioOut::~NullAudioOut() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
  }
  quit_changed_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void NullAudioOut::tick() noexcept {
  const std::chrono::steady_clock::time_point now = clock_();
  std::uint64_t frames = 0;
  if (now > last_) {
    // At most a second at once: the ring holds no more.
    const auto elapsed = std::min<std::chrono::nanoseconds>(
        now - last_, std::chrono::seconds(1));
    last_ = now;
    const std::uint64_t due =
        static_cast<std::uint64_t>(elapsed.count()) * format().sample_rate +
        carry_;
    frames = due / kNsPerSecond;
    carry_ = due % kNsPerSecond;
  }
  // At least one render, so a flush lands with no time passed.
  do {
    const std::size_t n = static_cast<std::size_t>(
        std::min<std::uint64_t>(frames, kScratchFrames));
    render(scratch_.data(), n);
    frames -= n;
  } while (frames > 0);
}

}  // namespace volumetric_kit::player
