// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file audio_feeder.hpp
/// @brief A file's audio for an output: which Audio track plays, its samples
///        as the output plays them, where each record goes in time, and the
///        feeder that reads the track ahead into an AudioOut.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/grid/stream/audio.hpp"
#include "volumetric_kit/grid/stream/session.hpp"
#include "volumetric_kit/grid/stream/stream_reader.hpp"
#include "volumetric_kit/player/audio_out.hpp"

namespace volumetric_kit::player {

/// The rates a track may have for the player to play it, in frames a second:
/// the range outputs play.
inline constexpr std::uint32_t kMinSampleRate = 8000;
inline constexpr std::uint32_t kMaxSampleRate = 384000;

/// @brief The Audio track a player plays.
struct AudioTrackChoice {
  std::uint16_t index = 0;           ///< In the session's tracks.
  grid::stream::AudioParams params;  ///< Its params, decoded.
};

/// @brief The Audio track to play beside @p geometry: the first on the
///        geometry track's clock that the player can play. An Audio track on
///        another clock, one whose params do not decode or name a codec
///        other than `PcmS16`, one of a rate outside [@ref kMinSampleRate,
///        @ref kMaxSampleRate] or one with no record is skipped, as is every
///        Audio track after the one chosen.
///
/// @code
/// const std::optional<AudioTrackChoice> audio =
///     find_audio_track(reader.session(), reader.stats(), geometry);
/// @endcode
/// @param session   A file's session.
/// @param stats     Its tracks' counts, by track (`StreamReader::stats`).
/// @param geometry  The geometry track's index.
/// @return The track and its params; none without one.
std::optional<AudioTrackChoice> find_audio_track(
    const grid::stream::Session& session,
    const std::vector<grid::stream::TrackStats>& stats, std::uint16_t geometry);

/// @return The channels an output plays of a track of @p channels: up to
///         the first two.
std::uint16_t played_channels(std::uint16_t channels) noexcept;

/// @return The format an output plays @p params in: their rate, and
///         played_channels of their channels.
AudioFormat played_format(const grid::stream::AudioParams& params) noexcept;

/// @brief Signed 16-bit little-endian frames as an output plays them: each
///        sample over 32768, so -32768 is -1, and of each frame the first
///        played_channels(@p channels) samples: more than two channels are
///        downmixed to the first two.
///
/// @code
/// std::vector<float> out(frames * played_channels(channels));
/// to_played(pcm, frames, channels, out.data());
/// @endcode
/// @param pcm       @p frames frames of @p channels interleaved samples.
/// @param frames    How many.
/// @param channels  Samples a frame; at least 1.
/// @param out       Room for @p frames frames of played_channels(@p channels)
///                  samples.
void to_played(const std::uint8_t* pcm, std::size_t frames,
               std::uint16_t channels, float* out) noexcept;

/// @brief Where a record goes: silence to write before it, or frames of its
///        start to drop.
struct Placement {
  std::uint64_t silence = 0;  ///< Frames of silence before it.
  std::uint64_t skip = 0;     ///< Its first frames dropped; at most all.
};

/// @brief Where a record goes against the running position, the pts of the
///        next frame to write. Within one chunk of it, the record follows on
///        at once, so jitter in the pts moves nothing. Past that, it is
///        placed at its pts: a forward jump (audio lost) is filled with
///        silence, and an overlap is dropped from the record's start.
///
/// @code
/// const Placement at = place(running_ns, info.pts_ns, frames, rate, false);
/// @endcode
/// @param running_ns   The running position, on the stream's clock.
/// @param pts_ns       The record's pts: its first frame's.
/// @param frames       Its frames: one chunk.
/// @param sample_rate  Frames a second; at least 1.
/// @param exact        Place it at its pts, whatever the difference, as the
///                     first record after a flush is.
/// @return The silence before it and its frames dropped, each rounded to
///         whole frames; both 0 for a record that follows on.
Placement place(std::uint64_t running_ns, std::uint64_t pts_ns,
                std::uint64_t frames, std::uint32_t sample_rate,
                bool exact) noexcept;

/// The feeder's lead, in queued time: below kFeedLow it reads the track up
/// to kFeedHigh; and how often it looks.
inline constexpr std::chrono::milliseconds kFeedLow{200};
inline constexpr std::chrono::milliseconds kFeedHigh{500};
inline constexpr std::chrono::milliseconds kFeedEvery{10};

/// @brief What a feeder did.
struct AudioFeedStats {
  std::uint64_t passes = 0;   ///< Passes begun: 1, and one more a restart.
  std::uint64_t records = 0;  ///< Records written, whole or in part.
  /// Records skipped: damaged, unreadable, or not whole frames.
  std::uint64_t skipped = 0;
  std::uint64_t silence_frames = 0;  ///< Written for forward jumps.
  std::uint64_t dropped_frames = 0;  ///< Dropped for overlaps.
  bool ended = false;  ///< This pass has read the track to its end.
};

/// @brief Reads a file's Audio track ahead into an AudioOut, on a thread of
///        its own, with a cursor of its own on the file's reader (which is
///        safe for several).
///
/// Each pass flushes the output to the start pts, the first geometry pts,
/// then reads the track from the file's start, each record converted
/// (@ref to_played) and placed (@ref place) against the running position,
/// the first exactly, so the audio starts at the start pts. It keeps the
/// output between @ref kFeedLow and @ref kFeedHigh ahead, looking every
/// @ref kFeedEvery; what a full ring refuses waits for the next look. At the
/// track's end it waits for @ref restart.
///
/// @code
/// VKC_ASSIGN(std::unique_ptr<AudioFeeder> feeder,
///            AudioFeeder::start(*reader, *audio, first_pts_ns, *out));
/// // ... at a loop:
/// feeder->restart();
/// @endcode
///
/// The reader and the output must outlive it; it is the output's only
/// producer. @ref restart, @ref stop and @ref stats are safe from any thread.
class AudioFeeder {
 public:
  /// @brief Start feeding @p out on a thread of its own.
  /// @param reader        The file's.
  /// @param track         The track to play (find_audio_track).
  /// @param start_pts_ns  Where each pass starts, on the track's clock.
  /// @param out           Where its frames go: of played_format(
  ///                      @p track.params).
  /// @return The feeder; `core::Status::Code::IoError` when its thread
  ///         cannot start.
  static core::Result<std::unique_ptr<AudioFeeder>> start(
      const grid::stream::StreamReader& reader, const AudioTrackChoice& track,
      std::uint64_t start_pts_ns, AudioOut& out);

