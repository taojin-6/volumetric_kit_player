// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file playback_stats.hpp
/// @brief The stats panel's figures, over the frames taken for drawing.
///        Player-internal.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "volumetric_kit/core/base/stage_metrics.hpp"
#include "volumetric_kit/grid/stream/session.hpp"
#include "volumetric_kit/player/playback.hpp"
#include "volumetric_kit/player/stream_player.hpp"

namespace volumetric_kit::player {

/// @brief A frame's figures beside its stage rows, in ms, each empty where
///        the frame lacks it: one frame's (@ref frame_figures), or a
///        window's, each a mean over the frames that have it
///        (PlaybackStats::frame_means).
///
/// @code
/// const FrameFigures means = stats.frame_means();
/// if (means.latency_ms) {
///   std::printf("due -> presented %.2f ms\n", *means.latency_ms);
/// }
/// @endcode
struct FrameFigures {
  /// A timed frame's device time: its measured stage rows, breakdowns aside,
  /// whose tiles add up to its batch.
  std::optional<double> gpu_ms;
  std::optional<double> device_idle_ms;  ///< FrameStamp::device_idle_ms.
  std::optional<double> queue_ms;        ///< FrameStamp::queue_ms.
  /// Its latency, FrameStamp::due to presented: only a frame with all three
  /// legs below has it.
  std::optional<double> latency_ms;
  std::optional<double> publish_ms;  ///< FrameStamp::due to published.
  std::optional<double> take_ms;     ///< Published to taken.
  std::optional<double> present_ms;  ///< Taken to presented.
};

/// @brief One frame's figures: what PlaybackStats keeps of each frame and
///        MeasureLog writes in its frame row.
///
/// @code
/// const FrameFigures one = frame_figures(stamp);  // as the frame has each
/// @endcode
/// @param stamp  The frame's stamp.
/// @return Its device time when timed and measured, its queue idle and
///         latency, each latency leg whose ends it has, in order, and with
///         all three its latency.
FrameFigures frame_figures(const FrameStamp& stamp);

/// @brief Figures over the frames taken for drawing: the track's mean
///        bitrate, the rate frames are taken at, the worker's stage means,
///        and for a live stream, which has no track mean, the bitrate and
///        the records over the last second. Host-only, with the time passed
///        in, so a test never sleeps. Not thread-safe: the drawing thread
///        owns it.
///
/// @code
/// PlaybackStats stats(player.track_stats(), player.rate());
/// exchange.begin_frame(slot, [&](const DeviceMesh&, FrameStamp& stamp) {
///   stats.add_frame(stamp, PlaybackStats::Clock::now());
///   player.taken();
///   return true;
/// });
/// const std::vector<core::StageRow> rows = stats.stage_means();
/// @endcode
class PlaybackStats {
 public:
  using Clock = Playback::Clock;

  /// The most frames the stage means cover, so a corrupt rate cannot grow
  /// the window without bound.
  static constexpr std::size_t kMaxWindow = 240;

  /// @param track  The track's counts, for @ref mean_mbit_per_s.
  /// @param rate   Its nominal rate. The stage means cover its frames a
  ///               second, rounded, from 1 to @ref kMaxWindow: 30 when
  ///               unknown (@ref kFallbackPeriod).
  PlaybackStats(const grid::stream::TrackStats& track, grid::stream::Rate rate);

  /// @brief A frame taken for drawing (MeshExchange's commit), at @p now,
  ///        added once presented (or with no present); a rebuild
  ///        (FrameStamp::rebuild) is the frame before again, so it changes
  ///        nothing.
  /// @param stamp  Its stamp; its rows and figures are copied.
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
  /// @return The Mbit/s the records brought over the second up to @p now, by
  ///         when their frames were published (`FrameStamp::published`), not
  ///         taken: 8 x the `record_bytes` of each frame published in it but
  ///         the first, over the time from the first's publish to the last's;
  ///         empty below 2, or for all published at once.
  std::optional<double> rolling_mbit_per_s(Clock::time_point now) const;
  /// @return The records a second, as @ref rolling_mbit_per_s: their frames'
  ///         `records` in place of their bytes; 0 where it is empty.
  double records_per_s(Clock::time_point now) const;
  /// @return Each row of the window's stamps as its mean over the frames
  ///         that have it: in their order, a row only some frames have (a
  ///         timed frame's breakdowns) after the row it follows there. Host
  ///         and wait over each of those frames; the device half as
  ///         `core::StageMetrics` folds one, its state the highest of theirs
  ///         (`core::DeviceTime`), so measured only when every frame that
  ///         timed it measured it, and its time over those measured. Empty
  ///         before the first frame.
  std::vector<core::StageRow> stage_means() const;
  /// @return The window's per-frame figures (@ref FrameFigures), each a mean
  ///         over the frames that have it.
  FrameFigures frame_means() const;
  /// @return The frames the stage means are taken over: the window's.
  std::uint32_t window_frames() const noexcept {
    return static_cast<std::uint32_t>(stages_.size());
  }
  /// @return Of them, those whose device work was timed (`FrameStamp::timed`).
  std::uint32_t timed_frames() const noexcept;

 private:
  // A frame taken: when, when it was published, and its records' count and
  // bytes.
  struct Taken {
    Clock::time_point at;
    Clock::time_point published;
    std::uint64_t records = 0;
    std::uint64_t bytes = 0;
  };

  // The sum of `value` over the frames whose `when` is in the second up to
  // `now`, each but the first, a second from the first's `when` to the
  // last's; empty below 2, or for all at once.
  std::optional<double> per_second(Clock::time_point now,
                                   Clock::time_point Taken::* when,
                                   double (*value)(const Taken&)) const;

  // A frame's rows, whether its device work was timed, and its figures.
  struct Stages {
    std::vector<core::StageRow> rows;
    bool timed = false;
    FrameFigures figures;
  };

  grid::stream::TrackStats track_;
  std::optional<double> nominal_fps_;
  std::size_t window_ = 0;     // frames the stage means cover
  std::deque<Taken> taken_;    // within a second of the newest
  std::deque<Stages> stages_;  // the window's, newest last
};

}  // namespace volumetric_kit::player
