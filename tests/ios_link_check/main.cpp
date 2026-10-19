// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// Never run: CI links it for iOS (CMakeLists.txt). Taking PlayerView::create
// pulls in what the view links: the core's device, recon, gfx, ImGui and
// MoltenVK.

#include "volumetric_kit/player/player_view.hpp"

int main() {
  auto* volatile create = &volumetric_kit::player::PlayerView::create;
  return create == nullptr ? 1 : 0;
}
