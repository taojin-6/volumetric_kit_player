// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/audio_clock.hpp"

#include <algorithm>
#include <cmath>

namespace volumetric_kit::player {
namespace {

// Seconds that are not finite, or negative, count as 0.
double seconds_or_zero(double seconds) {
  return std::isfinite(seconds) && seconds > 0.0 ? seconds : 0.0;
}

// When what is heard at host time `now` passed the device's output, for a
// run of `latency_ns`.
std::uint64_t passed_at(std::uint64_t now, std::uint64_t latency_ns) {
  return now > latency_ns ? now - latency_ns : 0;
}

// The pts of `run` at the device's output at host time `at`: its end less
// what is still to come, clamped to the run.
std::uint64_t pts_in(const HeardRun& run, std::uint64_t at) {
  if (at >= run.end_host_ns) {
    return run.end_pts_ns;
  }
  const std::uint64_t to_come = run.end_host_ns - at;
  const std::uint64_t span = run.end_pts_ns - run.first_pts_ns;
  return to_come >= span ? run.first_pts_ns : run.end_pts_ns - to_come;
}

}  // namespace

std::uint64_t host_ticks_to_ns(std::uint64_t ticks, std::uint32_t numer,
                               std::uint32_t denom) noexcept {
  // In two parts, so neither product overflows.
  return ticks / denom * numer + ticks % denom * numer / denom;
}

std::uint64_t output_latency_ns(double presentation_s,
                                double session_s) noexcept {
  const double seconds =
      seconds_or_zero(presentation_s) + seconds_or_zero(session_s);
  return static_cast<std::uint64_t>(std::llround(seconds * 1e9));
}

bool reanchors(const HeardClock& clock, const Rendered& rendered,
               const RenderTime& time) noexcept {
  if (rendered.frames == 0) {
    return false;
  }
  if (rendered.flushed || time.restarted || !clock.current || !clock.open ||
      rendered.pts_ns != clock.current->end_pts_ns) {
    return true;
  }
  const std::uint64_t end = clock.current->end_host_ns;
  const std::uint64_t gap =
      time.host_ns > end ? time.host_ns - end : end - time.host_ns;
  return gap > kFollowNs;
}

HeardClock after_render(const HeardClock& clock, const Rendered& rendered,
                        const RenderTime& time,
                        std::uint32_t sample_rate) noexcept {
  if (!rendered.based) {
    return clock;
  }
  HeardClock next = clock;
  if (rendered.flushed) {
    next = HeardClock{};
    next.based = true;
    next.floor_pts_ns = rendered.pts_ns;
  }
  if (rendered.frames == 0) {
    next.open = false;
    return next;
  }
  const std::uint64_t end_host =
      time.host_ns + audio_ns(rendered.frames, sample_rate);
  if (reanchors(clock, rendered, time)) {
    next.previous = next.current;
    next.current = HeardRun{rendered.pts_ns, rendered.end_pts_ns, end_host,
                            time.latency_ns};
  } else {
    // Each render's own time, so the run keeps the device's rate.
    next.current->end_pts_ns = rendered.end_pts_ns;
    next.current->end_host_ns = end_host;
  }
  next.open = rendered.frames == time.frames;
  return next;
}

std::optional<std::uint64_t> heard_pts(const HeardClock& clock,
                                       std::uint64_t now_host_ns) noexcept {
  if (!clock.based) {
    return std::nullopt;
  }
  if (!clock.current) {
    return clock.floor_pts_ns;
  }
  const HeardRun& run = *clock.current;
  const std::uint64_t at = passed_at(now_host_ns, run.latency_ns);
  const std::uint64_t span = run.end_pts_ns - run.first_pts_ns;
  // Before the current run's first frame is heard, the previous run's
  // frames play, each run at the latency it began with.
  if (clock.previous && at + span < run.end_host_ns) {
    const HeardRun& before = *clock.previous;
    return pts_in(before, passed_at(now_host_ns, before.latency_ns));
  }
  return pts_in(run, at);
}

std::optional<ClockFit> clock_fit(const std::vector<ClockSample>& samples) {
  if (samples.size() < 2) {
    return std::nullopt;
  }
  // About the first reading, so the sums keep their precision.
  const ClockSample& origin = samples.front();
  double mean_at = 0.0;
  double mean_value = 0.0;
  for (const ClockSample& sample : samples) {
    mean_at += static_cast<double>(sample.at_ns - origin.at_ns);
    mean_value += static_cast<double>(sample.value_ns - origin.value_ns);
  }
  mean_at /= static_cast<double>(samples.size());
  mean_value /= static_cast<double>(samples.size());
  double sxx = 0.0;
  double sxy = 0.0;
  for (const ClockSample& sample : samples) {
    const double x = static_cast<double>(sample.at_ns - origin.at_ns) - mean_at;
    const double y =
        static_cast<double>(sample.value_ns - origin.value_ns) - mean_value;
    sxx += x * x;
    sxy += x * y;
  }
  if (sxx == 0.0) {
    return std::nullopt;
  }
  ClockFit fit;
  fit.rate = sxy / sxx;
  for (const ClockSample& sample : samples) {
    const double x = static_cast<double>(sample.at_ns - origin.at_ns) - mean_at;
    const double y =
        static_cast<double>(sample.value_ns - origin.value_ns) - mean_value;
    fit.max_residual_ns =
        std::max(fit.max_residual_ns, std::abs(y - fit.rate * x));
  }
  return fit;
}

}  // namespace volumetric_kit::player
