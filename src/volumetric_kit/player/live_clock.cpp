// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/live_clock.hpp"

#include <algorithm>

namespace volumetric_kit::player {
namespace {

using std::chrono::nanoseconds;
using Clock = Playback::Clock;

// `at` in nanoseconds since its clock's epoch.
nanoseconds since_epoch(Clock::time_point at) {
  return std::chrono::duration_cast<nanoseconds>(at.time_since_epoch());
}

// A pts as a signed duration: pts are below 2^63 ns (292 years).
nanoseconds pts_ns_of(std::uint64_t pts_ns) {
  return nanoseconds(static_cast<std::int64_t>(pts_ns));
}

// The ring entry `i` places after the oldest at `head`.
std::size_t at_of(std::size_t head, std::size_t i) {
  return (head + i) % kLiveSamples;
}

// Drop the oldest of a ring while it is more than kLiveWindow before `now`:
// one exactly kLiveWindow old stays.
template <typename Sample>
void expire(const std::array<Sample, kLiveSamples>& ring, std::size_t* head,
            std::size_t* size, Clock::time_point now) {
  while (*size > 0 && now - ring[*head].at > kLiveWindow) {
    *head = at_of(*head, 1);
    --*size;
  }
}

// Add `sample` as a ring's newest, the oldest dropped when it is full.
template <typename Sample>
void push(std::array<Sample, kLiveSamples>& ring, std::size_t* head,
          std::size_t* size, const Sample& sample) {
  if (*size == kLiveSamples) {
    *head = at_of(*head, 1);
    --*size;
  }
  ring[at_of(*head, *size)] = sample;
  ++*size;
}

}  // namespace

const char* to_string(DelayNeed need) noexcept {
  switch (need) {
    case DelayNeed::kNone:
      return "none";
    case DelayNeed::kArrival:
      return "arrival";
    case DelayNeed::kProgram:
      return "program";
  }
  return "none";
}

Clock::time_point earliest(const Playout& playout,
                           std::uint64_t pts_ns) noexcept {
  return present(playout.offset, pts_ns);
}

Clock::time_point present(nanoseconds point, std::uint64_t pts_ns) noexcept {
  return Clock::time_point(
      std::chrono::duration_cast<Clock::duration>(pts_ns_of(pts_ns) + point));
}

std::int64_t pts_at(nanoseconds point, Clock::time_point at) noexcept {
  return (since_epoch(at) - point).count();
}

nanoseconds release_wait(std::uint64_t pts_ns, std::int64_t now_pts,
                         nanoseconds lead) noexcept {
  const std::int64_t due = (pts_ns_of(pts_ns) - lead).count();
  return now_pts >= due ? nanoseconds(0) : nanoseconds(due - now_pts);
}

bool mesh_now(bool free, std::optional<nanoseconds> since_publish,
              std::optional<std::uint64_t> next_pts_ns, std::int64_t now_pts,
              nanoseconds lead) noexcept {
  if (!free) {
    return false;
  }
  if (!since_publish || !next_pts_ns) {
    return true;  // the connection's first frame, or nothing held
  }
  return release_wait(*next_pts_ns, now_pts, lead) > nanoseconds(0) ||
         *since_publish >= kMeshEvery;
}

nanoseconds step_point(nanoseconds applied, nanoseconds target,
                       nanoseconds elapsed, bool settling) noexcept {
  if (target >= applied || settling) {
    return target;
  }
  // kPointFall a second of `elapsed`, exactly and with no overflow: whole
  // seconds, then the rest, under a second, scaled.
  constexpr std::int64_t kSecond = 1'000'000'000;
  const std::int64_t ns = std::max<std::int64_t>(elapsed.count(), 0);
  const nanoseconds fall((ns / kSecond) * kPointFall.count() +
                         (ns % kSecond) * kPointFall.count() / kSecond);
  return std::max(target, applied - fall);
}

std::optional<Playout> LiveClock::time() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

std::optional<nanoseconds> LiveClock::point() const noexcept {
  const std::int64_t point = point_ns_.load(std::memory_order_acquire);
  if (point == kNoPoint) {
    return std::nullopt;
  }
  return nanoseconds(point);
}

std::int64_t LiveClock::now_pts(const Playout& playout,
                                Clock::time_point now) const noexcept {
  return pts_at(playout.point, now);
}

void LiveClock::arrived(std::uint64_t pts_ns, Clock::time_point at,
                        nanoseconds program_need) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!state_) {
    first_at_ = at;  // the connection's first record
  }
  expire(arrivals_, &arrivals_head_, &arrivals_size_, at);
  const nanoseconds ahead = since_epoch(at) - pts_ns_of(pts_ns);
  nanoseconds least = ahead;  // the least with it
  for (std::size_t i = 0; i < arrivals_size_; ++i) {
    least = std::min(least, arrivals_[at_of(arrivals_head_, i)].ahead);
  }
  push(arrivals_, &arrivals_head_, &arrivals_size_, Arrival{at, ahead, least});
  program_need_ = std::max(program_need, nanoseconds(0));
  update(at);
}

void LiveClock::published(nanoseconds took, Clock::time_point at) {
  std::lock_guard<std::mutex> lock(mutex_);
  push(leads_, &leads_head_, &leads_size_,
       Lead{at, std::max(took, nanoseconds(0))});
  if (state_) {
    update(at);
  }
}

void LiveClock::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  arrivals_head_ = arrivals_size_ = 0;
  leads_head_ = leads_size_ = 0;
  program_need_ = nanoseconds(0);
  state_.reset();
  first_at_ = stepped_at_ = Clock::time_point{};
  point_ns_.store(kNoPoint, std::memory_order_release);
}

void LiveClock::update(Clock::time_point at) {
  expire(arrivals_, &arrivals_head_, &arrivals_size_, at);
  expire(leads_, &leads_head_, &leads_size_, at);
  Playout next;
  // The offset: the least over the window; with none left, as it was.
  if (arrivals_size_ == 0) {
    next.offset = state_ ? state_->offset : nanoseconds(0);
  } else {
    next.offset = arrivals_[arrivals_head_].ahead;
    for (std::size_t i = 1; i < arrivals_size_; ++i) {
      next.offset =
          std::min(next.offset, arrivals_[at_of(arrivals_head_, i)].ahead);
    }
  }
  for (std::size_t i = 0; i < leads_size_; ++i) {
    next.lead = std::max(next.lead, leads_[at_of(leads_head_, i)].took);
  }
  // Each record's lateness against the larger of the least as it arrived and
  // the least now, so a fast record entering or leaving moves no target.
  nanoseconds late(0);
  for (std::size_t i = 0; i < arrivals_size_; ++i) {
    const Arrival& arrival = arrivals_[at_of(arrivals_head_, i)];
    late = std::max(late, arrival.ahead - std::max(arrival.least, next.offset));
  }
  const nanoseconds arrival_need = late + next.lead;
  nanoseconds need = arrival_need;
  if (program_need_ > arrival_need) {
    need = program_need_;
    next.need = DelayNeed::kProgram;
  } else if (arrival_need > nanoseconds(0)) {
    next.need = DelayNeed::kArrival;
  }
  next.clamped = need > kMaxLiveDelay;
  next.target = std::min(need, kMaxLiveDelay);
  const nanoseconds target_point = next.offset + next.target;
  next.point = !state_
                   ? target_point
                   : step_point(state_->point, target_point, at - stepped_at_,
                                at - first_at_ < kLiveWindow);
  stepped_at_ = at;
  state_ = next;
  point_ns_.store(next.point.count(), std::memory_order_release);
}

}  // namespace volumetric_kit::player
