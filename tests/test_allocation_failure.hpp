// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

#include <cstddef>

namespace vp_test {

// Link test_allocation_failure.cpp only into tests that inject failures or
// measure allocations. The replacement operators are compiled separately from
// their callers so GCC does not inline delete's free and mistake it for a
// mismatched deallocation.
struct AllocationFailure {
  bool measure = false;
  std::size_t bytes = 0;
  std::size_t largest = 0;  // the largest allocation while measuring
  const void* largest_site = nullptr;
  bool armed = false;
  bool injected = false;
  const void* injected_site = nullptr;
};

// Limit injection to the calling thread, so a driver's worker cannot consume
// it.
extern thread_local AllocationFailure allocation_failure;

}  // namespace vp_test
