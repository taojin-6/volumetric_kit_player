// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file live_clock.hpp
/// @brief A live stream's playout clock: each frame is presented (taken for
///        drawing) at present(pts) = pts + offset + D, D the largest need
///        stated for it; and the worker's rules on that clock, on given
///        times. Live alone: a file is paced by its pts (playback.hpp).

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

#include "volumetric_kit/player/playback.hpp"

namespace volumetric_kit::player {

/// The most D may be; a larger need is clamped to it (Playout::clamped).
inline constexpr std::chrono::nanoseconds kMaxLiveDelay{
    std::chrono::seconds{1}};
/// The window the offset is the least over, and the arrival need and the
/// lead the largest over.
inline constexpr std::chrono::nanoseconds kLiveWindow{std::chrono::seconds{10}};
/// The samples a window holds: past 102.4 records a second it spans less
/// than kLiveWindow.
inline constexpr std::size_t kLiveSamples = 1024;
/// How far the applied point falls a second elapsed (@ref step_point), and
/// how far the offset rises (LiveClock): 1 ms a record at 25 Hz. Each moves
/// the other way at once.
inline constexpr std::chrono::nanoseconds kPointFall{
    std::chrono::milliseconds{25}};
/// The least time between two meshes while records keep coming due
/// (@ref mesh_now), so a join's backlog shows as it drains.
inline constexpr std::chrono::nanoseconds kMeshEvery{
    std::chrono::milliseconds{100}};
/// While audio plays (@ref slew_point): the applied point rises at most
/// kSlewRise a step and per kSlewEvery elapsed; and once D's target has not
/// risen for kLiveWindow, falls at most kSlewFall a step and per second.
inline constexpr std::chrono::nanoseconds kSlewRise{
    std::chrono::milliseconds{20}};
inline constexpr std::chrono::nanoseconds kSlewEvery{
    std::chrono::milliseconds{100}};
inline constexpr std::chrono::nanoseconds kSlewFall{
    std::chrono::milliseconds{10}};

/// @brief A need for D, and so what set its target: the largest.
enum class DelayNeed : std::uint8_t {
  kNone,     ///< None: D is 0. There is no fixed floor.
  kArrival,  ///< The played track's records: the latest after its
             ///< earliest, over the window, plus the lead (LiveClock).
  kProgram,  ///< A program's own (FrameHooks::live_delay).
  kAudio,    ///< The stream's audio (live_audio.hpp: audio_need).
};

/// @brief The name of a need, as the stats and the log show it.
///
/// @code
/// std::printf("delay set by %s\n", to_string(playout.need));  // "program"
/// @endcode
/// @param need  The need.
/// @return "none", "arrival", "program" or "audio".
const char* to_string(DelayNeed need) noexcept;

/// @brief The clock at one moment (LiveClock::time), and as a frame was
///        taken (FrameStamp::playout).
struct Playout {
  /// The least `arrival − pts` (steady ns less pts ns) over the window of
  /// the played track's records, which it follows down at once and up at
  /// most kPointFall a second (LiveClock): a frame of pts p arrives no
  /// earlier than p + offset (@ref earliest).
  std::chrono::nanoseconds offset{0};
  /// The applied point: a frame is presented at pts + point (@ref present).
  /// It is the offset plus the applied D, moved by @ref step_point.
  std::chrono::nanoseconds point{0};
  /// D's target: the largest need, clamped to [0, kMaxLiveDelay].
  std::chrono::nanoseconds target{0};
  DelayNeed need = DelayNeed::kNone;  ///< The need that set the target.
  /// That need passed kMaxLiveDelay: frames that needed more are late by
  /// the excess.
  bool clamped = false;
  /// The worker's lead: a record's decode begins at its present time less
  /// this, so its frame is published by then.
  std::chrono::nanoseconds lead{0};

