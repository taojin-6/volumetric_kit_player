// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The shells' input rules on the host: a press drives the scene or a
// program's panel as it began, through a drag across the panel's edge either
// way, and with nothing held the wheel goes where the pointer is; a double
// click is a second press close in time and place, the press after one starts
// a new pair, and a press the pointer dragged away from, even back again, is no
// first click.

#include "volumetric_kit/player/pointer.hpp"

#include <gtest/gtest.h>

namespace {

using volumetric_kit::player::DoubleClick;
using volumetric_kit::player::PointerTarget;

TEST(PointerTarget, APressKeepsTheTargetItBeganOn) {
  PointerTarget target;
  EXPECT_TRUE(target.on_panel(false, true)) << "the wheel over the panel";
  EXPECT_FALSE(target.on_panel(false, false)) << "the wheel over the scene";
  EXPECT_FALSE(target.on_panel(true, false)) << "a press on the scene";
  EXPECT_FALSE(target.on_panel(true, true)) << "dragged onto the panel";
  EXPECT_TRUE(target.on_panel(false, true)) << "released over the panel";
  EXPECT_TRUE(target.on_panel(true, true)) << "a press on the panel";
  EXPECT_TRUE(target.on_panel(true, false)) << "dragged onto the scene";
  EXPECT_FALSE(target.on_panel(false, false)) << "released over the scene";
}

TEST(DoubleClick, IsASecondPressCloseInTimeAndPlace) {
  DoubleClick clicks;
  EXPECT_FALSE(clicks.press({10.0f, 10.0f}, 1.0));
  EXPECT_TRUE(clicks.press({12.0f, 10.0f}, 1.2));
  EXPECT_FALSE(clicks.press({12.0f, 10.0f}, 1.3)) << "a new pair";
  EXPECT_FALSE(clicks.press({12.0f, 10.0f}, 1.7)) << "too late";
  EXPECT_FALSE(clicks.press({30.0f, 10.0f}, 1.8)) << "too far";
  EXPECT_TRUE(clicks.press({30.0f, 12.0f}, 2.0));
}

TEST(DoubleClick, ADragIsNoFirstClick) {
  DoubleClick clicks;
  EXPECT_FALSE(clicks.press({10.0f, 10.0f}, 1.0));
  clicks.moved({17.0f, 10.0f});
  EXPECT_FALSE(clicks.press({12.0f, 10.0f}, 1.1)) << "dragged away";
  clicks.moved({12.0f, 40.0f});
  clicks.moved({12.0f, 10.0f});
  EXPECT_FALSE(clicks.press({12.0f, 10.0f}, 1.2)) << "a flick and back";
  EXPECT_TRUE(clicks.press({12.0f, 10.0f}, 1.3)) << "that press starts a pair";

  EXPECT_FALSE(clicks.press({10.0f, 10.0f}, 2.0));
  clicks.moved({10.0f + DoubleClick::kPoints, 10.0f});
  EXPECT_TRUE(clicks.press({10.0f, 10.0f}, 2.1)) << "a click's wobble";
  clicks.moved({50.0f, 50.0f});
  EXPECT_FALSE(clicks.press({50.0f, 50.0f}, 2.2)) << "after a pair, no press";
  EXPECT_TRUE(clicks.press({50.0f, 50.0f}, 2.3));
}

}  // namespace
