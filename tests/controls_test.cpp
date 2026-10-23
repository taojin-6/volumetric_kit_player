// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The controls and the stats panel on the host: status_line's,
// live_status_line's and stats_lines' wording, file and live, the deblock's
// for each filter, off and skipped; live_reason; parse_live_address; and
// draw_controls and StatsPanel on a CPU-only ImGui context fed by gfx's
// ImGuiTouch, over a stream opened but not started. A tap on Play/Pause flips
// playing(), Step pauses, Loop toggles, Normals flips the view's shading and
// shows pressed while on, hovered or not, Deblock, on at first, toggles the
// player's deblock, on the row or, where the row would not fit, below it,
// "Pause" and "Normals" fit their buttons, a finger on the controls is
// ImGui's, and they keep clear of the safe area; a live player's, never
// started, are Normals and Deblock over its status line. The Source button is
// drawn only
// when asked for, at the work area's top right clear of the safe area and of
// the stats' body, leaving the controls where they were, and a tap on it is
// one press. The stats sit at the
// work area's top left; a finger on their header is ImGui's and on their body
// the scene's; a tap on the header closes them, with nothing rebuilt, and
// another opens them with the frames taken meanwhile; their figures hold for a
// refresh; and their table is the stage means, then the renderer's rows.

#include "volumetric_kit/player/controls.hpp"

#include <gtest/gtest.h>

#include <cfloat>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/vec2.hpp>

#include "controls_layout.hpp"
#include "imgui.h"
#include "imgui_internal.h"  // FindWindowByName, to find the panels
#include "test_stream.hpp"
#include "volumetric_kit/core/base/stage_metrics.hpp"
#include "volumetric_kit/gfx/ui/imgui_touch.hpp"
#include "volumetric_kit/player/playback_stats.hpp"
#include "volumetric_kit/player/stream_player.hpp"
#include "volumetric_kit/recon/stream/session.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace stream = volumetric_kit::recon::stream;
namespace ui = volumetric_kit::gfx::ui;
using std::chrono::milliseconds;
using std::chrono::seconds;
using volumetric_kit::player::draw_controls;
using volumetric_kit::player::draw_source;
using volumetric_kit::player::FrameStamp;
using volumetric_kit::player::kControlsWindow;
using volumetric_kit::player::kSourceWindow;
using volumetric_kit::player::kStatsWindow;
using volumetric_kit::player::live_reason;
using volumetric_kit::player::live_status_line;
using volumetric_kit::player::LiveAddress;
using volumetric_kit::player::LiveState;
using volumetric_kit::player::LiveStatus;
using volumetric_kit::player::parse_live_address;
using volumetric_kit::player::PlaybackStats;
using volumetric_kit::player::stats_lines;
using volumetric_kit::player::StatsPanel;
using volumetric_kit::player::status_line;
using volumetric_kit::player::StreamPlayer;
using Lines = std::vector<std::string>;
using TimePoint = PlaybackStats::Clock::time_point;

const TimePoint t0{seconds(100)};

// A stamp of frame `index`, `bytes` long, a key record or not.
FrameStamp stamp_of(std::uint64_t index, std::uint64_t bytes, bool key) {
  FrameStamp stamp;
  stamp.index = index;
  stamp.bytes = bytes;
  stamp.key = key;
  return stamp;
}

TEST(StatusLine, SaysLoadingBeforeTheFirstFrame) {
  EXPECT_EQ(status_line(std::nullopt, 150, true, true), "loading");
}