  /// @return The applied D: point − offset.
  std::chrono::nanoseconds delay() const noexcept { return point - offset; }
};

/// @brief When a frame arrives at the earliest, from which every need
///        counts.
/// @param playout  The clock.
/// @param pts_ns   The frame's pts.
/// @return pts + offset, on Playback::Clock.
Playback::Clock::time_point earliest(const Playout& playout,
                                     std::uint64_t pts_ns) noexcept;

/// @brief When a frame is presented: taken for drawing (FrameTaker). It
///        reaches the screen a present leg later (FrameFigures::present_ms).
/// @param point   Playout::point, or LiveClock::point on a realtime thread.
/// @param pts_ns  The frame's pts.
/// @return pts + point, on Playback::Clock.
Playback::Clock::time_point present(std::chrono::nanoseconds point,
                                    std::uint64_t pts_ns) noexcept;

/// @brief @ref present's inverse.
/// @param point  As @ref present takes it.
/// @param at     A time on Playback::Clock.
/// @return `at − point`, as ns in pts space; signed, as it may precede pts 0.
std::int64_t pts_at(std::chrono::nanoseconds point,
                    Playback::Clock::time_point at) noexcept;

/// @brief The release rule: how long until a held record's decode may
///        begin. With @p lead 0 it is the take's gate (FrameTaker).
/// @param pts_ns   The record's pts.
/// @param now_pts  Now in pts space (LiveClock::now_pts).
/// @param lead     The worker's lead (Playout::lead).
/// @return 0 once `now_pts >= pts_ns − lead`; else the time left.
std::chrono::nanoseconds release_wait(std::uint64_t pts_ns,
                                      std::int64_t now_pts,
                                      std::chrono::nanoseconds lead) noexcept;

/// @brief The mesh rule: whether the worker meshes its newest decode now.
/// @param free           The consumer took the last mesh, and no frame
///                       waits for its program (FrameHooks::data_ready).
/// @param since_publish  The time since this connection's last publish;
///                       none before its first.
/// @param next_pts_ns    The oldest record still held, if any.
/// @param now_pts        Now in pts space (LiveClock::now_pts).
/// @param lead           The worker's lead (Playout::lead).
/// @return Whether @p free, and either it is the connection's first frame,
///         the next held record is not due (@ref release_wait above 0, or
///         none held), or kMeshEvery has passed since the last publish.
bool mesh_now(bool free, std::optional<std::chrono::nanoseconds> since_publish,
              std::optional<std::uint64_t> next_pts_ns, std::int64_t now_pts,
              std::chrono::nanoseconds lead) noexcept;

/// @brief The applied point's step toward its target while no audio plays.
/// @param applied  The applied point, carried by the offset's fall while
///                 settling (LiveClock).
/// @param target   The offset plus D's target.
/// @param elapsed  The time since the last step.
/// @return @p target when at or above @p applied; else @p applied less
///         kPointFall a second of @p elapsed, no lower than @p target.
std::chrono::nanoseconds step_point(std::chrono::nanoseconds applied,
                                    std::chrono::nanoseconds target,
                                    std::chrono::nanoseconds elapsed) noexcept;

/// @brief The applied point's slew toward its target while audio plays, in
///        place of @ref step_point: each change of the point is a gap or a
///        skip in what is heard (live_audio.hpp), so it rises in steps the
///        ear barely notices and falls more slowly still.
///
/// @code
/// next = slew_point(applied, offset + target, at - stepped_at,
///                   at - target_rose_at);
/// @endcode
/// @param applied     The applied point.
/// @param target      The offset plus D's target.
/// @param elapsed     The time since the last step.
/// @param stable_for  The time since D's target last rose.
/// @return Toward @p target and never past it: up by at most kSlewRise, and
///         kSlewRise a kSlewEvery of @p elapsed; down only once
///         @p stable_for reaches kLiveWindow, by at most kSlewFall, and
///         kSlewFall a second of @p elapsed.
std::chrono::nanoseconds slew_point(
    std::chrono::nanoseconds applied, std::chrono::nanoseconds target,
    std::chrono::nanoseconds elapsed,
    std::chrono::nanoseconds stable_for) noexcept;

/// @brief What is heard, for a live clock whose audio plays
///        (LiveClock::set_audio_master): the master its now_pts takes.
///
/// @code
/// class Heard final : public AudioMaster {
///   std::optional<std::int64_t> heard_pts(
///       Playback::Clock::time_point) const noexcept override {
///     return pts;  // what the output plays now
///   }
///   std::int64_t pts = 0;
/// };
/// @endcode
class AudioMaster {
 public:
  /// @brief The pts being heard at @p now, on the clock's pts, while it
  ///        plays in step with the clock; any thread.
  /// @param now  The time, near now.
  /// @return The pts; none while nothing is heard in step (the picture is
  ///         then the master).
  virtual std::optional<std::int64_t> heard_pts(
      Playback::Clock::time_point now) const noexcept = 0;

