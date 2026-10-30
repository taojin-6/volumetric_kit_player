// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/held_records.hpp"

#include <algorithm>

namespace volumetric_kit::player {

std::uint8_t* HeldRecords::room(std::size_t size) {
  if (size_ == kHeldRecords) {
    return nullptr;
  }
  if (size_ == 0) {
    if (bytes_ == nullptr || size > capacity_) {
      capacity_ = std::max(capacity_, size);  // larger: held alone
      bytes_.reset(new std::uint8_t[capacity_]);
    }
    return bytes_.get();
  }
  const std::size_t first = held_[head_].at;
  if (upper_ == 0) {  // one span, [first, tail_)
    if (capacity_ - tail_ >= size) {
      return bytes_.get() + tail_;
    }
    return first >= size ? bytes_.get() : nullptr;  // wrapped to the start
  }
  // Wrapped: the oldest from `first` up, the newest in [0, tail_).
  return first - tail_ >= size ? bytes_.get() + tail_ : nullptr;
}

void HeldRecords::push(const grid::stream::Record& record,
                       Clock::time_point arrived, const std::uint8_t* at) {
  const std::size_t offset = static_cast<std::size_t>(at - bytes_.get());
  if (size_ == 0) {
    upper_ = 0;
  } else if (upper_ == 0 && offset != tail_) {
    upper_ = size_;  // wrapped to the start: every record held lies above
  }
  held_[(head_ + size_) % kHeldRecords] = Held{record, arrived, offset};
  ++size_;
  tail_ = offset + static_cast<std::size_t>(record.size);
}

void HeldRecords::pop() noexcept {
  head_ = (head_ + 1) % kHeldRecords;
  --size_;
  if (upper_ > 0) {
    --upper_;  // at 0, the records left are one span from the start
  }
}

}  // namespace volumetric_kit::player
