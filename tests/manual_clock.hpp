// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file manual_clock.hpp
/// A steady_clock time a test moves by hand, for the null audio output
/// (NullAudioOut's Clock): safe to read from any thread.

#include <atomic>
#include <chrono>
#include <cstdint>

namespace vp_test {

class ManualClock {
 public:
  std::chrono::steady_clock::time_point operator()() const {
    return std::chrono::steady_clock::time_point(
        std::chrono::nanoseconds(ns_.load()));
  }
  void advance(std::chrono::nanoseconds by) { ns_ += by.count(); }

 private:
  std::atomic<std::int64_t> ns_{1'000'000};
};

}  // namespace vp_test
