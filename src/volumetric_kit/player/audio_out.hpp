// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file audio_out.hpp
/// @brief Where a stream's audio goes to be heard: an output fed through a
///        ring by one producer (the feeder) and drained by its render thread,
///        with play, pause, flush, gain and the pts being heard; and the null
///        output, silent, which drains it on a clock.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/player/audio_ring.hpp"

namespace volumetric_kit::player {

/// @brief What an output plays.
struct AudioFormat {
  std::uint32_t sample_rate = 48000;  ///< Frames a second.
  std::uint16_t channels = 1;         ///< Samples a frame, interleaved: 1 or 2.
};

/// @return The time @p frames frames last at @p sample_rate, in ns, rounded
///         down.
/// @pre `sample_rate > 0`.
std::uint64_t audio_ns(std::uint64_t frames, std::uint32_t sample_rate);

/// @return The frames @p ns nanoseconds hold at @p sample_rate, rounded to
///         the nearest.
/// @pre `sample_rate > 0`.
std::uint64_t audio_frames(std::uint64_t ns, std::uint32_t sample_rate);

/// @brief What an output counted.
struct AudioOutStats {
  std::uint64_t played_frames = 0;  ///< Taken from the ring and rendered.
  /// Rendered as silence for want of frames while playing, once flushed.
  std::uint64_t underrun_frames = 0;
  std::uint64_t flushes = 0;  ///< Flushes the render thread has applied.
};

/// @brief An audio output: a ring (AudioRing) that one producer fills with
///        f32 frames placed in time, and a render thread that drains it at
///        the output's rate.
///
/// **Time.** @ref flush says where the next frame written sits: its pts,
/// on the stream's clock. Frames written after it follow one another, so
/// the k-th is at that pts plus k frames' time; the producer writes silence
/// or drops frames to keep it so (AudioFeeder). @ref position is the pts of
/// the frame being heard now.
///
/// **Controls.** It opens paused. @ref play and @ref pause start and stop
/// the frames going out: paused, the render thread plays silence and takes
/// nothing, so @ref position holds. @ref set_gain scales what goes out; at 0
/// the frames still go, so @ref position runs on (mute).
///
/// **Threads.** One producer calls @ref write and @ref flush. The controls,
/// @ref position, @ref queued and @ref stats are safe from any thread. The
/// render thread, an output's own, calls render(), which neither locks nor
/// allocates.
///
/// @code
/// VKC_ASSIGN(std::unique_ptr<AudioOut> out, AudioOut::open_default(format));
/// out->flush(first_pts_ns);       // the producer's
/// out->write(frames, count);      // the producer's
/// out->play();                    // any thread
/// std::optional<std::uint64_t> heard = out->position();  // any thread
/// @endcode
class AudioOut {
 public:
  /// @brief The output this platform plays through.
  /// @param format  What it plays.
  /// @return For now the null output, on `std::chrono::steady_clock` and a
  ///         thread of its own, everywhere; NullAudioOut::create's refusals.
  // TODO: the Apple output (AVAudioEngine) on macOS and iOS.
  static core::Result<std::unique_ptr<AudioOut>> open_default(
      const AudioFormat& format);

  virtual ~AudioOut() = default;
  AudioOut(const AudioOut&) = delete;
  AudioOut& operator=(const AudioOut&) = delete;
  AudioOut(AudioOut&&) = delete;
  AudioOut& operator=(AudioOut&&) = delete;

  /// @return What it plays.
  const AudioFormat& format() const noexcept { return format_; }
  /// @return Its ring's capacity, in frames: about a second.
  std::size_t capacity() const noexcept { return ring_.capacity(); }

  /// @brief The producer's: append up to @p frames interleaved frames.
  /// @param samples  @p frames frames of @ref format's channels; null for
  ///                 silence.
  /// @param frames   How many.
  /// @return The frames taken: fewer once the ring is full, the rest the
  ///         producer's to write again later.
  std::size_t write(const float* samples, std::size_t frames) noexcept {
    return ring_.write(samples, frames);
  }
  /// @brief The producer's: drop every frame written so far, and say that
  ///        the next one written is at @p pts_ns. The render thread applies
  ///        it at its next render, playing or paused; @ref position is then
  ///        @p pts_ns until a frame goes out.
  /// @param pts_ns  On the stream's clock.
  void flush(std::uint64_t pts_ns) noexcept;
  /// @return The frames written since the last flush that have not gone
  ///         out yet; safe from any thread, so as of a moment.
  std::uint64_t queued() const noexcept;

  /// @brief Frames go out: from the next render on.
  void play() noexcept { playing_.store(true, std::memory_order_relaxed); }
  /// @brief Silence goes out and nothing is taken: @ref position holds.
  void pause() noexcept { playing_.store(false, std::memory_order_relaxed); }
  /// @return Whether frames go out: false until @ref play.
  bool playing() const noexcept {
    return playing_.load(std::memory_order_relaxed);
  }
  /// @brief Scale what goes out by @p gain, from the next render; 1 at
  ///        first, 0 to mute.
  /// @param gain  At least 0.
  void set_gain(float gain) noexcept {
    gain_.store(gain, std::memory_order_relaxed);
  }
  /// @return What @ref set_gain last set.
  float gain() const noexcept { return gain_.load(std::memory_order_relaxed); }

