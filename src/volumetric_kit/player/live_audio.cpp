// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/live_audio.hpp"

#include <algorithm>
#include <utility>

#include "volumetric_kit/core/base/exception_status.hpp"

namespace volumetric_kit::player {
namespace {

using std::chrono::nanoseconds;
using Clock = Playback::Clock;

nanoseconds magnitude(nanoseconds ns) { return ns < nanoseconds(0) ? -ns : ns; }

}  // namespace

nanoseconds audio_need(nanoseconds period, nanoseconds latency,
                       nanoseconds raised) noexcept {
  const nanoseconds base = period + latency + kAudioMargin;
  return std::max(
      base, std::min(std::max(base, kAudioNeedLeast) + raised, kAudioNeedMost));
}

nanoseconds drift_trim(nanoseconds ahead) noexcept {
  if (ahead > kTrimBand) {
    return kTrimStep;  // heard early: an insert holds it back
  }
  if (ahead < -kTrimBand) {
    return -kTrimStep;  // heard late: a drop brings it on
  }
  return nanoseconds(0);
}

LiveAudio::LiveAudio(LiveClock& clock, AudioOutMaker make, Now now)
    : clock_(clock),
      make_(std::move(make)),
      now_(now ? std::move(now) : Now([] { return Clock::now(); })) {}

LiveAudio::~LiveAudio() { set_playing(false); }

core::Status LiveAudio::begin(const std::optional<AudioTrackChoice>& track,
                              nanoseconds period) {
  end();
  if (!track) {
    return {};
  }
  const AudioFormat format = played_format(track->params);
  if (out_ == nullptr || out_->format().sample_rate != format.sample_rate ||
      out_->format().channels != format.channels) {
    std::unique_ptr<AudioOut> made;
    try {
      if (make_) {
        VKC_ASSIGN(made, make_(format));
      } else {
        VKC_ASSIGN(made, NullAudioOut::create(format));
      }
    } catch (...) {
      return core::exception_status("LiveAudio's output",
                                    core::Status::Code::IoError);
    }
    if (made == nullptr) {
      return core::Status::invalid_argument(
          "LiveAudio: the audio output maker made none");
    }
    std::unique_ptr<AudioOut> last;  // destroyed outside the lock
    {
      std::lock_guard<std::mutex> lock(mutex_);
      made->set_gain(gain_);
      if (parked_) {
        made->park();
      }
      made->play();  // live, it always plays
      last = std::exchange(out_, std::move(made));
    }
  }
  track_ = track;
  period_ = period;
  underruns_ = out_->stats().underruns;
  counting_ = false;
  raised_ = nanoseconds(0);
  raised_at_ = now_();
  raise_played_.reset();
  raise_every_ =
      audio_frames(static_cast<std::uint64_t>(kAudioRaiseEvery.count()),
                   track->params.sample_rate);
  // Stated from the start, so the first frame's D has it.
  const nanoseconds need =
      audio_need(period_, nanoseconds(out_->route().latency_ns), raised_);
  need_ns_.store(need.count(), std::memory_order_relaxed);
  late_ns_.store(std::max(need - kMaxLiveDelay, nanoseconds(0)).count(),
                 std::memory_order_relaxed);
  return {};
}

void LiveAudio::end() {
  track_.reset();
  placed_ = false;
  raised_ = nanoseconds(0);
  need_ns_.store(0, std::memory_order_relaxed);
  late_ns_.store(0, std::memory_order_relaxed);
  set_playing(false);
}

void LiveAudio::unplace() {
  placed_ = false;
  set_playing(false);
}

void LiveAudio::set_playing(bool playing) {
  playing_.store(playing, std::memory_order_release);
  clock_.set_audio_playing(playing);
}

std::optional<nanoseconds> LiveAudio::audio_point() const {
  const std::optional<nanoseconds> point = clock_.point();
  if (!point) {
    return std::nullopt;
  }
  return *point + late();
}

std::int64_t LiveAudio::frames_of(nanoseconds ns) const {
  const auto frames = static_cast<std::int64_t>(
      audio_frames(static_cast<std::uint64_t>(magnitude(ns).count()),
                   track_->params.sample_rate));
  return ns < nanoseconds(0) ? -frames : frames;
}

void LiveAudio::arrived(const grid::stream::RecordInfo& info,
                        const std::uint8_t* pcm, std::size_t bytes,
                        Clock::time_point at) {
  if (!track_) {
    return;
  }
  AudioOut& out = *out_;
  const std::uint16_t channels = track_->params.channels;
  const std::uint32_t rate = track_->params.sample_rate;
  const std::size_t frame_bytes = std::size_t{2} * channels;
  const auto refused = [this] {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.dropped;
  };
  const std::optional<nanoseconds> point = audio_point();
  if (bytes % frame_bytes != 0 || !point) {
    refused();
    return;
  }
  const std::uint64_t frames = bytes / frame_bytes;
  if (!placed_) {
    // Its next frame is heard a render and the output's latency on: the pts
    // due then.
    const nanoseconds latency(out.route().latency_ns);
    base_pts_ = static_cast<std::uint64_t>(
        std::max<std::int64_t>(pts_at(*point, at + latency), 0));
    flushes_ = out.stats().flushes + 1;
    out.flush(base_pts_);
    written_ = 0;
    exact_ = true;
    placed_ = true;
    counting_ = false;  // from the first look that sees it applied
    placed_point_ = *point;
    shifted_at_ = at;
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.placements;
  }
  const Placement where = place(base_pts_ + audio_ns(written_, rate),
                                info.pts_ns, frames, rate, exact_);
  if (where.skip == frames) {
    refused();  // wholly before what is written: late
    return;
  }
  const std::uint64_t kept = frames - where.skip;
  const std::uint16_t played = played_channels(channels);
  samples_.resize(static_cast<std::size_t>(kept) * played);
  to_played(pcm + where.skip * frame_bytes, static_cast<std::size_t>(kept),
            channels, samples_.data());
  const auto silence = static_cast<std::size_t>(where.silence);
  const std::size_t quiet = out.write(nullptr, silence);
  const std::size_t wrote =
      quiet == silence
          ? out.write(samples_.data(), static_cast<std::size_t>(kept))
          : 0;
  written_ += quiet + wrote;
  exact_ = false;
  if (quiet < silence || wrote < kept) {
    unplace();  // the ring is full: the next record is placed afresh
  }
  std::lock_guard<std::mutex> lock(mutex_);
  ++stats_.records;
}

void LiveAudio::look(Clock::time_point now) {
  if (!track_) {
    return;
  }
  AudioOut& out = *out_;
  const AudioOutStats counted = out.stats();
  const nanoseconds latency(out.route().latency_ns);
  // Underruns since the look before, once the placing flush was applied
  // and a look had seen it: the audio came late. They raise the need a
  // step, at most once a kAudioRaiseEvery of audio played, and the audio is
  // placed afresh. kLiveWindow with none lowers the need a step.
  const bool applied = placed_ && counted.flushes >= flushes_;
  const bool ran_short = applied && counting_ && counted.underruns > underruns_;
  underruns_ = counted.underruns;
  counting_ = applied;
  if (ran_short) {
    if (!raise_played_ ||
        counted.played_frames - *raise_played_ >= raise_every_) {
      raised_ = std::min(raised_ + kAudioRaise, kAudioNeedMost);
      raised_at_ = now;
      raise_played_ = counted.played_frames;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++stats_.underruns;
    }
    unplace();
  } else if (raised_ > nanoseconds(0) && now - raised_at_ >= kLiveWindow) {
    raised_ = std::max(raised_ - kAudioRaise, nanoseconds(0));
    raised_at_ = now;
  }
  const nanoseconds need = audio_need(period_, latency, raised_);
  need_ns_.store(need.count(), std::memory_order_relaxed);
  late_ns_.store(std::max(need - kMaxLiveDelay, nanoseconds(0)).count(),
                 std::memory_order_relaxed);
  const std::optional<nanoseconds> point = audio_point();
  if (!placed_ || !applied || !point) {
    set_playing(false);
    return;
  }
  // Follow the clock: its point's change, shifted in once it reaches
  // kFollowBand, so the slew's and the offset's slight moves add up rather
  // than each fading the audio; past kAudioReach, placed afresh.
  const nanoseconds moved = *point - placed_point_;
  if (magnitude(moved) > kAudioReach) {
    unplace();
    return;
  }
  if (magnitude(moved) >= kFollowBand) {
    const std::int64_t frames = frames_of(moved);
    out.shift(frames);
    const nanoseconds shifted(static_cast<std::int64_t>(
        audio_ns(static_cast<std::uint64_t>(frames < 0 ? -frames : frames),
                 track_->params.sample_rate)));
    placed_point_ += frames < 0 ? -shifted : shifted;
    shifted_at_ = now;
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.follows;
  }
  if (out.shifting() != 0) {
    shifted_at_ = now;
  }
  // What is heard against the clock, once every shift has been heard.
  const std::optional<std::uint64_t> heard = out.position();
  if (!heard) {
    set_playing(false);
    return;
  }
  const nanoseconds ahead(static_cast<std::int64_t>(*heard) -
                          pts_at(*point, now));
  if (now - shifted_at_ >= latency + kShiftSettle) {
    if (magnitude(ahead) > kAudioReach) {
      unplace();  // out of step, as when its device stopped
      return;
    }
    if (const nanoseconds trim = drift_trim(ahead); trim != nanoseconds(0)) {
      out.shift(frames_of(trim));
      shifted_at_ = now;
      std::lock_guard<std::mutex> lock(mutex_);
      ++stats_.trims;
    }
  }
  set_playing(magnitude(ahead) <= kAudioReach);
}

std::optional<std::int64_t> LiveAudio::heard_pts(
    Clock::time_point now) const noexcept {
  if (!playing_.load(std::memory_order_acquire)) {
    return std::nullopt;
  }
  const std::optional<nanoseconds> point = clock_.point();
  std::lock_guard<std::mutex> lock(mutex_);
  if (out_ == nullptr || !point) {
    return std::nullopt;
  }
  const std::optional<std::uint64_t> position = out_->position();
  if (!position || out_->queued() == 0) {
    return std::nullopt;  // run dry: its position stalls
  }
  const std::int64_t heard = static_cast<std::int64_t>(*position) +
                             (now - now_()).count() + late().count();
  const std::int64_t clock_pts = pts_at(*point, now);
  if (magnitude(nanoseconds(heard - clock_pts)) > kAudioReach) {
    return std::nullopt;
  }
  return heard;
}

std::optional<std::uint64_t> LiveAudio::position() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return out_ != nullptr ? out_->position() : std::nullopt;
}

LiveAudioStats LiveAudio::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

void LiveAudio::set_gain(float gain) {
  std::lock_guard<std::mutex> lock(mutex_);
  gain_ = gain;
  if (out_ != nullptr) {
    out_->set_gain(gain);
  }
}

void LiveAudio::park() {
  std::lock_guard<std::mutex> lock(mutex_);
  parked_ = true;
  if (out_ != nullptr) {
    out_->park();
  }
}

void LiveAudio::unpark() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (parked_ && out_ != nullptr) {
    out_->unpark();
  }
  parked_ = false;
}

}  // namespace volumetric_kit::player
