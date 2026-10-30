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

// `ms` as frames at `rate`, at least 1.
std::size_t frames_of(std::chrono::milliseconds ms, std::uint32_t rate) {
  const auto ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(ms).count());
  return static_cast<std::size_t>(
      std::max<std::uint64_t>(audio_frames(ns, rate), 1));
}

// A fade's gain at frame `i` of `frames`, rising from near 0 to near 1.
float ramp(std::size_t i, std::size_t frames) {
  return (static_cast<float>(i) + 0.5f) / static_cast<float>(frames);
}

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
    : format_(format),
      ring_(format.sample_rate, format.channels),
      fade_frames_(frames_of(kShiftFade, format.sample_rate)),
      drop_frames_(frames_of(kDropStep, format.sample_rate)),
      drop_scratch_((fade_frames_ + drop_frames_) * format.channels) {}

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
  // The shifts asked for so far were made against what it dropped.
  shift_.store(0, std::memory_order_relaxed);
  shift_left_.store(0, std::memory_order_relaxed);
  inserting_ = false;
  fade_in_left_ = 0;
  flushes_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

std::size_t AudioOut::crossfade_drop(float* out) noexcept {
  const std::int64_t left = shift_left_.load(std::memory_order_relaxed);
  const std::size_t step = static_cast<std::size_t>(
      std::min<std::int64_t>(-left, static_cast<std::int64_t>(drop_frames_)));
  const std::size_t needed = step + fade_frames_;
  if (ring_.written() - ring_.read_index() < needed) {
    return 0;  // the next render tries again
  }
  ring_.read(drop_scratch_.data(), needed);
  // From the frames at the read index into those `step` later.
  const std::size_t channels = format_.channels;
  for (std::size_t i = 0; i < fade_frames_; ++i) {
    const float in = ramp(i, fade_frames_);
    for (std::size_t c = 0; c < channels; ++c) {
      out[i * channels + c] = drop_scratch_[i * channels + c] * (1.0f - in) +
                              drop_scratch_[(step + i) * channels + c] * in;
    }
  }
  shift_left_.store(left + static_cast<std::int64_t>(step),
                    std::memory_order_release);
  return fade_frames_;
}

Rendered AudioOut::render(float* out, std::size_t frames) noexcept {
  Rendered rendered;
  rendered.flushed = apply_flush();
  rendered.based = based_;
  // The ring's index can pass the base only by frames taken since.
  const auto pts_at = [this](std::uint64_t index) {
    return base_pts_ + audio_ns(index - base_index_, format_.sample_rate);
  };
  const std::size_t channels = format_.channels;
  const bool playing = playing_.load(std::memory_order_relaxed);
  // What goes out: an insert's silence under way (`lead`), the frames taken
  // (`got`), then silence: an insert begun here, a pause or a short ring.
  std::size_t lead = 0;
  std::size_t got = 0;
  bool begins = false;  // an insert begins after this render's frames
  if (playing && based_) {
    std::int64_t left = shift_left_.load(std::memory_order_relaxed) +
                        shift_.exchange(0, std::memory_order_acq_rel);
    if (inserting_ && left <= 0) {  // a drop asked for ends the insert
      inserting_ = false;
      fade_in_left_ = fade_frames_;
    }
    if (inserting_) {
      lead = static_cast<std::size_t>(
          std::min<std::int64_t>(left, static_cast<std::int64_t>(frames)));
      left -= static_cast<std::int64_t>(lead);
      if (left == 0) {
        inserting_ = false;
        fade_in_left_ = fade_frames_;
      }
    }
    shift_left_.store(left, std::memory_order_release);
    // A drop at a render's start, once the fade-in before it is done.
    if (!inserting_ && left < 0 && lead == 0 && fade_in_left_ == 0 &&
        frames >= fade_frames_) {
      got = crossfade_drop(out);
    }
    begins = !inserting_ && shift_left_.load(std::memory_order_relaxed) > 0;
  }
  std::fill(out, out + lead * channels, 0.0f);
  if (based_) {
    rendered.pts_ns = pts_at(ring_.read_index() - got);
  }
  if (playing) {
    const std::size_t room = frames - lead - got;
    std::size_t take = room;
    bool ends = false;  // the insert begun here also ends here
    if (begins) {
      // Its silence after a fade-out of the frames before it: the whole
      // insert in this render if it fits, else what does, the rest leading
      // the next renders.
      const std::int64_t left = shift_left_.load(std::memory_order_relaxed);
      const std::size_t silence =
          static_cast<std::size_t>(std::min<std::int64_t>(
              left, static_cast<std::int64_t>(
                        room > fade_frames_ ? room - fade_frames_ : 0)));
      take = room - silence;
      shift_left_.store(left - static_cast<std::int64_t>(silence),
                        std::memory_order_release);
      inserting_ = left > static_cast<std::int64_t>(silence);
      ends = !inserting_;
    }
    float* const first = out + (lead + got) * channels;
    const std::size_t taken = ring_.read(first, take);
    // After an insert, its frames fade in; before one, out.
    for (std::size_t i = 0; i < taken && fade_in_left_ > 0;
         ++i, --fade_in_left_) {
      const float in = ramp(fade_frames_ - fade_in_left_, fade_frames_);
      std::for_each(first + i * channels, first + (i + 1) * channels,
                    [in](float& sample) { sample *= in; });
    }
    if (begins) {
      const std::size_t faded = std::min(taken, fade_frames_);
      for (std::size_t j = 0; j < faded; ++j) {
        const float in = ramp(fade_frames_ - faded + j, fade_frames_);
        float* const frame = first + (taken - faded + j) * channels;
        std::for_each(frame, frame + channels,
                      [in](float& sample) { sample *= 1.0f - in; });
      }
    }
    if (ends) {
      fade_in_left_ = fade_frames_;  // the next frames taken
    }
    got += taken;
    const float gain = gain_.load(std::memory_order_relaxed);
    if (gain != 1.0f) {
      std::for_each(out + lead * channels, out + (lead + got) * channels,
                    [gain](float& sample) { sample *= gain; });
    }
    played_.fetch_add(got, std::memory_order_relaxed);
    if (based_ && taken < take) {
      underrun_.fetch_add(take - taken, std::memory_order_relaxed);
      underruns_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  std::fill(out + (lead + got) * channels, out + frames * channels, 0.0f);
  rendered.frames = got;
  rendered.silence_before = lead;
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
      scratch_(kScratchFrames * format.channels) {
  ticked_ns_.store(last_.time_since_epoch().count(), std::memory_order_relaxed);
}

std::optional<std::uint64_t> NullAudioOut::position() const noexcept {
  // The position first: a tick stores its time before its position, so the
  // time read after is that tick's or a later one's, and the time since
  // never counts a tick's frames twice.
  const std::optional<std::uint64_t> at = AudioOut::position();
  const std::int64_t ticked = ticked_ns_.load(std::memory_order_acquire);
  if (!at || !playing() || shifting() > 0) {
    return at;
  }
  const std::int64_t now = clock_().time_since_epoch().count();
  if (now <= ticked) {
    return at;
  }
  return *at +
         std::min<std::uint64_t>(static_cast<std::uint64_t>(now - ticked),
                                 audio_ns(queued(), format().sample_rate));
}

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
    // Before its render: a reader that sees this tick's position sees this.
    ticked_ns_.store(now.time_since_epoch().count(), std::memory_order_release);
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
