// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/playback_stats.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>

namespace volumetric_kit::player {
namespace {

// The span the rate is taken over.
constexpr std::chrono::seconds kRateSpan{1};

// From `from` to `to`, in ms, when both are known and in order.
std::optional<double> leg_ms(
    PlaybackStats::Clock::time_point from,
    std::optional<PlaybackStats::Clock::time_point> to) {
  if (from == PlaybackStats::Clock::time_point{} || !to || *to < from) {
    return std::nullopt;
  }
  return std::chrono::duration<double, std::milli>(*to - from).count();
}

// The frames of `period` in a second, rounded, from 1 to kMaxWindow; a period
// of 0, from a rate past a billion, takes the most.
std::size_t frames_a_second(std::chrono::nanoseconds period) {
  const double frames =
      std::chrono::duration<double>(std::chrono::seconds(1)) / period;
  return static_cast<std::size_t>(std::lround(
      std::clamp(frames, 1.0, static_cast<double>(PlaybackStats::kMaxWindow))));
}

}  // namespace

FrameMeans frame_figures(const FrameStamp& stamp) {
  FrameMeans figures;
  // Its measured stage rows, breakdowns aside, whose tiles add up to its
  // batch.
  const bool measured =
      std::any_of(stamp.stages.rows().begin(), stamp.stages.rows().end(),
                  [](const core::StageRow& row) {
                    return row.device == core::DeviceTime::Measured;
                  });
  if (stamp.timed && measured) {
    figures.gpu_ms = stamp.stages.total_gpu_ms();
  }
  figures.device_idle_ms = stamp.device_idle_ms;
  figures.queue_ms = stamp.queue_ms;
  figures.publish_ms = leg_ms(stamp.due, stamp.published);
  if (stamp.taken) {
    figures.take_ms = leg_ms(stamp.published, stamp.taken);
    figures.present_ms = leg_ms(*stamp.taken, stamp.presented);
  }
  return figures;
}

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
  if (stamp.rebuild) {
    return;  // the frame before, again
  }
  // A frame taken over a second ago was published over a second ago too.
  while (!taken_.empty() && now - taken_.front().at >= kRateSpan) {
    taken_.pop_front();
  }
  taken_.push_back({now, stamp.published, stamp.records, stamp.record_bytes});
  if (stages_.size() == window_) {
    stages_.pop_front();
  }
  stages_.push_back({stamp.stages.rows(), stamp.timed, frame_figures(stamp)});
}

FrameMeans PlaybackStats::frame_means() const {
  FrameMeans means;
  for (std::optional<double> FrameMeans::* figure :
       {&FrameMeans::gpu_ms, &FrameMeans::device_idle_ms, &FrameMeans::queue_ms,
        &FrameMeans::publish_ms, &FrameMeans::take_ms,
        &FrameMeans::present_ms}) {
    double sum = 0.0;
    int frames = 0;
    for (const Stages& frame : stages_) {
      if (frame.figures.*figure) {
        sum += *(frame.figures.*figure);
        ++frames;
      }
    }
    if (frames > 0) {
      means.*figure = sum / frames;
    }
  }
  return means;
}

std::uint32_t PlaybackStats::timed_frames() const noexcept {
  return static_cast<std::uint32_t>(
      std::count_if(stages_.begin(), stages_.end(),
                    [](const Stages& frame) { return frame.timed; }));
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
  return per_second(now, &Taken::at, [](const Taken&) { return 1.0; })
      .value_or(0.0);
}

std::optional<double> PlaybackStats::rolling_mbit_per_s(
    Clock::time_point now) const {
  const std::optional<double> bits =
      per_second(now, &Taken::published, [](const Taken& taken) {
        return 8.0 * static_cast<double>(taken.bytes);
      });
  if (!bits) {
    return std::nullopt;
  }
  return *bits * 1e-6;
}

double PlaybackStats::records_per_s(Clock::time_point now) const {
  return per_second(now, &Taken::published,
                    [](const Taken& taken) {
                      return static_cast<double>(taken.records);
                    })
      .value_or(0.0);
}

std::optional<double> PlaybackStats::per_second(
    Clock::time_point now, Clock::time_point Taken::* when,
    double (*value)(const Taken&)) const {
  const auto first = std::find_if(
      taken_.begin(), taken_.end(),
      [&](const Taken& taken) { return now - taken.*when < kRateSpan; });
  if (taken_.end() - first < 2 || taken_.back().*when == (*first).*when) {
    return std::nullopt;
  }
  double sum = 0.0;
  for (auto taken = first + 1; taken != taken_.end(); ++taken) {
    sum += value(*taken);
  }
  return sum /
         std::chrono::duration<double>(taken_.back().*when - (*first).*when)
             .count();
}

std::vector<core::StageRow> PlaybackStats::stage_means() const {
  // The names in order: one a frame brings first goes after the row it
  // follows there, so a breakdown stays under its stage.
  std::vector<const char*> names;
  for (const Stages& frame : stages_) {
    std::size_t after = 0;
    for (const core::StageRow& row : frame.rows) {
      const auto found = std::find_if(
          names.begin(), names.end(),
          [&](const char* name) { return std::strcmp(name, row.name) == 0; });
      if (found == names.end()) {
        names.insert(names.begin() + static_cast<std::ptrdiff_t>(after),
                     row.name);
        ++after;
      } else {
        after = static_cast<std::size_t>(found - names.begin()) + 1;
      }
    }
  }
  std::vector<core::StageRow> means;
  means.reserve(names.size());
  for (const char* name : names) {
    core::StageRow mean;
    mean.name = name;
    int frames = 0;
    int measured = 0;
    for (const Stages& frame : stages_) {
      for (const core::StageRow& row : frame.rows) {
        if (std::strcmp(row.name, name) != 0) {
          continue;
        }
        ++frames;
        mean.cpu_ms += row.cpu_ms;
        mean.wait_ms += row.wait_ms;
        // The states rank, as core::StageMetrics folds them.
        mean.device = std::max(mean.device, row.device);
        if (row.device == core::DeviceTime::Measured) {
          mean.gpu_ms += row.gpu_ms;
          ++measured;
        }
      }
    }
    mean.cpu_ms /= frames;
    mean.wait_ms /= frames;
    mean.gpu_ms = measured > 0 ? mean.gpu_ms / measured : 0.0;
    mean.has_gpu = mean.device == core::DeviceTime::Measured;
    means.push_back(mean);
  }
  return means;
}

}  // namespace volumetric_kit::player
