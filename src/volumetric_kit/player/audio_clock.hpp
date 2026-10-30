// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file audio_clock.hpp
/// @brief What a device output hears when, as pure functions: the host
///        clock's ticks in ns, the output latency, the runs of frames its
///        renders put out and the re-anchor rule between them, the pts heard
///        at a host time, and a clock's rate against another.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "volumetric_kit/player/audio_out.hpp"

namespace volumetric_kit::player {

/// @param ticks  A time on the host clock (Core Audio's host time,
///               `mach_absolute_time`), in its ticks.
/// @param numer  Its timebase's numerator (`mach_timebase_info`).
/// @param denom  Its denominator; at least 1.
/// @return @p ticks in ns, rounded down, with no overflow for any uptime.
std::uint64_t host_ticks_to_ns(std::uint64_t ticks, std::uint32_t numer,
                               std::uint32_t denom) noexcept;

/// @param presentation_s  The output's presentation latency, in seconds
///                        (`AVAudioIONode.presentationLatency`).
/// @param session_s       The route's, in seconds
///                        (`AVAudioSession.outputLatency` on iOS; 0 on
///                        macOS).
/// @return Their sum in ns, rounded to the nearest: from a frame passing the
///         device's output to its being heard. A part that is negative or
///         not finite counts as 0.
std::uint64_t output_latency_ns(double presentation_s,
                                double session_s) noexcept;

/// The most a render's time may be off where its run's frames end for it to
/// follow on (@ref reanchors).
inline constexpr std::uint64_t kFollowNs = 1'000'000;

/// @brief Frames heard back to back: from @ref first_pts_ns to
///        @ref end_pts_ns, the end passing the device's output at
///        @ref end_host_ns, so each frame passes it at end_host_ns less the
///        pts still to come after it, and is heard @ref latency_ns later.
struct HeardRun {
  std::uint64_t first_pts_ns = 0;
  std::uint64_t end_pts_ns = 0;   ///< Past the run's last frame.
  std::uint64_t end_host_ns = 0;  ///< On the host clock.
  /// The route's latency as the run began (@ref output_latency_ns).
  std::uint64_t latency_ns = 0;
};

/// @brief What a device output's renders say about when each pts is heard,
///        since its last flush: the run its renders extend now, and the one
///        before, whose frames may still be heard while it plays out.
struct HeardClock {
  bool based = false;  ///< A flush has been applied.
  /// The last flush's pts: heard until a run's first frame is.
  std::uint64_t floor_pts_ns = 0;
  std::optional<HeardRun> current;
  std::optional<HeardRun> previous;
  bool open = false;  ///< @ref current's last render took all it asked.
};

/// @brief When one render goes out, as the device's render thread is told.
struct RenderTime {
  std::size_t frames = 0;  ///< Asked for.
  /// When its first frame passes the device's output, on the host clock.
  std::uint64_t host_ns = 0;
  bool restarted = false;  ///< The device started since the render before.
  /// The route's latency since the device's last start: a run it begins
  /// keeps it.
  std::uint64_t latency_ns = 0;
};

/// @brief The re-anchor rule: whether a render's frames start a new run
///        rather than extend the current one. They extend it only when the
///        device has not started again since, the run's last render took all
///        it asked for, the pts follow on from the run's end, and the render's
///        time is within @ref kFollowNs of it; a flush starts anew anyway.
///
/// @code
/// const bool anchor = reanchors(clock, rendered, time);
/// @endcode
/// @param clock     As of the render before.
/// @param rendered  What the render took (AudioOut::render).
/// @param time      When it goes out.
/// @return Whether it starts a run; false for a render that took nothing.
bool reanchors(const HeardClock& clock, const Rendered& rendered,
               const RenderTime& time) noexcept;

/// @brief The clock after one render: a flush clears it to its pts; frames
///        taken extend the current run or, as @ref reanchors says, start one
///        at the render's latency, the current becoming the previous; a
///        render that took fewer than it asked for (paused, or short) closes
///        the run.
///
/// @code
/// clock = after_render(clock, render(out, frames), time, rate);
/// @endcode
/// @param clock        As of the render before.
/// @param rendered     What the render took.
/// @param time         When it goes out.
/// @param sample_rate  Frames a second; at least 1.
/// @return The clock after it.
HeardClock after_render(const HeardClock& clock, const Rendered& rendered,
                        const RenderTime& time,
                        std::uint32_t sample_rate) noexcept;

/// @brief The pts heard at a host time: what passed the device's output its
///        run's latency before it. Within a run that is the run's end less
///        what is still to pass, clamped to the run; before the current run's
///        first frame is heard, the previous run's, at its own latency; with
///        no run, the flush's pts. A new route's longer latency therefore
///        never moves it back.
///
/// @code
/// const auto heard = heard_pts(clock, host_now_ns);
/// @endcode
/// @param clock        A device output's clock.
/// @param now_host_ns  The time, on the host clock.
/// @return The pts; none before a flush.
std::optional<std::uint64_t> heard_pts(const HeardClock& clock,
                                       std::uint64_t now_host_ns) noexcept;

/// @brief One reading of a clock against another.
struct ClockSample {
  std::int64_t at_ns = 0;     ///< The reference's, e.g. steady_clock's.
  std::int64_t value_ns = 0;  ///< The clock read, e.g. AudioOut::position.
};

/// @brief A least-squares line through a clock's readings against a
///        reference (@ref clock_fit).
struct ClockFit {
  double rate = 1.0;  ///< Its slope: 1 for a clock that keeps the time.
  /// The reading farthest off the line, in ns: the steps and jitter the
  /// slope averages out.
  double max_residual_ns = 0.0;
};

/// @brief A clock's rate against a reference, and how far its readings
///        stray from that, by a least-squares line through them.
///
/// @code
/// const std::optional<ClockFit> fit = clock_fit(samples);
/// const double ppm = (fit->rate - 1.0) * 1e6;  // its drift
/// @endcode
/// @param samples  Readings, in any order.
/// @return The line's slope and largest residual; none for fewer than two
///         readings at different times.
std::optional<ClockFit> clock_fit(const std::vector<ClockSample>& samples);

}  // namespace volumetric_kit::player
