// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/pointer.hpp"

#include <glm/geometric.hpp>

namespace volumetric_kit::player {

bool PointerTarget::on_panel(bool held, bool over_panel) noexcept {
  if (held && !held_) {
    began_on_panel_ = over_panel;
  }
  held_ = held;
  return held ? began_on_panel_ : over_panel;
}

bool DoubleClick::press(glm::vec2 at, double seconds) noexcept {
  const bool second = first_ && seconds - first_->seconds <= kSeconds &&
                      glm::distance(at, first_->at) <= kPixels;
  if (second) {
    first_.reset();
  } else {
    first_ = Press{at, seconds};
  }
  return second;
}

}  // namespace volumetric_kit::player
