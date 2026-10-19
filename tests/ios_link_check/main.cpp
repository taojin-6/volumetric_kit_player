// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// Never run: CI links it for iOS (CMakeLists.txt). Taking PlayerView::create
// loads the view, and through it the whole library and what that links: the
// core's device, recon, gfx, ImGui and MoltenVK. The volatile keeps the
// reference in an optimized build.

#include "volumetric_kit/player/player_view.hpp"

int main() {
  auto* volatile create = &volumetric_kit::player::PlayerView::create;
  return create == nullptr ? 1 : 0;
}
