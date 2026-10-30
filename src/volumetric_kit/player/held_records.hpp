// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file held_records.hpp
/// @brief A live player's records held as host bytes until their release
///        (StreamPlayer::connect), in one buffer it reuses. Player-internal.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "volumetric_kit/grid/stream/stream_reader.hpp"
#include "volumetric_kit/player/playback.hpp"

namespace volumetric_kit::player {

/// The most records a live player holds before their release; past it, as
/// past their bytes' buffer, it reads nothing more.
inline constexpr std::size_t kHeldRecords = 1024;

/// @brief Records held as host bytes, oldest first: their bytes in one
///        buffer, each record's contiguous, their headers in a fixed ring of
///        kHeldRecords.
///
/// The bytes follow one another, and wrap to the buffer's start once the
/// next does not fit before its end. Whether they have wrapped is kept, not
/// read from the offsets, so a record of no bytes is placed like any other.
/// The buffer is made at the first use and kept, uncleared: its pages are
/// touched as records are written, so making it costs no write of its whole
/// size on the first record's way. A record larger than the buffer waits
/// until nothing is held, which grows the buffer to it. Nothing is allocated
/// per record.
///
/// @code
/// HeldRecords held(kLiveHoldBytes);
/// if (std::uint8_t* const at = held.room(record.size)) {
///   VKC_TRY(client.read(record, at, record.size));
///   held.push(record, arrived, at);
/// }  // else full: read it once the front is released
/// ...
/// decode(held.bytes(held.front()), held.front().record.size);
/// held.pop();
/// @endcode
class HeldRecords {
 public:
  using Clock = Playback::Clock;

  /// @brief One record held.
  struct Held {
    grid::stream::Record record;  ///< Its header.
    Clock::time_point arrived;    ///< When it arrived whole.
    std::size_t at = 0;           ///< Its bytes' start in the buffer.
  };

  /// @param capacity  The buffer's bytes, made at the first @ref room.
  explicit HeldRecords(std::size_t capacity) noexcept : capacity_(capacity) {}

  /// @brief Where @p size bytes more go: after the newest record's bytes,
  ///        or at the buffer's start once they do not fit before its end.
  /// @param size  The record's bytes.
  /// @return The place; null while full: kHeldRecords held, or no room
  ///         before the oldest's bytes. With nothing held, the buffer's
  ///         start, the buffer made, or grown to @p size when larger.
  std::uint8_t* room(std::size_t size);

  /// @brief Hold @p record, whose bytes were just written at @p at.
  /// @pre @p at is what @ref room returned for its size, with no push, pop
  ///      or clear since.
  /// @param record   Its header.
  /// @param arrived  When it arrived whole.
  /// @param at       Where its bytes are.
  void push(const grid::stream::Record& record, Clock::time_point arrived,
            const std::uint8_t* at);

  /// @return Whether none is held.
  bool empty() const noexcept { return size_ == 0; }

  /// @return The records held.
  std::size_t size() const noexcept { return size_; }

  /// @return The oldest record held.
  /// @pre Not @ref empty.
  const Held& front() const noexcept { return held_[head_]; }

  /// @param held  A record held.
  /// @return Its bytes, `held.record.size` of them.
  const std::uint8_t* bytes(const Held& held) const noexcept {
    return bytes_.get() + held.at;
  }

  /// @brief Let the oldest record go.
  /// @pre Not @ref empty.
  void pop() noexcept;

  /// @brief Let every record go, keeping the buffer.
  void clear() noexcept { head_ = size_ = tail_ = upper_ = 0; }

 private:
  std::unique_ptr<std::uint8_t[]> bytes_;  // capacity_ bytes, uncleared
  std::size_t capacity_;
  std::array<Held, kHeldRecords> held_{};  // a ring, oldest first
  std::size_t head_ = 0, size_ = 0;
  std::size_t tail_ = 0;   // the end of the newest record's bytes
  std::size_t upper_ = 0;  // wrapped: the oldest records above tail_; else 0
};

}  // namespace volumetric_kit::player
