// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/frame_hooks.hpp"

#include <utility>

#include "volumetric_kit/core/base/check.hpp"

namespace volumetric_kit::player {

namespace mesh = grid::mesh;

FrameTaker::FrameTaker(StreamPlayer& player, FrameExchange& exchange,
                       std::uint32_t frames_in_flight, const FrameHooks* hooks)
    : player_(&player),
      exchange_(&exchange),
      hooks_(hooks),
      slot_extras_(frames_in_flight) {
  VKC_CHECK(frames_in_flight > 0,
            "FrameTaker: frames_in_flight must be at least 1");
}

core::Result<std::optional<FrameStamp>> FrameTaker::take(std::uint32_t slot) {
  VKC_CHECK(slot < slot_extras_.size(), "FrameTaker::take: slot out of range");
  slot_extras_[slot].reset();  // its last frame has completed
  std::optional<FrameStamp> drawn;
  core::Status failed;
  // Held: counted once a frame, and kept back for the next take.
  const auto hold = [this] {
    if (!held_offered_) {
      held_offered_ = true;
      ++held_;
    }
    return mesh::CommitDecision::kPark;
  };
  exchange_->begin_frame(
      slot, [&](const mesh::DeviceMesh& taken, FrameStamp& stamp) {
        if (taken.generation != offered_) {
          offered_ = taken.generation;
          held_offered_ = false;
          player_->taken();
        }
        // after_mesh's batch may still run: drawn only behind a value reached.
        if (taken.ready.semaphore != nullptr) {
          const core::Result<std::uint64_t> reached =
              taken.ready.semaphore->value();
          if (!reached.ok()) {
            failed = reached.status();
            return hold();
          }
          if (*reached < taken.ready.value) {
            return hold();
          }
        }
        FrameCommit decided = FrameCommit::kDraw;
        if (hooks_ != nullptr && hooks_->commit) {
          core::Result<FrameCommit> said = hooks_->commit(taken, stamp);
          if (!said.ok()) {
            failed = said.status();
            return hold();
          }
          decided = *said;
        }
        // One skip between two frames drawn: the ring holds one dropped mesh.
        if (decided == FrameCommit::kSkip && skipped_since_draw_) {
          decided = FrameCommit::kHold;
        }
        switch (decided) {
          case FrameCommit::kHold:
            return hold();
          case FrameCommit::kSkip:
            skipped_since_draw_ = true;
            ++skipped_;
            return mesh::CommitDecision::kDrop;
          case FrameCommit::kDraw:
            break;
        }
        skipped_since_draw_ = false;
        stamp.taken = Playback::Clock::now();
        live_extra_ = stamp.extra;
        drawn = std::move(stamp);
        return mesh::CommitDecision::kCommit;
      });
  slot_extras_[slot] = live_extra_;
  if (!failed.ok()) {
    return failed;
  }
  return drawn;
}

}  // namespace volumetric_kit::player
