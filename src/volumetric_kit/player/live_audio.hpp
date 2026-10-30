// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file live_audio.hpp
/// @brief A live stream's audio on the live playout clock (live_clock.hpp):
///        its Audio track's records written into an output as they arrive,
///        placed so the frame of pts p is heard at present(p); the clock's
///        point followed, and the output's drift trimmed, by shifts
///        (AudioOut::shift); what is heard, the clock's master while it plays
///        in step; and audio's need for D.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/grid/stream/session.hpp"
#include "volumetric_kit/player/audio_feeder.hpp"
#include "volumetric_kit/player/audio_out.hpp"
#include "volumetric_kit/player/live_clock.hpp"
#include "volumetric_kit/player/playback.hpp"

namespace volumetric_kit::player {

/// What audio's need adds to a geometry period and the output's latency.
inline constexpr std::chrono::nanoseconds kAudioMargin{
    std::chrono::milliseconds{20}};
/// The range underruns move audio's need within (@ref audio_need).
inline constexpr std::chrono::nanoseconds kAudioNeedLeast{
    std::chrono::milliseconds{80}};
inline constexpr std::chrono::nanoseconds kAudioNeedMost{
    std::chrono::milliseconds{300}};
/// How far underruns raise audio's need, at most once a kAudioRaiseEvery of
/// audio played; it falls as far once kLiveWindow passes with none.
inline constexpr std::chrono::nanoseconds kAudioRaise{
    std::chrono::milliseconds{20}};
inline constexpr std::chrono::nanoseconds kAudioRaiseEvery{
    std::chrono::milliseconds{100}};
/// The drift trim's band and step (@ref drift_trim).
inline constexpr std::chrono::nanoseconds kTrimBand{
    std::chrono::milliseconds{20}};
inline constexpr std::chrono::nanoseconds kTrimStep{
    std::chrono::milliseconds{10}};
/// The least change of the clock's point a follow shifts in: smaller ones
/// add up until they reach it, as each shift is faded.
inline constexpr std::chrono::nanoseconds kFollowBand{
    std::chrono::milliseconds{10}};
/// The most what is heard may be off the clock and still be in step: the
/// clock's master. Past it, as after a stall, the audio is placed afresh.
inline constexpr std::chrono::nanoseconds kAudioReach{
    std::chrono::milliseconds{60}};
/// How long after a shift is made, beyond the output's latency, before what
/// is heard is trimmed again: until the shift is heard.
inline constexpr std::chrono::nanoseconds kShiftSettle{
    std::chrono::milliseconds{50}};

/// @brief Audio's need for D: what must pass from a record's earliest
///        arrival (`earliest`) before its audio plays, so none runs short.
///
/// @code
/// const auto need = audio_need(40ms, out.route().latency_ns, raised);
/// @endcode
/// @param period   One period of the geometry track: an Audio record rides
///                 behind the first frame at or past its pts, so it comes up
///                 to a period after that frame's earliest.
/// @param latency  The output's (AudioRoute::latency_ns).
/// @param raised   What underruns raised it by.
/// @return The base, @p period + @p latency + kAudioMargin, at least
///         kAudioNeedLeast, plus @p raised, at most kAudioNeedMost; never
///         below the base.
std::chrono::nanoseconds audio_need(std::chrono::nanoseconds period,
                                    std::chrono::nanoseconds latency,
                                    std::chrono::nanoseconds raised) noexcept;

/// @brief The drift trim: the shift that brings what is heard back toward
///        the clock once it strays past kTrimBand, as the sender's mic and
///        the viewer's output run at rates up to about 200 ppm apart.
///
/// @code
/// out.shift(signed_frames(drift_trim(heard - clock_pts)));
/// @endcode
/// @param ahead  How far what is heard is ahead of the clock (a later pts
///               heard now than the clock's): positive heard early.
/// @return kTrimStep, an insert, for @p ahead past kTrimBand; −kTrimStep, a
///         drop, below −kTrimBand; else 0.
std::chrono::nanoseconds drift_trim(std::chrono::nanoseconds ahead) noexcept;

/// @brief What a live feeder did, over the player's life.
struct LiveAudioStats {
  std::uint64_t records = 0;  ///< Written, whole or in part.
  /// Refused: more than a second of frames (@ref LiveAudio::takes), not
  /// whole frames, before the clock had a point, or wholly before what was
  /// already written (late).
  std::uint64_t dropped = 0;
  std::uint64_t placements = 0;  ///< Flushes placing the next record exactly.
  std::uint64_t follows = 0;     ///< Shifts that followed the clock's point.
  std::uint64_t trims = 0;       ///< Drift trims (@ref drift_trim).
  /// Underruns a look saw, each placing the audio afresh.
  std::uint64_t underruns = 0;
};

/// @brief A live stream's audio, fed on the worker: the first Audio track
///        on the geometry's clock (find_audio_track) plays through an output
///        of its own, on the live clock.
///
/// **Placing.** A connection's first record, and the first after the audio
/// fell out of step, flushes the output to the pts due to be heard as its
/// next frame is: `pts_at(point, at + latency)`, at its arrival and the
/// output's latency. The records after follow by pts, as a file's do
/// (@ref place): within a chunk of the running position they follow on, and
/// past it a gap is silence and an overlap is dropped. Records are written
/// as they arrive, so the output holds about D of audio, never more than
/// its ring: a record it refuses is placed afresh.
///
/// **Looks** (@ref look, on the worker between its steps). Each follows the
/// clock: a change of its point that reaches kFollowBand is shifted into the
/// output at once (an insert as it rises, a drop as it falls), smaller ones
/// adding up until they do, and a change past kAudioReach, as a join's
/// settling carries, places the audio afresh. Once each shift
/// has been heard, what is heard is trimmed toward the clock (@ref
/// drift_trim), and past kAudioReach, as when its device stopped, the audio
/// is placed afresh. The trims never touch D. An underrun, the audio having
/// come late (as through a stall), places it afresh too, and raises the
/// need a step (kAudioRaise, @ref audio_need), at most once a
/// kAudioRaiseEvery of audio played; kLiveWindow with none lowers it a step.
/// Each look says to the clock whether audio plays in step
/// (LiveClock::set_audio_playing).
///
/// **The master.** While it plays in step, @ref heard_pts is what is heard,
/// on the clock's pts: the output's position, now, plus @ref late. None
/// while the output runs dry or what is heard strays past kAudioReach, so a
/// stall never holds the picture.
///
/// **Late.** A need past kMaxLiveDelay, as a route of longer latency than
/// any wired one would need, is clamped: the audio then plays @ref late
/// behind the picture, and the status line says so.
///
/// **Threads.** The worker calls @ref begin, @ref end, @ref takes,
/// @ref arrived and @ref look. @ref heard_pts, @ref need, @ref late,
/// @ref playing, @ref position, @ref stats, @ref set_gain, @ref park and
/// @ref unpark are safe from any thread; the output is swapped, at a
/// @ref begin of another format, under a mutex they take.
///
/// @code
/// LiveAudio audio(clock, make_audio);
/// clock.set_audio_master(&audio);
/// VKC_TRY(audio.begin(find_audio_track(session, geometry), period));
/// // each record of its track, as it arrives, read if it takes it:
/// if (audio.takes(record.size)) {
///   audio.arrived(record.info, payload.data(), payload.size(), arrived);
/// }
/// // between the worker's steps:
/// audio.look(Clock::now());
/// clock.arrived(pts, at, program_need, audio.need());
/// @endcode
class LiveAudio final : public AudioMaster {
 public:
  /// @brief What it reads the time from: steady_clock's time.
  using Now = std::function<Playback::Clock::time_point()>;

