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

// The frames of `period` in a second, rounded, from 1 to kMaxWindow; a period
// of 0, from a rate past a billion, takes the most.
std::size_t frames_a_second(std::chrono::nanoseconds period) {
  const double frames =
      std::chrono::duration<double>(std::chrono::seconds(1)) / period;
  return static_cast<std::size_t>(std::lround(
      std::clamp(frames, 1.0, static_cast<double>(PlaybackStats::kMaxWindow))));
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
  while (!taken_.empty() && now - taken_.front().at >= kRateSpan) {
    taken_.pop_front();
  }
  taken_.push_back({now, stamp.records, stamp.record_bytes});
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
  return per_second(now, [](const Taken&) { return 1.0; }).value_or(0.0);
}

std::optional<double> PlaybackStats::rolling_mbit_per_s(
    Clock::time_point now) const {
  const std::optional<double> bits = per_second(now, [](const Taken& taken) {
    return 8.0 * static_cast<double>(taken.bytes);
  });
  if (!bits) {
    return std::nullopt;
  }
  return *bits * 1e-6;
}

double PlaybackStats::records_per_s(Clock::time_point now) const {
  return per_second(now,
                    [](const Taken& taken) {
                      return static_cast<double>(taken.records);
                    })
      .value_or(0.0);
}

std::optional<double> PlaybackStats::per_second(
    Clock::time_point now, double (*value)(const Taken&)) const {
  const auto first = std::find_if(
      taken_.begin(), taken_.end(),
      [&](const Taken& taken) { return now - taken.at < kRateSpan; });
  if (taken_.end() - first < 2 || taken_.back().at == first->at) {
    return std::nullopt;
  }
  double sum = 0.0;
  for (auto taken = first + 1; taken != taken_.end(); ++taken) {
    sum += value(*taken);
  }
  return sum /
         std::chrono::duration<double>(taken_.back().at - first->at).count();
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
