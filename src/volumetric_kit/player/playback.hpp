// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file playback.hpp
/// @brief The clock that paces a stream's frames by their timestamps:
///        play, pause and step, with the time passed in.

#include <chrono>
#include <cstdint>
#include <optional>

namespace volumetric_kit::player {

/// The period taken for an unknown rate: 30 frames a second.
inline constexpr std::chrono::nanoseconds kFallbackPeriod{1'000'000'000 / 30};

/// @brief The nominal period of a track of `num / den` frames a second.
///
/// @code
/// const auto period = nominal_period(30000, 1001).value_or(kFallbackPeriod);
/// @endcode
/// @param num  The rate's numerator.
/// @param den  Its denominator.
/// @return The period, in whole nanoseconds; empty when the rate is unknown
///         (`num` or `den` 0).
std::optional<std::chrono::nanoseconds> nominal_period(std::uint32_t num,
                                                       std::uint32_t den);

/// @brief When each frame of a stream is due, from its pts, like a video
///        player's clock.
///
/// A frame is due as long after the last frame shown was due as its pts is
/// after that frame's. The first frame after construction or @ref play is
/// due at once, so a resume shows the next frame without catching up on the
/// pause. A frame shown more than one nominal period late counts as due when
/// it was shown, so the frames behind it follow it rather than come in a
/// burst. A pts at or before the last one's, as at a loop's first frame or
/// in a stream whose pts do not increase, or more than a second after it, is
/// due one nominal period after the last frame. While paused, nothing is due
/// but one frame per @ref step, each at once.
///
/// The caller passes the time in, so a test never sleeps. It is not
/// thread-safe: a caller that shares it guards it with its own lock.
///
/// @code
/// Playback clock(track.rate.num, track.rate.den);
/// std::unique_lock lock(mutex);
/// for (;;) {
///   const auto now = Playback::Clock::now();
///   const auto due = clock.due(pts_ns, now);
///   if (due && *due <= now) break;
///   if (due) changed.wait_until(lock, *due); else changed.wait(lock);
/// }
/// clock.shown(pts_ns, Playback::Clock::now());
/// @endcode
class Playback {
 public:
  using Clock = std::chrono::steady_clock;

  /// @brief A clock for a track of `rate_num / rate_den` frames a second,
  ///        playing.
  /// @param rate_num The nominal rate's numerator; 0 when unknown, which
  ///        is taken as @ref kFallbackPeriod, as is a `rate_den` of 0.
  /// @param rate_den The nominal rate's denominator.
  Playback(std::uint32_t rate_num, std::uint32_t rate_den);

  /// @brief Plays, or resumes: the next frame is due at once. Drops any
  ///        step not yet taken; does nothing while playing.
  void play();
  /// @brief Pauses: no frame is due until @ref play or @ref step.
  void pause();
  /// @brief Pauses, and lets one more frame through, at once.
  void step();
  /// @return Whether it plays, rather than being paused.
  bool playing() const { return playing_; }

  /// @brief When the frame at `pts_ns` is due.
  /// @param pts_ns The frame's pts, in nanoseconds on its track's clock.
  /// @param now The current time, returned when the frame is due at once.
  /// @return The time it is due, or empty while paused with no step to
  ///         take.
  std::optional<Clock::time_point> due(std::uint64_t pts_ns,
                                       Clock::time_point now) const;

  /// @brief Records that the frame at `pts_ns` was shown at `now`, taking a
  ///        step while paused.
  /// @pre `due(pts_ns, now)` is not empty.
  void shown(std::uint64_t pts_ns, Clock::time_point now);

 private:
  std::chrono::nanoseconds period_;  // The nominal frame period.
  bool playing_ = true;
  int steps_ = 0;  // Steps not yet taken.
  // The last frame shown: when it was due (or shown, if late) and its pts.
  // Empty after construction and play.
  bool anchored_ = false;
  Clock::time_point last_due_{};
  std::uint64_t last_pts_ns_ = 0;
};

}  // namespace volumetric_kit::player