 protected:
  AudioMaster() = default;
  ~AudioMaster() = default;
  AudioMaster(const AudioMaster&) = default;
  AudioMaster& operator=(const AudioMaster&) = default;
};

/// @brief A live player's playout clock (StreamPlayer::live_clock).
///
/// **Offset.** The least `arrival − pts` over kLiveWindow of the played
/// track's records, never another track's. It follows the least down at
/// once, and up at most kPointFall a second, never more than kMaxLiveDelay
/// below it. So a fast record leaving the window moves neither @ref earliest
/// nor a program's target point (offset + its need) at once: a program's
/// need counts from the offset.
///
/// **Needs.** The arrival need is the largest lateness over the window, plus
/// the lead. A record's lateness is its `arrival − pts` less the larger of
/// two offsets: the offset as it arrived, and the offset now. So a fast
/// record entering or leaving the window leaves the target point
/// (offset + target) where it was. A join's burst adds nothing, as each
/// record of it is a new least. The program's need is its answer at each
/// record (FrameHooks::live_delay), and audio's its feeder's (LiveAudio). The
/// target is the largest need, clamped to [0, kMaxLiveDelay].
///
/// **Point.** At each write, the applied point steps toward the offset plus
/// the target (@ref step_point), with the time since the last step; while
/// audio plays (@ref set_audio_playing), it slews (@ref slew_point). Within
/// kLiveWindow of the connection's first record, while the least is still
/// being found, the offset's fall carries the point with it at once, so a
/// join's backlog lands at the live edge; a fall of D is stepped as ever.
///
/// **Master.** Now in pts space (@ref now_pts) is the picture's,
/// `pts_at(point, now)`, or while audio plays what is heard
/// (@ref set_audio_master), whenever it is heard in step.
///
/// **Lead.** The largest time over the window that the worker took from a
/// record's release to its frame's publish. The waits for the consumer's take
/// and for the program (FrameHooks::data_ready) are left out, and so is a
/// connection's first frame, which builds the grid.
///
/// **Times are passed in**, so a test never sleeps. **No allocation per
/// record:** the samples sit in fixed rings of kLiveSamples.
///
/// **Threads.** The worker alone writes (@ref arrived, @ref published,
/// @ref reset, @ref set_audio_playing). @ref time copies the clock under a
/// mutex, for the worker, the take, a program and the stats. @ref point
/// reads one atomic: it is lock-free and never blocks, so it serves a
/// realtime thread.
///
/// @code
/// clock.arrived(record.info.pts_ns, arrived, program_need);
/// const std::optional<Playout> playout = clock.time();
/// const std::int64_t now_pts = clock.now_pts(*playout, Clock::now());
/// if (release_wait(front_pts, now_pts, playout->lead).count() == 0) {
///   // ... decode the front record ...
/// }
/// @endcode
class LiveClock {
 public:
  using Clock = Playback::Clock;

  /// @return The clock now, under its mutex; none before the first record
  ///         of the played track since construction or @ref reset.
  std::optional<Playout> time() const;

  /// @return Playout::point alone, from one atomic, for a realtime thread;
  ///         none as @ref time.
  std::optional<std::chrono::nanoseconds> point() const noexcept;

