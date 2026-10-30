// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/audio_feeder.hpp"

#include <algorithm>
#include <utility>

#include "volumetric_kit/core/base/exception_status.hpp"

namespace volumetric_kit::player {
namespace {

namespace stream = grid::stream;

std::uint64_t ns_of(std::chrono::milliseconds ms) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(ms).count());
}

}  // namespace

std::optional<AudioTrackChoice> find_audio_track(
    const stream::Session& session,
    const std::vector<stream::TrackStats>& stats, std::uint16_t geometry) {
  if (geometry >= session.tracks.size()) {
    return std::nullopt;
  }
  const std::uint16_t clock = session.tracks[geometry].clock;
  for (std::size_t i = 0; i < session.tracks.size() && i < stats.size(); ++i) {
    const stream::Track& track = session.tracks[i];
    if (track.kind != stream::TrackKind::Audio || track.clock != clock ||
        stats[i].records == 0) {
      continue;
    }
    const core::Result<stream::AudioParams> params =
        stream::decode_audio_params(track.params);
    if (!params.ok() || params->codec != stream::AudioCodec::PcmS16 ||
        params->sample_rate < kMinSampleRate ||
        params->sample_rate > kMaxSampleRate) {
      continue;
    }
    return AudioTrackChoice{static_cast<std::uint16_t>(i), *params};
  }
  return std::nullopt;
}

std::uint16_t played_channels(std::uint16_t channels) noexcept {
  return std::min<std::uint16_t>(channels, 2);
}

AudioFormat played_format(const stream::AudioParams& params) noexcept {
  return {params.sample_rate, played_channels(params.channels)};
}

void to_played(const std::uint8_t* pcm, std::size_t frames,
               std::uint16_t channels, float* out) noexcept {
  const std::uint16_t played = played_channels(channels);
  for (std::size_t f = 0; f < frames; ++f) {
    const std::uint8_t* const frame = pcm + f * channels * 2;
    for (std::uint16_t c = 0; c < played; ++c) {
      int sample = frame[2 * c] | (frame[2 * c + 1] << 8);
      if (sample >= 32768) {
        sample -= 65536;
      }
      *out++ = static_cast<float>(sample) / 32768.0f;
    }
  }
}

Placement place(std::uint64_t running_ns, std::uint64_t pts_ns,
                std::uint64_t frames, std::uint32_t sample_rate,
                bool exact) noexcept {
  const std::uint64_t chunk = exact ? 0 : frames;
  Placement at;
  if (pts_ns >= running_ns) {
    const std::uint64_t ahead = audio_frames(pts_ns - running_ns, sample_rate);
    if (ahead > chunk) {
      at.silence = ahead;
    }
  } else {
    const std::uint64_t behind = audio_frames(running_ns - pts_ns, sample_rate);
    if (behind > chunk) {
      at.skip = std::min(behind, frames);
    }
  }
  return at;
}

core::Result<std::unique_ptr<AudioFeeder>> AudioFeeder::start(
    const stream::StreamReader& reader, const AudioTrackChoice& track,
    std::uint64_t start_pts_ns, AudioOut& out) {
  try {
    std::unique_ptr<AudioFeeder> feeder(
        new AudioFeeder(reader, track, start_pts_ns, out));
    AudioFeeder* const self = feeder.get();
    feeder->thread_ = std::thread([self] { self->run(); });
    return feeder;
  } catch (...) {
    return core::exception_status("AudioFeeder::start",
                                  core::Status::Code::IoError);
  }
}

AudioFeeder::AudioFeeder(const stream::StreamReader& reader,
                         const AudioTrackChoice& track,
                         std::uint64_t start_pts_ns, AudioOut& out)
    : reader_(reader),
      track_(track),
      start_pts_ns_(start_pts_ns),
      out_(out),
      frame_bytes_(std::size_t{2} * track.params.channels),
      played_(played_channels(track.params.channels)),
      low_(audio_frames(ns_of(kFeedLow), track.params.sample_rate)),
      high_(audio_frames(ns_of(kFeedHigh), track.params.sample_rate)) {}

AudioFeeder::~AudioFeeder() { stop(); }

void AudioFeeder::restart() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    restart_ = true;
  }
  changed_.notify_all();
}

void AudioFeeder::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
  }
  changed_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

AudioFeedStats AudioFeeder::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

void AudioFeeder::run() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!quit_) {
    const bool restart = std::exchange(restart_, false);
    lock.unlock();
    try {
      if (restart) {
        begin();
      }
      feed();
    } catch (...) {
      ended_ = true;  // out of memory for a record: the pass ends
    }
    lock.lock();
    stats_.ended = ended_;
    changed_.wait_for(lock, kFeedEvery, [this] { return quit_ || restart_; });
  }
}

void AudioFeeder::begin() {
  out_.flush(start_pts_ns_);
  pos_ = reader_.begin();
  running_ = 0;
  exact_ = true;
  ended_ = false;
  silence_ = 0;
  samples_.clear();
  taken_ = 0;
  std::lock_guard<std::mutex> lock(mutex_);
  ++stats_.passes;
  stats_.ended = false;
}

void AudioFeeder::feed() {
  if (out_.queued() >= low_) {
    return;
  }
  while (out_.queued() < high_) {
    if (!drain() || ended_) {
      return;
    }
    next_record();
  }
}

bool AudioFeeder::drain() {
  while (silence_ > 0) {
    const std::size_t want = static_cast<std::size_t>(
        std::min<std::uint64_t>(silence_, out_.capacity()));
    const std::size_t wrote = out_.write(nullptr, want);
    silence_ -= wrote;
    if (wrote < want) {
      return false;
    }
  }
  const std::size_t left = samples_.size() / played_ - taken_;
  const std::size_t wrote =
      out_.write(samples_.data() + taken_ * played_, left);
  taken_ += wrote;
  return wrote == left;
}

void AudioFeeder::next_record() {
  const std::uint32_t rate = track_.params.sample_rate;
  for (;;) {
    stream::Record record;
    const core::Result<bool> more = reader_.next(&pos_, &record);
    if (!more.ok()) {
      const core::Status::Code code = more.status().domain();
      if (code == core::Status::Code::InvalidArgument ||
          code == core::Status::Code::Unsupported) {
        continue;  // damage: the cursor is past it
      }
      ended_ = true;  // the file cannot be read on: the pass ends
      return;
    }
    if (!*more) {
      ended_ = true;
      return;
    }
    if (record.info.track != track_.index) {
      continue;
    }
    const std::uint64_t frames = record.size / frame_bytes_;
    if (record.size % frame_bytes_ != 0 ||
        !reader_.read(record, &payload_).ok()) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++stats_.skipped;
      continue;
    }
    const Placement at = place(start_pts_ns_ + audio_ns(running_, rate),
                               record.info.pts_ns, frames, rate, exact_);
    if (at.skip == frames) {  // wholly before the running position
      std::lock_guard<std::mutex> lock(mutex_);
      stats_.dropped_frames += frames;
      continue;
    }
    const std::uint64_t kept = frames - at.skip;
    samples_.resize(static_cast<std::size_t>(kept) * played_);
    to_played(payload_.data() + at.skip * frame_bytes_,
              static_cast<std::size_t>(kept), track_.params.channels,
              samples_.data());
    taken_ = 0;
    silence_ = at.silence;
    running_ += at.silence + kept;
    exact_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.records;
    stats_.silence_frames += at.silence;
    stats_.dropped_frames += at.skip;
    return;
  }
}

}  // namespace volumetric_kit::player
