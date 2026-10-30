// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// A live player's records held as bytes (held_records.hpp), host only: with
// nothing held a record goes at the buffer's start; records follow one
// another, wrap to the start once the next does not fit before the end, and
// a wrapped one waits for the oldest to go; a record of no bytes, wrapped
// right up to the oldest, takes none of its bytes; kHeldRecords records fill
// it whatever their bytes; a record larger than the buffer waits until none
// is held, then grows it and is held alone; and many records pushed and
// popped in turn, of sizes 0 to 37 in a buffer of 100, never overlap one
// another, each read back as written.

#include "volumetric_kit/player/held_records.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>

namespace {

namespace vkp = volumetric_kit::player;
using vkp::HeldRecords;

// A record of `size` bytes, its seq `seq`.
volumetric_kit::grid::stream::Record record_of(std::uint64_t seq,
                                               std::size_t size) {
  volumetric_kit::grid::stream::Record record;
  record.info.seq = seq;
  record.size = size;
  return record;
}

// Hold record `seq` of `size` bytes, each byte `seq`'s low byte: its
// offset in the buffer, or none while full.
std::optional<std::size_t> hold(HeldRecords& held, std::uint8_t* start,
                                std::uint64_t seq, std::size_t size) {
  std::uint8_t* const at = held.room(size);
  if (at == nullptr) {
    return std::nullopt;
  }
  std::memset(at, static_cast<int>(seq & 0xff), size);
  held.push(record_of(seq, size), {}, at);
  return start == nullptr ? 0 : static_cast<std::size_t>(at - start);
}

// Whether the oldest held is record `seq`, its bytes as written.
bool front_is(const HeldRecords& held, std::uint64_t seq) {
  if (held.empty() || held.front().record.info.seq != seq) {
    return false;
  }
  const std::uint8_t* const bytes = held.bytes(held.front());
  for (std::size_t i = 0; i < held.front().record.size; ++i) {
    if (bytes[i] != static_cast<std::uint8_t>(seq & 0xff)) {
      return false;
    }
  }
  return true;
}

// The buffer's start, once made.
std::uint8_t* start_of(HeldRecords& held) {
  std::uint8_t* const at = held.room(0);
  EXPECT_NE(at, nullptr);
  return held.empty() ? at : nullptr;
}

TEST(HeldRecords, RecordsFollowAndWrapToTheStartOnceTheOldestIsGone) {
  HeldRecords held(100);
  EXPECT_TRUE(held.empty());
  std::uint8_t* const start = start_of(held);
  ASSERT_NE(start, nullptr);
  EXPECT_EQ(hold(held, start, 1, 40), 0u);
  EXPECT_EQ(hold(held, start, 2, 40), 40u);
  EXPECT_EQ(held.size(), 2u);
  // 20 bytes left before the end; none before the oldest.
  EXPECT_FALSE(hold(held, start, 3, 30).has_value());
  ASSERT_TRUE(front_is(held, 1));
  held.pop();
  // The oldest at 40: 30 bytes wrap to the start, then 10 fill up to it.
  EXPECT_EQ(hold(held, start, 3, 30), 0u);
  EXPECT_EQ(hold(held, start, 4, 10), 30u);
  EXPECT_FALSE(hold(held, start, 5, 1).has_value()) << "past the oldest";
  ASSERT_TRUE(front_is(held, 2));
  held.pop();
  // The oldest wrapped: one span from the start, [0, 40), 60 bytes after.
  EXPECT_EQ(hold(held, start, 5, 60), 40u);
  for (const std::uint64_t seq : {3, 4, 5}) {
    ASSERT_TRUE(front_is(held, seq)) << "record " << seq;
    held.pop();
  }
  EXPECT_TRUE(held.empty());
  EXPECT_EQ(hold(held, start, 6, 100), 0u) << "empty: from the start";
}

// The ring wrapped and filled exactly up to the oldest's bytes: a record of
// no bytes goes there, and the next record still waits for the oldest.
TEST(HeldRecords, ARecordOfNoBytesUpToTheOldestTakesNoneOfItsBytes) {
  HeldRecords held(100);
  std::uint8_t* const start = start_of(held);
  ASSERT_NE(start, nullptr);
  ASSERT_EQ(hold(held, start, 1, 60), 0u);
  ASSERT_EQ(hold(held, start, 2, 30), 60u);
  held.pop();
  ASSERT_EQ(hold(held, start, 3, 60), 0u);  // wrapped, up to the oldest
  EXPECT_EQ(hold(held, start, 4, 0), 60u);
  EXPECT_FALSE(hold(held, start, 5, 1).has_value()) << "over the oldest";
  ASSERT_TRUE(front_is(held, 2));
  held.pop();
  ASSERT_TRUE(front_is(held, 3));
  held.pop();
  ASSERT_TRUE(front_is(held, 4));
  held.pop();
  EXPECT_TRUE(held.empty());
}

TEST(HeldRecords, KHeldRecordsFillItWhateverTheirBytes) {
  HeldRecords held(1 << 20);
  for (std::uint64_t seq = 0; seq < vkp::kHeldRecords; ++seq) {
    ASSERT_TRUE(hold(held, nullptr, seq, seq % 3).has_value()) << seq;
  }
  EXPECT_EQ(held.size(), vkp::kHeldRecords);
  EXPECT_FALSE(hold(held, nullptr, 9999, 0).has_value());
  EXPECT_EQ(held.room(1), nullptr);
  held.pop();
  EXPECT_TRUE(hold(held, nullptr, 9999, 1).has_value());
  held.clear();
  EXPECT_TRUE(held.empty());
}

TEST(HeldRecords, ARecordLargerThanTheBufferIsHeldAlone) {
  HeldRecords held(100);
  std::uint8_t* const start = start_of(held);
  ASSERT_NE(start, nullptr);
  ASSERT_EQ(hold(held, start, 1, 10), 0u);
  EXPECT_EQ(held.room(150), nullptr) << "something held";
  held.pop();
  // Nothing held: the buffer grows to it.
  std::uint8_t* const grown = held.room(150);
  ASSERT_NE(grown, nullptr);
  std::memset(grown, 2, 150);
  held.push(record_of(2, 150), {}, grown);
  EXPECT_EQ(held.room(1), nullptr) << "held alone";
  ASSERT_TRUE(front_is(held, 2));
  held.pop();
  // The buffer stays grown.
  std::uint8_t* const again = held.room(150);
  ASSERT_NE(again, nullptr);
  std::memset(again, 3, 150);
  held.push(record_of(3, 150), {}, again);
  EXPECT_TRUE(front_is(held, 3));
}

// A model of the ring: records pushed while there is room and popped
// otherwise, sizes 0 to 37 in 100 bytes. Each place room gives overlaps no
// record held, and every record reads back as written.
TEST(HeldRecords, PushedAndPoppedInTurnNoneOverlapsAnother) {
  constexpr std::size_t kCapacity = 100;
  HeldRecords held(kCapacity);
  std::uint8_t* const start = start_of(held);
  ASSERT_NE(start, nullptr);
  struct Span {
    std::uint64_t seq;
    std::size_t at, size;
  };
  std::deque<Span> model;
  std::uint32_t random = 12345;
  std::uint64_t seq = 0, wraps = 0;
  for (int step = 0; step < 20'000; ++step) {
    random = random * 1'103'515'245u + 12'345u;
    const std::size_t size = (random >> 16) % 38;
    std::uint8_t* const at = held.room(size);
    if (at != nullptr) {
      const std::size_t offset = static_cast<std::size_t>(at - start);
      ASSERT_LE(offset + size, kCapacity) << "step " << step;
      for (const Span& span : model) {
        const bool apart = offset + size <= span.at ||
                           span.at + span.size <= offset || size == 0 ||
                           span.size == 0;
        ASSERT_TRUE(apart) << "step " << step << ": [" << offset << ", "
                           << offset + size << ") over record " << span.seq;
      }
      wraps += !model.empty() && offset < model.back().at ? 1 : 0;
      std::memset(at, static_cast<int>(seq & 0xff), size);
      held.push(record_of(seq, size), {}, at);
      model.push_back({seq++, offset, size});
    } else {
      ASSERT_FALSE(model.empty()) << "step " << step << ": empty yet full";
      ASSERT_TRUE(front_is(held, model.front().seq)) << "step " << step;
      held.pop();
      model.pop_front();
    }
    ASSERT_EQ(held.size(), model.size());
  }
  EXPECT_GT(wraps, 100u);
  EXPECT_GT(seq, 5'000u);
}

}  // namespace
