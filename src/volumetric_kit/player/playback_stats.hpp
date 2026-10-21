// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file playback_stats.hpp
/// @brief The stats panel's figures, over the frames taken for drawing.
///        Player-internal.

#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

#include "volumetric_kit/core/base/stage_metrics.hpp"
#include "volumetric_kit/player/playback.hpp"
#include "volumetric_kit/player/stream_player.hpp"
#include "volumetric_kit/recon/stream/session.hpp"

namespace volumetric_kit::player {

/// @brief Figures over the frames taken for drawing: the track's mean
///        bitrate, the rate frames are taken at, and the worker's stage
///        means. Host-only, with the time passed in, so a test never sleeps.
///        Not thread-safe: the drawing thread owns it.
///
/// @code
/// PlaybackStats stats(player.track_stats(), player.rate());
/// exchange.begin_frame(slot, [&](const DeviceMesh&, FrameStamp& stamp) {
///   stats.add_frame(stamp, PlaybackStats::Clock::now());
///   return true;
/// });
/// const std::vector<core::StageRow> rows = stats.stage_means();
/// @endcode
class PlaybackStats {
 public:
  using Clock = Playback::Clock;

  /// @param track  The track's counts, for @ref mean_mbit_per_s.
  /// @param rate   Its nominal rate. The stage means cover its frames a
  ///               second, rounded, and at least 1: 30 when unknown
  ///               (@ref kFallbackPeriod).
  PlaybackStats(const recon::stream::TrackStats& track,
                recon::stream::Rate rate);

  /// @brief A frame taken for drawing (MeshExchange's commit), at @p now.
  /// @param stamp  Its stamp; its rows are copied.
  /// @param now    When it was taken; no earlier than the last frame's.
  void add_frame(const FrameStamp& stamp, Clock::time_point now);

  /// @return The track's Mbit/s (10^6 bit/s): 8 x payload_bytes / records
  ///         per frame, over the mean pts step (max - min) / (records - 1).
  ///         Empty below 2 records or for a pts span of 0.
  std::optional<double> mean_mbit_per_s() const noexcept;
  /// @return The nominal frames a second; empty when unknown.
  std::optional<double> nominal_fps() const noexcept { return nominal_fps_; }
  /// @return Frames taken a second over those taken in the second up to
  ///         @p now: (n - 1) over the time from the first to the last; 0
  ///         below 2, or for all taken at once.
  double frames_per_s(Clock::time_point now) const;
  /// @return Each row of the newest frame's stamp, in its order, as its mean
  ///         over the window: CPU over every frame in it, GPU over those
  ///         whose row has a device span (`has_gpu` false with none). Empty
  ///         before the first frame.
  std::vector<core::StageRow> stage_means() const;

 private:
  recon::stream::TrackStats track_;
  std::optional<double> nominal_fps_;
  std::size_t window_ = 0;               // frames the stage means cover
  std::deque<Clock::time_point> taken_;  // within a second of the newest
  std::deque<std::vector<core::StageRow>> stages_;  // the window's, newest last
};

}  // namespace volumetric_kit::player
