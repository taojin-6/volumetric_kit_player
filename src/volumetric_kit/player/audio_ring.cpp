// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/audio_ring.hpp"

#include <algorithm>

namespace volumetric_kit::player {
namespace {

// The least power of two at or above `n`, at least 1.
std::size_t power_of_two_at_least(std::size_t n) {
  std::size_t p = 1;
  while (p < n) {
    p <<= 1;
  }
  return p;
}

}  // namespace

AudioRing::AudioRing(std::size_t frames, std::uint16_t channels)
    : mask_(power_of_two_at_least(frames) - 1),
      channels_(std::max<std::uint16_t>(channels, 1)) {
  samples_.resize((mask_ + 1) * channels_);
}

template <typename Run>
void AudioRing::runs(std::uint64_t at, std::size_t frames, Run&& run) noexcept {
  std::size_t start = static_cast<std::size_t>(at) & mask_;
  std::size_t done = 0;
  while (done < frames) {
    const std::size_t n = std::min(frames - done, capacity() - start);
    run(samples_.data() + start * channels_, done * channels_, n * channels_);
    done += n;
    start = 0;
  }
}

std::size_t AudioRing::write(const float* samples,
                             std::size_t frames) noexcept {
  const std::uint64_t w = write_.load(std::memory_order_relaxed);
  const std::uint64_t r = read_.load(std::memory_order_acquire);
  const std::size_t room = capacity() - static_cast<std::size_t>(w - r);
  const std::size_t n = std::min(frames, room);
  runs(w, n, [samples](float* ring, std::size_t from, std::size_t count) {
    if (samples == nullptr) {
      std::fill_n(ring, count, 0.0f);
    } else {
      std::copy_n(samples + from, count, ring);
    }
  });
  write_.store(w + n, std::memory_order_release);
  return n;
}

std::size_t AudioRing::read(float* out, std::size_t frames) noexcept {
  const std::uint64_t r = read_.load(std::memory_order_relaxed);
  const std::uint64_t w = write_.load(std::memory_order_acquire);
  const std::size_t n = std::min(frames, static_cast<std::size_t>(w - r));
  runs(r, n, [out](float* ring, std::size_t to, std::size_t count) {
    std::copy_n(ring, count, out + to);
  });
  read_.store(r + n, std::memory_order_release);
  return n;
}

void AudioRing::skip_to(std::uint64_t index) noexcept {
  if (index > read_.load(std::memory_order_relaxed)) {
    read_.store(index, std::memory_order_release);
  }
}

}  // namespace volumetric_kit::player