  /// @brief A feeder with no track yet, which plays nothing.
  /// @param clock  The live clock it plays on; it must outlive this.
  /// @param make   Makes its output at a @ref begin; empty for the null
  ///               output, on steady_clock.
  /// @param now    Its time; empty for steady_clock's.
  explicit LiveAudio(LiveClock& clock, AudioOutMaker make = {}, Now now = {});
  /// @brief Tells the clock audio no longer plays.
  ~LiveAudio();
  LiveAudio(const LiveAudio&) = delete;
  LiveAudio& operator=(const LiveAudio&) = delete;
  LiveAudio(LiveAudio&&) = delete;
  LiveAudio& operator=(LiveAudio&&) = delete;

  /// @brief The worker, at a connection's session: play @p track, or with
  ///        none play nothing. An output of another format than the last is
  ///        made, playing, the last kept otherwise.
  /// @param track   The Audio track (find_audio_track), or none.
  /// @param period  The geometry track's period
  ///                (grid::stream::nominal_period_ns).
  /// @return OK; or the output's failure, which leaves no track.
  core::Status begin(const std::optional<AudioTrackChoice>& track,
                     std::chrono::nanoseconds period);
  /// @brief The worker, at a connection's end: no track, no need, and audio
  ///        no longer plays on the clock. What the output holds is dropped
  ///        (AudioOut::flush), so neither an @ref unpark nor the next
  ///        connection plays it stale.
  void end();

