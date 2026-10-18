// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/playback.hpp"

namespace volumetric_kit::player {
namespace {

constexpr std::int64_t kNsPerSecond = 1'000'000'000;
constexpr std::uint32_t kFallbackFps = 30;
// The longest pts gap honoured; a longer one, as from a corrupt stream,
// would stall playback (or overflow the due time).
constexpr std::uint64_t kMaxGapNs = kNsPerSecond;

std::chrono::nanoseconds nominal_period(std::uint32_t num, std::uint32_t den) {
  if (num == 0 || den == 0) {
    return std::chrono::nanoseconds(kNsPerSecond / kFallbackFps);
  }
  return std::chrono::nanoseconds(static_cast<std::int64_t>(den) *
                                  kNsPerSecond / num);
}

}  // namespace

Playback::Playback(std::uint32_t rate_num, std::uint32_t rate_den)
    : period_(nominal_period(rate_num, rate_den)) {}

void Playback::play() {
  if (playing_) {
    return;
  }
  playing_ = true;
  steps_ = 0;
  anchored_ = false;
}

void Playback::pause() { playing_ = false; }

void Playback::step() {
  playing_ = false;
  ++steps_;
}

std::optional<Playback::Clock::time_point> Playback::due(
    std::uint64_t pts_ns, Clock::time_point now) const {
  if (!playing_) {
    if (steps_ == 0) {
      return std::nullopt;
    }
    return now;
  }
  if (!anchored_) {
    return now;
  }
  if (pts_ns > last_pts_ns_ && pts_ns - last_pts_ns_ <= kMaxGapNs) {
    return last_due_ + std::chrono::nanoseconds(
                           static_cast<std::int64_t>(pts_ns - last_pts_ns_));
  }
  return last_due_ + period_;
}

void Playback::shown(std::uint64_t pts_ns, Clock::time_point now) {
  Clock::time_point at = now;
  if (playing_ && anchored_) {
    const Clock::time_point scheduled = *due(pts_ns, now);
    if (now - scheduled <= period_) {
      at = scheduled;
    }
  }
  if (!playing_ && steps_ > 0) {
    --steps_;
  }
  anchored_ = true;
  last_due_ = at;
  last_pts_ns_ = pts_ns;
}

}  // namespace volumetric_kit::player