  /// @return The pts of the frame being heard now, on the stream's clock:
  ///         as of the render thread's last render; none until it has
  ///         applied a @ref flush.
  std::optional<std::uint64_t> position() const noexcept;
  /// @return What it counted so far.
  AudioOutStats stats() const noexcept;

 protected:
  /// @brief An output of @p format, paused, its ring a second long.
  /// @throws std::bad_alloc when its ring cannot be allocated.
  explicit AudioOut(const AudioFormat& format);

  /// @brief The render thread's: the next @p frames frames to go out into
  ///        @p out, silence where the ring runs short or while paused,
  ///        scaled by the gain; a @ref flush the producer made first is
  ///        applied first. It neither locks nor allocates.
  /// @param out     Room for @p frames interleaved frames.
  /// @param frames  How many.
  void render(float* out, std::size_t frames) noexcept;

 private:
  // The render thread's: apply the producer's last flush, if it has not
  // been, and is not being written.
  void apply_flush() noexcept;

  static constexpr std::uint64_t kNoPosition = ~std::uint64_t{0};

  AudioFormat format_;
  AudioRing ring_;
  // The producer's last flush, under a sequence lock: mark_seq_ is odd while
  // the producer writes mark_index_ and mark_pts_.
  std::atomic<std::uint64_t> mark_seq_{0};
  std::atomic<std::uint64_t> mark_index_{0};  // the ring's written() then
  std::atomic<std::uint64_t> mark_pts_{0};
  // The render thread's: the flush applied last, and the ring index and pts
  // its frames count from.
  std::uint64_t seen_seq_ = 0;
  bool based_ = false;
  std::uint64_t base_index_ = 0;
  std::uint64_t base_pts_ = 0;
  // Any thread's.
  std::atomic<bool> playing_{false};
  std::atomic<float> gain_{1.0f};
  std::atomic<std::uint64_t> position_{kNoPosition};
  std::atomic<std::uint64_t> played_{0};
  std::atomic<std::uint64_t> underrun_{0};
  std::atomic<std::uint64_t> flushes_{0};
};

/// @brief Makes the output a player plays a stream's audio through, of the
///        given format (StreamPlayer::open).
using AudioOutMaker =
    std::function<core::Result<std::unique_ptr<AudioOut>>(const AudioFormat&)>;

/// How often the null output's own thread renders.
inline constexpr std::chrono::milliseconds kNullPeriod{10};

/// @brief The silent output: it renders what its clock says has played
///        since its last @ref tick and drops it, as a device would play it,
///        so @ref position, pause and the ring behave as on a device. Linux,
///        CI and every platform without an output of its own play through
///        it.
///
/// Each tick renders the time its clock has moved since the one before,
/// rounded to whole frames with the remainder carried, and at most a second
/// of it: a longer stall is cut to that.
///
/// @code
/// // A test's: its own clock, ticked by the test, the render thread.
/// VKC_ASSIGN(std::unique_ptr<NullAudioOut> out,
///            NullAudioOut::create(format, [&] { return now; },
///                                 NullAudioOut::Ticks::kByCaller));
/// out->flush(0);
/// out->write(frames, count);
/// out->play();
/// now += 25ms;
/// out->tick();  // position() is now 25 ms on
/// @endcode
class NullAudioOut final : public AudioOut {
 public:
  /// @brief The clock it plays by.
  using Clock = std::function<std::chrono::steady_clock::time_point()>;
  /// @brief Who ticks it.
  enum class Ticks {
    kOwnThread,  ///< A thread of its own, every @ref kNullPeriod.
    kByCaller,   ///< The caller, by @ref tick: then its render thread.
  };

  /// @brief A null output of @p format, paused, its clock's time at the
  ///        call its start.
  /// @param format  What it plays.
  /// @param clock   Its clock; empty for `std::chrono::steady_clock`.
  /// @param ticks   Who ticks it.
  /// @return The output; `core::Status::Code::InvalidArgument` for a
  ///         sample rate of 0 or other than 1 or 2 channels; `OutOfMemory`
  ///         when its ring cannot be allocated; or `IoError` when its thread
  ///         cannot start.
  static core::Result<std::unique_ptr<NullAudioOut>> create(
      const AudioFormat& format, Clock clock = {},
      Ticks ticks = Ticks::kOwnThread);

  /// @brief Stops its thread, if it has one.
  ~NullAudioOut() override;

  /// @brief Render, and drop, the frames its clock says went out since the
  ///        last tick. With Ticks::kByCaller only, from one thread at a
  ///        time.
  void tick() noexcept;

 private:
  NullAudioOut(const AudioFormat& format, Clock clock);

  Clock clock_;
  // The render thread's.
  std::chrono::steady_clock::time_point last_;
  std::uint64_t carry_ = 0;     // ns * rate not yet a whole frame
  std::vector<float> scratch_;  // what each render writes, dropped
  // Its own thread's quit.
  std::mutex mutex_;
  std::condition_variable quit_changed_;
  bool quit_ = false;
  std::thread thread_;
};

}  // namespace volumetric_kit::player