  /// @brief @ref stop.
  ~AudioFeeder();
  AudioFeeder(const AudioFeeder&) = delete;
  AudioFeeder& operator=(const AudioFeeder&) = delete;
  AudioFeeder(AudioFeeder&&) = delete;
  AudioFeeder& operator=(AudioFeeder&&) = delete;

  /// @brief Begin a new pass at once: flush the output, and read the track
  ///        from the start again.
  void restart();
  /// @brief Stop the thread and join it. Idempotent.
  void stop();
  /// @return What it did.
  AudioFeedStats stats() const;

 private:
  AudioFeeder(const grid::stream::StreamReader& reader,
              const AudioTrackChoice& track, std::uint64_t start_pts_ns,
              AudioOut& out);

  // The thread: a pass at each restart, fed until quit.
  void run();
  // A new pass: the output flushed, the cursor at the file's start.
  void begin();
  // Below kFeedLow, read and write records until kFeedHigh is queued, the
  // ring is full or the track ends.
  void feed();
  // Write what is held, silence first: false while the ring refuses some.
  bool drain();
  // The next record of the track converted and placed into what is held;
  // at the track's end, or an error that is no damage, the pass ends.
  void next_record();

  const grid::stream::StreamReader& reader_;
  const AudioTrackChoice track_;
  const std::uint64_t start_pts_ns_;
  AudioOut& out_;
  const std::size_t frame_bytes_;  // a frame's in the file
  const std::uint16_t played_;     // channels an output frame holds
  const std::uint64_t low_;        // kFeedLow and kFeedHigh, in frames
  const std::uint64_t high_;

  // The thread's: the pass's cursor, the frames written since the flush,
  // whether the next record is placed exactly, and what is held.
  grid::stream::Position pos_;
  std::uint64_t running_ = 0;
  bool exact_ = true;
  bool ended_ = false;
  std::uint64_t silence_ = 0;   // frames of silence still to write
  std::vector<float> samples_;  // a record's frames, converted
  std::size_t taken_ = 0;       // of them, written
  std::vector<std::uint8_t> payload_;

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool quit_ = false;
  bool restart_ = true;  // the first pass
  AudioFeedStats stats_;
  std::thread thread_;
};

}  // namespace volumetric_kit::player