TEST(StatusLine, NamesTheFrameFromOneThePlayStateAndLoop) {
  const FrameStamp stamp = stamp_of(11, 41'234, false);
  EXPECT_EQ(status_line(stamp, 150, true, true),
            "frame 12/150 · playing · loop");
  EXPECT_EQ(status_line(stamp, 150, false, false), "frame 12/150 · paused");
}

// 300 records of 41 kB at 30 fps: 9.84 Mbit/s.
stream::TrackStats a_track() {
  stream::TrackStats track;
  track.records = 300;
  track.payload_bytes = 300 * 41'000;
  track.max_pts_ns = 299 * 1'000'000'000ULL / 30;
  return track;
}

TEST(StatsLines, SayDashForTheFrameBeforeTheFirst) {
  const PlaybackStats stats(a_track(), {30, 1});
  EXPECT_EQ(stats_lines(stats, std::nullopt, 0, t0, false),
            (Lines{"bitrate 9.8 Mbit/s", "frame - · - kB · - k tris",
                   "deblock - · T - mm", "rate 0.0 / 30 fps"}));
}

// recon's filter names as they come, "off" with the deblock off, and
// "skipped" when it was on and ran none; T to the micrometre either way.
TEST(StatsLines, SayWhatTheDeblockDidAndTheFramesT) {
  const PlaybackStats stats(a_track(), {30, 1});
  const auto line = [&](bool on, const char* filter, float step) {
    FrameStamp stamp = stamp_of(7, 41'234, false);
    stamp.deblock_on = on;
    stamp.deblock.filter = filter;
    stamp.deblock.step = step;
    return stats_lines(stats, stamp, 182'364, t0, false)[2];
  };
  EXPECT_EQ(line(true, "LF", 0.000125f), "deblock LF · T 0.125 mm");
  EXPECT_EQ(line(true, "LG", 0.0005f), "deblock LG · T 0.500 mm");
  EXPECT_EQ(line(true, "R", 0.002f), "deblock R · T 2.000 mm");
  EXPECT_EQ(line(true, "off", 0.0005f), "deblock skipped · T 0.500 mm");
  EXPECT_EQ(line(false, "off", 0.0005f), "deblock off · T 0.500 mm");
}

TEST(StatsLines, NameTheFramesTypeBytesAndTrianglesAndTheRate) {
  PlaybackStats stats(a_track(), {30, 1});
  // 25 frames 40 ms apart: 24 steps over 0.96 s.
  for (int i = 0; i < 25; ++i) {
    stats.add_frame({}, t0 + milliseconds(40 * i));
  }
  const TimePoint now = t0 + milliseconds(960);
  EXPECT_EQ(stats_lines(stats, stamp_of(7, 41'234, false), 182'364, now, false),
            (Lines{"bitrate 9.8 Mbit/s", "frame P · 41.2 kB · 182 k tris",
                   "deblock off · T 0.000 mm", "rate 25.0 / 30 fps"}));
  EXPECT_EQ(stats_lines(stats, stamp_of(6, 98'765, true), 0, now, false)[1],
            "frame I · 98.8 kB · 0 k tris");
}

TEST(StatsLines, LeaveOutWhatIsUnknownAndShowAFractionalRate) {
  EXPECT_EQ(stats_lines(PlaybackStats({}, {0, 1}), std::nullopt, 0, t0, false),
            (Lines{"bitrate - Mbit/s", "frame - · - kB · - k tris",
                   "deblock - · T - mm", "rate 0.0 fps"}));
  EXPECT_EQ(stats_lines(PlaybackStats({}, {30000, 1001}), std::nullopt, 0, t0,
                        false)[3],
            "rate 0.0 / 29.97 fps");
  EXPECT_EQ(
      stats_lines(PlaybackStats({}, {25, 1}), std::nullopt, 0, t0, false)[3],
      "rate 0.0 / 25 fps");
}

// Live, as the view builds them: no track and no rate.
TEST(StatsLines, LiveTheyShowTheLastSecondsBitrateAndRecords) {
  PlaybackStats stats({}, {});
  EXPECT_EQ(stats_lines(stats, std::nullopt, 0, t0, true),
            (Lines{"bitrate - Mbit/s · last 1 s", "frame - · - kB · - k tris",
                   "deblock - · T - mm", "rate 0.0 fps · 0 rec/s"}));
  // 11 frames 100 ms apart, each taken as it is published after 3 records of
  // 12.5 kB: 3 Mbit/s, 30 records a second. A room's masked frames: the
  // deblock skipped them.
  FrameStamp taken = stamp_of(0, 12'500, false);
  taken.records = 3;
  taken.record_bytes = 3 * 12'500;
  taken.deblock_on = true;
  taken.deblock.step = 0.0005f;
  for (int i = 0; i <= 10; ++i) {
    taken.published = t0 + milliseconds(100 * i);
    stats.add_frame(taken, taken.published);
  }
  EXPECT_EQ(
      stats_lines(stats, taken, 1'200, t0 + seconds(1), true),
      (Lines{"bitrate 3.0 Mbit/s · last 1 s", "frame P · 12.5 kB · 1 k tris",
             "deblock skipped · T 0.500 mm", "rate 10.0 fps · 30 rec/s"}));
}

TEST(LiveStatusLine, SaysConnectingThenWhyItReconnects) {
  LiveStatus status;
  status.address = "lab:47801";
  EXPECT_EQ(live_status_line(status, t0), "connecting to lab:47801");
  status.last_error = "the sender closed the connection";
  EXPECT_EQ(live_status_line(status, t0),
            "reconnecting to lab:47801: the sender closed the connection");
}

// recon's prefix goes, as the line names the address; a message with none,
// or another address's, is shown whole.
TEST(LiveReason, DropsStreamClientsPrefixAndKeepsAnyOtherMessageWhole) {
  const auto reason = [](const char* message) {
    return live_reason(core::Status::io_error(message), "lab:47801");
  };
  EXPECT_EQ(reason("StreamClient: lab:47801: cannot connect: Connection "
                   "refused"),
            "cannot connect: Connection refused");
  EXPECT_EQ(reason("StreamClient: lab:47801: ChunkParser: the stream is not "
                   "a .vrst file"),
            "ChunkParser: the stream is not a .vrst file");
  EXPECT_EQ(reason("StreamClient::connect: an empty host"), "an empty host");
  for (const char* whole :
       {"out of memory", "the sender's session has no geometry track",
        "StreamClient: lab:1: cannot connect: Connection refused",
        "ChunkParser: the stream is not a .vrst file", "cannot connect: x",
        "StreamClient: lab:47801"}) {
    EXPECT_EQ(reason(whole), whole);
  }
}

TEST(LiveStatusLine, SaysTheSenderHasNoKeyOnceConnected) {
  LiveStatus status;
  status.state = LiveState::kConnected;
  status.address = "lab:47801";
  EXPECT_EQ(live_status_line(status, t0),
            "connected · the sender has no I-frame yet");
}

TEST(LiveStatusLine, NamesTheNewestSeqAndPastASecondHowLongNothingCame) {
  LiveStatus status;
  status.state = LiveState::kLive;
  status.address = "lab:47801";
  status.seq = 1234;
  status.last_record = t0;
  EXPECT_EQ(live_status_line(status, t0), "live · seq 1234");
  EXPECT_EQ(live_status_line(status, t0 + seconds(1)), "live · seq 1234");
  EXPECT_EQ(live_status_line(status, t0 + milliseconds(1001)),
            "live · seq 1234 · nothing for 1 s");
  EXPECT_EQ(live_status_line(status, t0 + milliseconds(3500)),
            "live · seq 1234 · nothing for 3 s");
}

TEST(ParseLiveAddress, TakesAHostAndAnOptionalPort) {
  const auto parsed = [](const char* text) {
    const core::Result<LiveAddress> address = parse_live_address(text);
    EXPECT_TRUE(address.ok()) << text << ": " << address.status().message();
    return address.ok() ? to_string(address.value()) : std::string();
  };
  EXPECT_EQ(parsed("h"), "h:47801");
  EXPECT_EQ(parsed("h:1"), "h:1");
  EXPECT_EQ(parsed("1.2.3.4:47801"), "1.2.3.4:47801");
  EXPECT_EQ(parsed("h:65535"), "h:65535");
  EXPECT_EQ(parse_live_address("lab").value().port,
            volumetric_kit::recon::networking::kDefaultPort);
}

TEST(ParseLiveAddress, RefusesNoHostAndAPortOutOfRange) {
  for (const char* text :
       {"", ":5", "h:0", "h:65536", "h:x", "h:5:6", "h:", "h:+5", "h:5 "}) {
    const core::Status refused = parse_live_address(text).status();
    EXPECT_EQ(refused.domain(), core::Status::Code::InvalidArgument)
        << "'" << text << "'";
    EXPECT_NE(refused.message().find(std::string("'") + text + "'"),
              std::string::npos)
        << refused.message();
  }
}

// A `width` x 300-point display at 2 pixels per point, `safe_area` kept
// clear.
ui::DisplayFrame display(double seconds, const ui::EdgeInsets& safe_area,
                         std::uint32_t width) {
  ui::DisplayFrame frame;
  frame.pixels = {2 * width, 600};
  frame.pixels_per_point = 2.0f;
  frame.safe_area = safe_area;
  frame.seconds = seconds;
  return frame;
}

class ControlsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const std::string path = scratch_.file("undecodable.vrst");
    const core::Status written = vp_test::write_undecodable_stream(path, 3);
    ASSERT_TRUE(written.ok()) << written.message();
    core::Result<std::unique_ptr<StreamPlayer>> opened =
        StreamPlayer::open(path);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    player_ = std::move(opened).value();
    stats_.emplace(player_->track_stats(), player_->rate());

    // ImGui's own context, as the overlay's but with no renderer: its font
    // atlas built on the CPU, and no imgui.ini.
    context_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(context_);
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto touch = ui::ImGuiTouch::create(context_);
    ASSERT_TRUE(touch.ok()) << touch.status().message();
    touch_ = std::move(touch).value();
    // An auto-sized panel takes its size on its second frame.
    frame();
    frame();
    ASSERT_NE(ImGui::FindWindowByName(kControlsWindow), nullptr);
    ASSERT_NE(ImGui::FindWindowByName(kStatsWindow), nullptr);
    ASSERT_NE(stats_body(), nullptr);
  }

  void TearDown() override {
    touch_.reset();
    if (context_ != nullptr) {
      ImGui::DestroyContext(context_);
    }
  }

  // The time the stats are drawn at: one frame on per frame.
  TimePoint now() const {
    return t0 + std::chrono::duration_cast<TimePoint::duration>(
                    std::chrono::duration<double>(seconds_));
  }

  // One frame, 1/60 s after the last, with the controls, Source while
  // `source_shown_`, and the stats in it; a press on Source is counted.
  void frame() {
    seconds_ += 1.0 / 60.0;
    touch_->new_frame(display(seconds_, safe_area_, width_));
    ImGui::NewFrame();
    draw_controls(*player_, std::nullopt, normals_);
    if (source_shown_ && draw_source()) {
      ++source_presses_;
    }
    panel_.draw(*stats_, shown_, triangles_, render_, now(), player_->live());
    ImGui::Render();
  }

  // Source drawn from the next frame; its window takes its size on its
  // second.
  void show_source() {
    source_shown_ = true;
    frame();
    frame();
  }

  // The ImGui item under a finger at `at` in the frame it lands in; 0 for
  // none. It then goes, pressing nothing.
  ImGuiID item_under(glm::vec2 at) {
    touch_->touch_began(1, at);
    frame();
    const ImGuiID item = ImGui::GetCurrentContext()->HoveredId;
    touch_->touch_cancelled(1);
    frame();
    frame();
    return item;
  }

  // The controls, as the last frame laid them out; SetUp found them.
  const ImGuiWindow& panel() const {
    return *ImGui::FindWindowByName(kControlsWindow);
  }

  // Source's window, as the last frame laid it out; null before it is first
  // drawn.
  static ImGuiWindow* source_window() {
    return ImGui::FindWindowByName(kSourceWindow);
  }

  // The stats' header, as the last frame laid it out; SetUp found it.
  static const ImGuiWindow& stats_header() {
    return *ImGui::FindWindowByName(kStatsWindow);
  }

  // The stats' body, which gfx names after the header; SetUp found it.
  static const ImGuiWindow* stats_body() {
    return ImGui::FindWindowByName(
        (std::string(kStatsWindow) + "##body").c_str());
  }

  static glm::vec2 middle(const ImGuiWindow& window) {
    return {window.Pos.x + 0.5f * window.Size.x,
            window.Pos.y + 0.5f * window.Size.y};
  }

  // The middle of the row's `index`th control (controls_layout.hpp).
  glm::vec2 control(int index) const {
    return vp_test::control_middle(panel(), index);
  }

  // Whether the last frame filled Normals, the row's last control, in a
  // pressed button's colour: a vertex of that colour in its frame. With no
  // finger pressing the panel, only its toggle draws that colour there.
  bool normals_drawn_pressed() const {
    const ImU32 pressed = ImGui::GetColorU32(ImGuiCol_ButtonActive);
    const glm::vec2 min = vp_test::control_top_left(panel(), 3);
    const glm::vec2 max = min + vp_test::control_size(panel(), 3);
    for (const ImDrawVert& vertex : panel().DrawList->VtxBuffer) {
      if (vertex.col == pressed && vertex.pos.x >= min.x - 0.5f &&
          vertex.pos.x <= max.x + 0.5f && vertex.pos.y >= min.y - 0.5f &&
          vertex.pos.y <= max.y + 0.5f) {
        return true;
      }
    }
    return false;
  }

  // A finger lifts where it lands, and the frames ImGui takes the tap over
  // run: its position, its press, its release, and its position cleared.
  void tap(glm::vec2 at) {
    touch_->touch_began(1, at);
    touch_->touch_ended(1);
    for (int i = 0; i < 4; ++i) {
      frame();
    }
  }

  // Whether a finger at `at` is ImGui's in the frame it lands in. It then
  // goes, pressing nothing.
  bool captured(glm::vec2 at) {
    touch_->touch_began(1, at);
    frame();
    const bool wants = ImGui::GetIO().WantCaptureMouse;
    touch_->touch_cancelled(1);
    frame();
    frame();
    return wants;
  }

  // The worker's frame `index` taken for drawing now, as the view's commit
  // takes it.
  void take(std::uint64_t index) {
    FrameStamp stamp = stamp_of(index, 2'000, index % 3 == 0);
    stamp.stages.add_cpu("read", 0.5);
    stamp.stages.add_cpu("decode", 2.0);
    stamp.stages.add_cpu("mesh", 1.0);
    stats_->add_frame(stamp, now());
    shown_ = std::move(stamp);
    triangles_ = 1'200;
  }

  vp_test::Scratch scratch_;
  std::unique_ptr<StreamPlayer> player_;
  bool normals_ = false;  // the view's shading, which the controls flip
  std::optional<PlaybackStats> stats_;
  StatsPanel panel_;
  std::optional<FrameStamp> shown_;
  std::uint32_t triangles_ = 0;
  std::vector<core::StageRow> render_;
  bool source_shown_ = false;
  int source_presses_ = 0;
  ui::EdgeInsets safe_area_;
  std::uint32_t width_ = 400;  // the display's, in points
  ImGuiContext* context_ = nullptr;
  std::unique_ptr<ui::ImGuiTouch> touch_;  // reset before the context
  double seconds_ = 0.0;
};

TEST_F(ControlsTest, ATapOnPlayPauseFlipsPlaying) {
  ASSERT_TRUE(player_->playing());
  tap(control(0));
  EXPECT_FALSE(player_->playing());
  tap(control(0));
  EXPECT_TRUE(player_->playing());
}

TEST_F(ControlsTest, StepPauses) {
  ASSERT_TRUE(player_->playing());
  tap(control(1));
  EXPECT_FALSE(player_->playing());
}

TEST_F(ControlsTest, LoopToggles) {
  ASSERT_TRUE(player_->loop());
  tap(control(2));
  EXPECT_FALSE(player_->loop());
  EXPECT_TRUE(player_->playing());
  tap(control(2));
  EXPECT_TRUE(player_->loop());
}

// Pressed while on, under a pointer too, as a mouse rests on it after a click.
TEST_F(ControlsTest, NormalsFlipsTheViewsShadingAndShowsPressedWhileOn) {
  ASSERT_FALSE(normals_);
  EXPECT_FALSE(normals_drawn_pressed());
  tap(control(3));
  EXPECT_TRUE(normals_);
  EXPECT_TRUE(normals_drawn_pressed());
  EXPECT_TRUE(player_->playing());
  EXPECT_TRUE(player_->loop());
  touch_->touch_began(1, control(3));
  frame();  // its position alone: hovered, not held
  EXPECT_TRUE(normals_drawn_pressed()) << "hovered";
  touch_->touch_cancelled(1);
  frame();
  frame();
  EXPECT_TRUE(normals_);
  tap(control(3));
  EXPECT_FALSE(normals_);
  EXPECT_FALSE(normals_drawn_pressed());
}

TEST_F(ControlsTest, DeblockIsOnAtFirstAndATapTogglesIt) {
  ASSERT_TRUE(player_->deblock());
  ASSERT_TRUE(vp_test::deblock_on_row(panel()));
  tap(control(vp_test::kDeblock));
  EXPECT_FALSE(player_->deblock());
  EXPECT_TRUE(player_->playing());
  EXPECT_TRUE(player_->loop());
  EXPECT_FALSE(normals_);
  tap(control(vp_test::kDeblock));
  EXPECT_TRUE(player_->deblock());
}

// 340 points across, the row with Deblock would be wider than the panel may
// grow, so Deblock is below it, at its left, and the status line below that;
// a tap there toggles it.
TEST_F(ControlsTest, WhereTheRowWouldNotFitDeblockIsBelowIt) {
  width_ = 340;
  frame();
  frame();
  const ImGuiWindow& window = panel();
  EXPECT_FALSE(vp_test::deblock_on_row(window));
  const glm::vec2 deblock =
      vp_test::control_top_left(window, vp_test::kDeblock);
  EXPECT_EQ(deblock.x, vp_test::control_top_left(window, 0).x);
  const float row = vp_test::control_size(window, 0).y;
  const float spacing = ImGui::GetStyle().ItemSpacing.y;
  EXPECT_NEAR(window.ContentSize.y,
              row + spacing + row + spacing + window.FontRefSize, 1.0f);
  EXPECT_LE(window.Size.x, 340.0f);
  EXPECT_EQ(item_under(control(vp_test::kDeblock)),
            ImGui::FindWindowByName(kControlsWindow)->GetID("Deblock"));
  tap(control(vp_test::kDeblock));
  EXPECT_FALSE(player_->deblock());
  EXPECT_TRUE(player_->playing()) << "the tap pressed Deblock alone";
}

// At the desktop's scale, this context's, as at the iOS shell's
// (player_view_test), "Pause" and "Normals" fit their buttons.
TEST_F(ControlsTest, PauseAndNormalsFitTheirButtons) {
  const float padding = 2.0f * ImGui::GetStyle().FramePadding.x;
  EXPECT_GE(vp_test::control_size(panel(), 0).x,
            vp_test::text_width(panel(), "Pause") + padding);
  EXPECT_GE(vp_test::control_size(panel(), 3).x,
            vp_test::text_width(panel(), "Normals") + padding);
}

// A live player's controls are Normals and Deblock over its status line: a
// frame and a line tall and as wide as the line, which a row with
// Play/Pause, Step and Loop would outgrow. A tap on Normals flips the view's
// shading, and one on Deblock the player's deblock.
TEST_F(ControlsTest,
       ALivePlayersControlsAreNormalsAndDeblockOverItsStatusLine) {
  core::Result<std::unique_ptr<StreamPlayer>> live =
      StreamPlayer::connect({"127.0.0.1", 47801});  // never started: no I/O
  ASSERT_TRUE(live.ok()) << live.status().message();
  player_ = std::move(live).value();
  frame();
  frame();
  const ImGuiWindow& window = panel();
  const float line = window.FontRefSize;
  EXPECT_NEAR(window.ContentSize.y,
              vp_test::control_size(window, 0).y +
                  ImGui::GetStyle().ItemSpacing.y + line,
              1.0f);
  const float width =
      ImGui::GetIO()
          .Fonts->Fonts[0]
          ->CalcTextSizeA(line, FLT_MAX, 0.0f, "connecting to 127.0.0.1:47801")
          .x;
  EXPECT_NEAR(window.ContentSize.x, width, 1.0f);
  tap(control(0));
  EXPECT_TRUE(normals_);
  tap(control(0));
  EXPECT_FALSE(normals_);
  EXPECT_TRUE(player_->deblock());
  tap(vp_test::live_deblock_middle(panel()));
  EXPECT_FALSE(player_->deblock());
  EXPECT_FALSE(normals_);
}

// Asked for, all of its window is the button, to its corner.
TEST_F(ControlsTest, SourceIsDrawnOnlyWhenAskedFor) {
  EXPECT_EQ(source_window(), nullptr);
  show_source();
  ASSERT_NE(source_window(), nullptr);
  const ImGuiID source = source_window()->GetID("Source");
  EXPECT_EQ(item_under(vp_test::source_middle(*source_window())), source);
  EXPECT_EQ(item_under(vp_test::source_top_left(*source_window()) +
                       glm::vec2(0.5f, 0.5f)),
            source);
  EXPECT_EQ(source_presses_, 0) << "the fingers pressed nothing";
}

// The controls stay where they were, and work as before.
TEST_F(ControlsTest, ATapOnSourceIsOnePressAndTheControlsStayPut) {
  const glm::vec2 play = control(0);
  const float height = panel().Size.y;
  show_source();
  EXPECT_EQ(control(0), play);
  EXPECT_EQ(panel().Size.y, height);
  tap(vp_test::source_middle(*source_window()));
  EXPECT_EQ(source_presses_, 1);
  for (int i = 0; i < 10; ++i) {
    frame();
  }
  EXPECT_EQ(source_presses_, 1) << "one tap, one press";
  EXPECT_TRUE(player_->playing());
  tap(control(0));
  EXPECT_FALSE(player_->playing());
  EXPECT_EQ(source_presses_, 1);
}

// A phone's insets in landscape, the Dynamic Island's side on the right: the
// window is the button, kButtonEms font sizes wide and as tall as the stats'
// header, a frame, so their body clears it. ImGui snaps a window's position
// and size to whole points.
TEST_F(ControlsTest, SourceSitsAtTheWorkAreasTopRightClearOfTheSafeArea) {
  show_source();
  const ImGuiWindow& source = *source_window();
  EXPECT_EQ(source.Pos.x + source.Size.x, 400.0f);
  EXPECT_EQ(source.Pos.y, 0.0f);
  safe_area_.top = 62.0f;
  safe_area_.right = 62.0f;
  frame();
  EXPECT_EQ(source.Pos.x + source.Size.x, 338.0f);
  EXPECT_EQ(source.Pos.y, 62.0f);
  EXPECT_NEAR(source.Size.x, vp_test::control_size(source, 0).x, 1.0f);
  EXPECT_EQ(source.Size.y, stats_header().Size.y);
  EXPECT_LE(source.Pos.y + source.Size.y, stats_body()->Pos.y);
}

TEST_F(ControlsTest, AFingerOnThePanelIsImGuis) {
  const ImGuiIO& io = ImGui::GetIO();
  touch_->touch_began(1, control(1));
  frame();
  EXPECT_TRUE(io.WantCaptureMouse);
  touch_->touch_ended(1);
  frame();
  frame();
  frame();
  EXPECT_FALSE(io.WantCaptureMouse);
  touch_->touch_began(2, {390.0f, 10.0f});  // above the panel, by no stats
  frame();
  EXPECT_FALSE(io.WantCaptureMouse);
  touch_->touch_ended(2);
}

// ImGui snaps a window's position to whole points.
TEST_F(ControlsTest, ThePanelSitsAtTheBottomCentreClearOfTheSafeArea) {
  const ImGuiWindow& window = panel();
  EXPECT_NEAR(window.Pos.x + 0.5f * window.Size.x, 200.0f, 1.0f);
  EXPECT_NEAR(window.Pos.y + window.Size.y, 300.0f, 1.0f);
  safe_area_.bottom = 34.0f;
  frame();
  EXPECT_NEAR(panel().Pos.y + panel().Size.y, 266.0f, 1.0f);
}

TEST_F(ControlsTest, TheFontDrawsTheStatusLinesDot) {
  EXPECT_TRUE(ImGui::GetFont()->IsGlyphInFont(0x00B7));
}

// A phone's insets in landscape: the Dynamic Island's side, and the top.
TEST_F(ControlsTest, TheStatsSitAtTheWorkAreasTopLeftClearOfTheSafeArea) {
  EXPECT_EQ(stats_header().Pos.x, 0.0f);
  EXPECT_EQ(stats_header().Pos.y, 0.0f);
  safe_area_.top = 62.0f;
  safe_area_.left = 62.0f;
  frame();
  EXPECT_EQ(stats_header().Pos.x, 62.0f);
  EXPECT_EQ(stats_header().Pos.y, 62.0f);
  // The body directly below the header.
  EXPECT_EQ(stats_body()->Pos.x, 62.0f);
  EXPECT_EQ(stats_body()->Pos.y, 62.0f + stats_header().Size.y);
}

TEST_F(ControlsTest, AFingerOnTheStatsHeaderIsImGuisAndOnTheirBodyTheScenes) {
  EXPECT_TRUE(captured(middle(stats_header())));
  EXPECT_FALSE(captured(middle(*stats_body())));
  EXPECT_TRUE(panel_.open()) << "the fingers pressed nothing";
}

TEST_F(ControlsTest, ATapOnTheStatsHeaderClosesThemAndAnotherOpensThem) {
  ASSERT_TRUE(panel_.open());
  ASSERT_TRUE(stats_body()->Active);
  const Lines before = panel_.lines();
  ASSERT_EQ(before.size(), 4u);
  EXPECT_EQ(before[1], "frame - · - kB · - k tris");
  EXPECT_EQ(before[3], "rate 0.0 / 25 fps");
  tap(middle(stats_header()));
  EXPECT_FALSE(panel_.open());
  EXPECT_FALSE(stats_body()->Active) << "closed, the header alone";
  // Half a second closed, past the refresh, with a frame taken every other
  // tick for the last of it: nothing is rebuilt.
  for (int tick = 0; tick < 30; ++tick) {
    if (tick % 2 == 0) {
      take(static_cast<std::uint64_t>(tick / 2));
    }
    frame();
  }
  EXPECT_FALSE(stats_body()->Active);
  EXPECT_EQ(panel_.lines(), before) << "rebuilt while closed";
  // Open again, from the frame the tap opens it on, it shows the frames taken
  // while it was closed: the newest, frame 14, and the 15 over 28 ticks, 30 a
  // second.
  touch_->touch_began(1, middle(stats_header()));
  touch_->touch_ended(1);
  for (int i = 0; i < 4 && !panel_.open(); ++i) {
    frame();
  }
  EXPECT_TRUE(panel_.open());
  EXPECT_TRUE(stats_body()->Active);
  EXPECT_EQ(panel_.lines(),
            (Lines{before[0], "frame P · 2.0 kB · 1 k tris",
                   "deblock off · T 0.000 mm", "rate 30.0 / 25 fps"}));
}

// A frame taken within a refresh of the last rebuild leaves the figures as
// they were; the first frame past it shows it.
TEST_F(ControlsTest, TheFiguresHoldUntilTheRefresh) {
  const double refresh =
      std::chrono::duration<double>(StatsPanel::kRefresh).count();
  take(0);
  seconds_ += refresh;
  frame();
  const Lines first = panel_.lines();
  ASSERT_EQ(first.size(), 4u);
  EXPECT_EQ(first[1], "frame I · 2.0 kB · 1 k tris");
  take(1);
  frame();
  EXPECT_EQ(panel_.lines(), first) << "rebuilt within the refresh";
  seconds_ += refresh;
  frame();
  EXPECT_EQ(panel_.lines()[1], "frame P · 2.0 kB · 1 k tris");
}

TEST_F(ControlsTest, TheStatsTableIsTheStageMeansThenTheRenderersRows) {
  render_ = {{"draw", 0.4, 0.3, true}};
  take(0);
  // A frame past the refresh rebuilds.
  seconds_ += std::chrono::duration<double>(StatsPanel::kRefresh).count();
  frame();
  const std::vector<core::StageRow>& rows = panel_.rows();
  ASSERT_EQ(rows.size(), 4u);
  EXPECT_STREQ(rows[0].name, "read");
  EXPECT_STREQ(rows[1].name, "decode");
  EXPECT_STREQ(rows[2].name, "mesh");
  EXPECT_STREQ(rows[3].name, "draw");
  EXPECT_DOUBLE_EQ(rows[1].cpu_ms, 2.0);
  EXPECT_TRUE(rows[3].has_gpu);
}

}  // namespace
