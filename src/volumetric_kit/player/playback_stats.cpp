// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/playback_stats.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace volumetric_kit::player {
namespace {

// The span the rate is taken over.
constexpr std::chrono::seconds kRateSpan{1};

// The frames of `period` in a second, rounded, and at least 1.
std::size_t frames_a_second(std::chrono::nanoseconds period) {
  const double frames =
      std::chrono::duration<double>(std::chrono::seconds(1)) / period;
  return std::max<std::size_t>(1,
                               static_cast<std::size_t>(std::lround(frames)));
}

}  // namespace

PlaybackStats::PlaybackStats(const recon::stream::TrackStats& track,
                             recon::stream::Rate rate)
    : track_(track) {
  const std::optional<std::chrono::nanoseconds> period =
      nominal_period(rate.num, rate.den);
  if (period) {
    nominal_fps_ = static_cast<double>(rate.num) / rate.den;
  }
  window_ = frames_a_second(period.value_or(kFallbackPeriod));
}

void PlaybackStats::add_frame(const FrameStamp& stamp, Clock::time_point now) {
  while (!taken_.empty() && now - taken_.front() >= kRateSpan) {
    taken_.pop_front();
  }
  taken_.push_back(now);
  if (stages_.size() == window_) {
    stages_.pop_front();
  }
  stages_.push_back(stamp.stages.rows());
}

std::optional<double> PlaybackStats::mean_mbit_per_s() const noexcept {
  if (track_.records < 2 || track_.max_pts_ns <= track_.min_pts_ns) {
    return std::nullopt;
  }
  const double records = static_cast<double>(track_.records);
  const double bits_a_frame =
      8.0 * static_cast<double>(track_.payload_bytes) / records;
  const double step_s =
      static_cast<double>(track_.max_pts_ns - track_.min_pts_ns) * 1e-9 /
      (records - 1.0);
  return bits_a_frame / step_s * 1e-6;
}

double PlaybackStats::frames_per_s(Clock::time_point now) const {
  const auto first = std::find_if(
      taken_.begin(), taken_.end(),
      [&](Clock::time_point taken) { return now - taken < kRateSpan; });
  const auto n = static_cast<double>(taken_.end() - first);
  if (n < 2.0 || taken_.back() == *first) {
    return 0.0;
  }
  return (n - 1.0) /
         std::chrono::duration<double>(taken_.back() - *first).count();
}

std::vector<core::StageRow> PlaybackStats::stage_means() const {
  if (stages_.empty()) {
    return {};
  }
  std::vector<core::StageRow> means = stages_.back();
  for (core::StageRow& mean : means) {
    double cpu_ms = 0.0;
    double gpu_ms = 0.0;
    int gpu_frames = 0;
    for (const std::vector<core::StageRow>& frame : stages_) {
      for (const core::StageRow& row : frame) {
        if (std::strcmp(row.name, mean.name) != 0) {
          continue;
        }
        cpu_ms += row.cpu_ms;
        if (row.has_gpu) {
          gpu_ms += row.gpu_ms;
          ++gpu_frames;
        }
      }
    }
    mean.cpu_ms = cpu_ms / static_cast<double>(stages_.size());
    mean.has_gpu = gpu_frames > 0;
    mean.gpu_ms = mean.has_gpu ? gpu_ms / gpu_frames : 0.0;
  }
  return means;
}

}  // namespace volumetric_kit::player
