// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file audio_ring.hpp
/// @brief A single-producer, single-consumer ring of interleaved f32 audio
///        frames: the feeder writes it, an output's render thread reads it.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace volumetric_kit::player {

/// @brief A ring of interleaved f32 frames between one producer thread and
///        one consumer thread, lock-free: neither side locks, and only the
///        constructor allocates.
///
/// Each side keeps a count of the frames it has passed, never wrapping: the
/// producer's @ref written, the consumer's @ref read_index. The ring holds
/// the frames between them. A frame's index is its place in that count, so a
/// caller can say where a frame sits in time from its index (AudioOut does).
///
/// @code
/// AudioRing ring(48000, 2);  // a second of stereo at 48 kHz
/// // The producer's thread:
/// const std::size_t taken = ring.write(samples, frames);  // the rest waits
/// // The consumer's thread:
/// const std::size_t got = ring.read(out, frames);  // silence after `got`
/// @endcode
///
/// @warning One thread writes (@ref write) and one reads (@ref read,
///          @ref skip_to); @ref written and @ref read_index are safe from any
///          thread.
class AudioRing {
 public:
  /// @brief An empty ring.
  /// @param frames    Its capacity in frames, rounded up to a power of two;
  ///                  at least 1.
  /// @param channels  Samples a frame; at least 1.
  /// @throws std::bad_alloc when its samples cannot be allocated.
  AudioRing(std::size_t frames, std::uint16_t channels);

  AudioRing(const AudioRing&) = delete;
  AudioRing& operator=(const AudioRing&) = delete;
  AudioRing(AudioRing&&) = delete;
  AudioRing& operator=(AudioRing&&) = delete;

  /// @return Its capacity in frames: a power of two.
  std::size_t capacity() const noexcept { return mask_ + 1; }
  /// @return Samples a frame.
  std::uint16_t channels() const noexcept { return channels_; }

  /// @brief The producer's: append up to @p frames frames.
  /// @param samples  @p frames interleaved frames; null for silence.
  /// @param frames   How many.
  /// @return The frames taken, oldest first: fewer than @p frames once the
  ///         ring is full (an overrun), the rest the producer's to write
  ///         again later.
  std::size_t write(const float* samples, std::size_t frames) noexcept;

  /// @brief The consumer's: take up to @p frames frames, oldest first.
  /// @param out     Room for @p frames interleaved frames.
  /// @param frames  How many.
  /// @return The frames taken into @p out: fewer than @p frames once the
  ///         ring is empty (an underrun); what follows them in @p out is
  ///         left as it was.
  std::size_t read(float* out, std::size_t frames) noexcept;

  /// @brief The consumer's: drop every frame before index @p index unread.
  /// @param index  At most @ref written; one at or before @ref read_index
  ///               drops nothing.
  void skip_to(std::uint64_t index) noexcept;

  /// @return The frames ever written: the index of the next frame written.
  std::uint64_t written() const noexcept {
    return write_.load(std::memory_order_acquire);
  }
  /// @return The frames ever read or skipped: the index of the next frame
  ///         read.
  std::uint64_t read_index() const noexcept {
    return read_.load(std::memory_order_acquire);
  }

 private:
  // `run(ring, offset, samples)` over the `frames` frames from index `at`,
  // in at most two runs of the ring: each run's first sample in the ring,
  // its first sample's offset among the frames', and its samples.
  template <typename Run>
  void runs(std::uint64_t at, std::size_t frames, Run&& run) noexcept;

  std::vector<float> samples_;
  std::size_t mask_;  // capacity - 1
  std::uint16_t channels_;
  std::atomic<std::uint64_t> write_{0};  // the producer's, published
  std::atomic<std::uint64_t> read_{0};   // the consumer's, published
};

}  // namespace volumetric_kit::player