  /// @brief Now in pts space, from the one snapshot a decision reads, for
  ///        the release rule, the mesh rule and the take's gate: the one
  ///        place the master is chosen.
  /// @param playout  The snapshot (@ref time).
  /// @param now      The time.
  /// @return While audio plays and is heard in step, the pts heard
  ///         (AudioMaster::heard_pts); else the picture's,
  ///         `pts_at(playout.point, now)`.
  std::int64_t now_pts(const Playout& playout,
                       Clock::time_point now) const noexcept;

  /// @brief What is heard while audio plays: @ref now_pts's master then.
  ///        Set once, before the worker starts.
  /// @param audio  It, which must outlive this clock's readers; null for
  ///               none.
  void set_audio_master(const AudioMaster* audio) noexcept {
    audio_.store(audio, std::memory_order_release);
  }
  /// @brief The worker: whether audio plays in step with the clock. While
  ///        it does, the point slews (@ref slew_point) and @ref now_pts
  ///        takes what is heard. False at first and after @ref reset.
  /// @param playing  Whether it plays.
  void set_audio_playing(bool playing) noexcept {
    audio_playing_.store(playing, std::memory_order_release);
  }
  /// @return What @ref set_audio_playing last set.
  bool audio_playing() const noexcept {
    return audio_playing_.load(std::memory_order_acquire);
  }

  /// @brief The worker: a record of the played track arrived whole at
  ///        @p at. The offset and the arrival need take it, samples past
  ///        the window expire, and the point steps.
  /// @param pts_ns        Its pts.
  /// @param at            When; no earlier than the last write's.
  /// @param program_need  FrameHooks::live_delay's answer; 0 for none.
  /// @param audio_need    The stream's audio's (LiveAudio::need); 0 for none.
  void arrived(
      std::uint64_t pts_ns, Clock::time_point at,
      std::chrono::nanoseconds program_need,
      std::chrono::nanoseconds audio_need = std::chrono::nanoseconds(0));

  /// @brief The worker: a frame was published at @p at, @p took after its
  ///        record's release, waits left out. The lead takes it, and the
  ///        point steps.
  /// @param took  The worker's time on it.
  /// @param at    When; no earlier than the last write's.
  void published(std::chrono::nanoseconds took, Clock::time_point at);

  /// @brief The worker, at a new connection: forget everything. Then
  ///        @ref time and @ref point are none until the next record.
  void reset();

 private:
  // One record: when it arrived, its arrival − pts, and the offset as it
  // arrived.
  struct Arrival {
    Clock::time_point at;
    std::chrono::nanoseconds ahead;
    std::chrono::nanoseconds least;
  };
  // One publish: when it happened, and how long it took.
  struct Lead {
    Clock::time_point at;
    std::chrono::nanoseconds took;
  };
  // Under mutex_: expire, move the offset, recompute the needs, step the
  // point, and store it; for an `arrival`, set the newest's offset.
  void update(Clock::time_point at, bool arrival);

  static constexpr std::int64_t kNoPoint =
      std::numeric_limits<std::int64_t>::min();
  static_assert(std::atomic<std::int64_t>::is_always_lock_free);

  mutable std::mutex mutex_;                      // what follows, but point_ns_
  std::array<Arrival, kLiveSamples> arrivals_{};  // a ring, oldest first
  std::size_t arrivals_head_ = 0, arrivals_size_ = 0;
  std::array<Lead, kLiveSamples> leads_{};  // a ring, oldest first
  std::size_t leads_head_ = 0, leads_size_ = 0;
  std::chrono::nanoseconds program_need_{0};
  std::chrono::nanoseconds audio_need_{0};
  std::optional<Playout> state_;
  Clock::time_point first_at_{};    // the connection's first record
  Clock::time_point stepped_at_{};  // the last step
  Clock::time_point rose_at_{};     // when D's target last rose
  std::atomic<std::int64_t> point_ns_{kNoPoint};  // state_->point, for point()
  std::atomic<const AudioMaster*> audio_{nullptr};
  std::atomic<bool> audio_playing_{false};
};

}  // namespace volumetric_kit::player