  /// @brief The worker, before reading a record of the track: whether it
  ///        takes one of @p bytes. One of more than a second of the track's
  ///        frames, more than the output's ring holds, is refused and
  ///        counted as dropped, so no buffer grows to it; the caller passes
  ///        it over unread.
  /// @param bytes  Its payload's bytes.
  /// @return Whether to read it for @ref arrived; false without a track.
  bool takes(std::size_t bytes);
  /// @brief The worker: a record of the track arrived whole at @p at:
  ///        placed and written into the output; refused as @ref takes
  ///        refuses it too.
  /// @param info   Its header.
  /// @param pcm    Its payload: s16 little-endian frames of the track's
  ///               channels, converted as to_played does.
  /// @param bytes  Its payload's bytes.
  /// @param at     When it arrived.
  void arrived(const grid::stream::RecordInfo& info, const std::uint8_t* pcm,
               std::size_t bytes, Playback::Clock::time_point at);
  /// @brief The worker, between its steps: follow the clock, trim, count
  ///        underruns, and say whether audio plays in step.
  /// @param now  The time.
  void look(Playback::Clock::time_point now);

  /// @return Audio's need for D (@ref audio_need) while a track plays; 0
  ///         without one.
  std::chrono::nanoseconds need() const noexcept {
    return std::chrono::nanoseconds(need_ns_.load(std::memory_order_relaxed));
  }
  /// @return How far the need passes kMaxLiveDelay: how late the audio
  ///         plays behind the picture; 0 within it.
  std::chrono::nanoseconds late() const noexcept {
    return std::chrono::nanoseconds(late_ns_.load(std::memory_order_relaxed));
  }
  /// @return Whether it plays in step with the clock, as the last look saw.
  bool playing() const noexcept {
    return playing_.load(std::memory_order_acquire);
  }
  /// @param now  The time, near now.
  /// @return What is heard at @p now, on the clock's pts (above); none while
  ///         not in step.
  std::optional<std::int64_t> heard_pts(
      Playback::Clock::time_point now) const noexcept override;
  /// @return The output's position (AudioOut::position); none without a
  ///         track or an output.
  std::optional<std::uint64_t> position() const;
  /// @return What it did.
  LiveAudioStats stats() const;

  /// @brief Its output's gain, now and for an output made later: 0 mutes.
  /// @param gain  At least 0 (AudioOut::set_gain).
  void set_gain(float gain);
  /// @brief Park its output (AudioOut::park), now and for one made later.
  void park();
  /// @brief End a @ref park.
  void unpark();

 private:
  // Out of step: the next record is placed afresh.
  void unplace();
  // The audio's point: the clock's plus late().
  std::optional<std::chrono::nanoseconds> audio_point() const;
  // `ns` as a signed count of frames at the track's rate, to the nearest.
  std::int64_t frames_of(std::chrono::nanoseconds ns) const;
  // The most bytes a record of the track may hold: a second of its frames.
  std::size_t most_bytes() const;
  // Counts a record refused.
  void refused();
  void set_playing(bool playing);

  LiveClock& clock_;
  const AudioOutMaker make_;
  const Now now_;

  // The output, swapped under mutex_ by the worker alone, so the worker
  // reads it without; the gain and the park, for an output made later; and
  // whether a track plays, for position().
  mutable std::mutex mutex_;
  std::unique_ptr<AudioOut> out_;
  float gain_ = 1.0f;
  bool parked_ = false;
  bool tracked_ = false;
  LiveAudioStats stats_;

  // The worker's.
  std::optional<AudioTrackChoice> track_;
  std::chrono::nanoseconds period_{0};
  std::vector<float> samples_;  // a record's frames, converted
  bool placed_ = false;         // flushed, and placing records since
  std::uint64_t flushes_ = 0;   // the output's, once that flush is applied
  std::uint64_t base_pts_ = 0;  // the flush's pts
  std::uint64_t written_ = 0;   // frames written since it
  bool exact_ = true;           // the next record is placed exactly
  std::chrono::nanoseconds placed_point_{0};  // the point followed so far
  Playback::Clock::time_point shifted_at_{};  // a shift made or under way
  std::uint64_t underruns_ = 0;               // the output's, as last seen
  bool counting_ = false;  // a look saw the placing flush applied
  std::chrono::nanoseconds raised_{0};
  Playback::Clock::time_point raised_at_{};  // the last raise, or fall
  // The output's frames played at the last raise, none before one; and
  // kAudioRaiseEvery in frames.
  std::optional<std::uint64_t> raise_played_;
  std::uint64_t raise_every_ = 0;

  // Any thread's.
  std::atomic<bool> playing_{false};
  std::atomic<std::int64_t> need_ns_{0};
  std::atomic<std::int64_t> late_ns_{0};
};

}  // namespace volumetric_kit::player
