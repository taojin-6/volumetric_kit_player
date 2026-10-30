// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file controls.hpp
/// @brief The playback controls and the stats panel, drawn on the view's
///        ImGui context. Player-internal.

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec2.hpp>

#include "volumetric_kit/core/base/stage_metrics.hpp"
#include "volumetric_kit/gfx/core/frame_metrics.hpp"
#include "volumetric_kit/gfx/ui/metrics_panel.hpp"
#include "volumetric_kit/player/playback.hpp"
#include "volumetric_kit/player/playback_stats.hpp"
#include "volumetric_kit/player/stream_player.hpp"

namespace volumetric_kit::player {

/// The renderer's row for its one pass, the mesh and the UI together, as
/// Apple GPUs time only a pass's bounds (PlayerView::frame); compact stats
/// show it as "draw".
inline constexpr const char* kDrawRow = "draw (mesh+UI)";
/// The controls' ImGui window.
inline constexpr const char* kControlsWindow = "##player_controls";
/// The stats panel's header window; its body's is derived from it
/// (gfx::ui::begin_collapsible_panel).
inline constexpr const char* kStatsWindow = "##player_stats";
/// The Source button's ImGui window (@ref draw_source).
inline constexpr const char* kSourceWindow = "##player_source";
/// The width of the Play/Pause, Step, Normals and Source buttons, in font
/// sizes, so each is a stable target: "Pause" and "Normals" fit at the
/// desktop's scale and, with "Source", at the iOS shell's, and a phone's row
/// stays inside 375 pt.
inline constexpr float kButtonEms = 4.5f;

/// @brief A rectangle of ImGui's display, in points: where the controls, the
///        stats and Source are laid out.
struct UiArea {
  glm::vec2 pos{0.0f, 0.0f};   ///< Its top-left.
  glm::vec2 size{0.0f, 0.0f};  ///< Its size.
};

/// @brief The main viewport's work area: the safe area on a phone
///        (gfx::ui::ImGuiTouch), the window on a desktop.
///
/// @code
/// draw_source(work_area());
/// @endcode
/// @return It, in points.
/// @pre An ImGui frame is open.
UiArea work_area();

/// @brief Draw Play/Pause, Step, Loop, Normals, Deblock, Unlit and, when
///        given, Texture and a program's panel's box, and Mute for a file
///        with audio (StreamPlayer::has_audio), in a row, the status
///        line below, at the bottom centre of @p area, so they stay clear of
///        a notch, a home indicator and the panel; a live player's
///        (StreamPlayer::live), Normals, Deblock, Unlit and, when given,
///        Texture and the panel's box over its @ref live_status_line.
///        Deblock, a box that shows and sets StreamPlayer::deblock, then
///        Unlit, Texture, the panel's and Mute, which shows and sets
///        StreamPlayer::muted, boxes, each go after the line's
///        last control, or start a line below when it would not fit @p area
///        with them, as Deblock does on a phone in portrait; a status line
///        too wide for it breaks between its parts, as a stats line does.
///        There is no text field, so touch needs no keyboard.
///
/// @code
/// overlay.new_frame();
/// draw_controls(player, shown, normals, unlit, &textured, work_area(),
///               "Inspect", &panel_open);
/// @endcode
/// @param player      The controls act on it; they are safe from this thread.
/// @param shown       The last stream frame drawn, for the status line.
/// @param normals     The view's shading (PlayerView::normals): a press on
///                    Normals flips it, and the button shows pressed while
///                    on.
/// @param unlit       The view's Unlit (PlayerView::unlit), which its box
///                    shows and a press flips.
/// @param textured    The view's Texture (PlayerView::textured), which its
///                    box shows and a press flips; null draws no Texture box,
///                    as for a view with no texture bound.
/// @param area        Where they go (@ref work_area, or scene_area).
/// @param panel_label  The panel's box's label (PanelHooks::label); null
///                     draws no box, as for a program with no panel.
/// @param panel_open  Whether the panel is open, which its box shows and a
///                    press flips; given with @p panel_label.
/// @pre An ImGui frame is open on the view's context.
void draw_controls(StreamPlayer& player, const std::optional<FrameStamp>& shown,
                   bool& normals, bool& unlit, bool* textured,
                   const UiArea& area, const char* panel_label = nullptr,
                   bool* panel_open = nullptr);

/// @brief Draw the Source button, which asks for something else to play, at
///        the top right of @p area. Its window has no padding or border, so
///        the button is all of it, a frame tall, as the stats' header is: the
///        stats' body below the header clears it, as the controls at the
///        bottom do, in either orientation.
///
/// @code
/// overlay.new_frame();
/// if (draw_source(work_area())) {
///   source_requested = true;  // PlayerView::take_source_request
/// }
/// @endcode
/// @param area  Where it goes (@ref work_area, or scene_area).
/// @return Whether a tap or click pressed it in this frame.
/// @pre An ImGui frame is open on the view's context.
bool draw_source(const UiArea& area);

/// @brief A live player's status line.
///
/// @code
/// // "live · seq 1234 · set 77 · nothing for 3 s"
/// const std::string line =
///     live_status_line(player.live_status(), Playback::Clock::now());
/// @endcode
/// @param status  Its connection (StreamPlayer::live_status).
/// @param now     The time, for how long no record has come.
/// @return "connecting to <address>", or with a `last_error`
///         "reconnecting to <address>: <last_error>";
///         "connected · the sender has no I-frame yet"; "live · seq <seq>",
///         with " · set <set>" when the newest record names one, and
///         " · nothing for <n> s" once no record has come for over a second,
///         n in whole seconds.
std::string live_status_line(const LiveStatus& status,
                             Playback::Clock::time_point now);

/// @brief A file's audio on its status line.
///
/// @code
/// const std::string text = audio_status_text(*player.audio_status());
/// @endcode
/// @param audio  Its audio (StreamPlayer::audio_status).
/// @return "muted" while muted; else "audio <n> ms", n the route's latency
///         to the nearest ms, while a device plays it; else "audio off" (the
///         null output, or a device stopped).
std::string audio_status_text(const AudioStatus& audio);

/// @brief The status line under the controls.
///
/// @code
/// // "frame 12/150 · set 77 · playing · loop · audio 23 ms"
/// const std::string line =
///     status_line(stamp, 150, true, true, player.audio_status());
/// @endcode
/// @param shown        The last stream frame drawn; empty before the first.
/// @param frame_count  The track's frames.
/// @param playing      Whether it plays, rather than being paused.
/// @param loop         Whether it loops.
/// @param audio        Its audio; none without an Audio track.
/// @return The frame drawn, counted from 1, over the track's; its set
///         (FrameStamp::set) when it has one; `playing` or `paused`; `loop`
///         when on; and with audio its @ref audio_status_text:
///         "frame 12/150 · set 77 · playing · loop · audio 23 ms". "loading"
///         before the first.
std::string status_line(const std::optional<FrameStamp>& shown,
                        std::uint64_t frame_count, bool playing, bool loop,
                        const std::optional<AudioStatus>& audio = {});

/// @brief A live view's playout delay, for the stats: the applied D of the
///        shown frame's clock as it was taken (FrameStamp::playout), and the
///        need that set its target (live_clock.hpp).
///
/// @code
/// // "delay 123 ms · program"; past the clamp, "delay 1000 ms · program ·
/// // over 1 s"; before the first frame, "delay - ms"
/// const std::string line = delay_line(shown);
/// @endcode
/// @param shown  The newest frame drawn; empty before the first.
/// @return The line, D to the millisecond.
std::string delay_line(const std::optional<FrameStamp>& shown);

/// @brief The stats panel's lines, in Mbit/s (10^6 bit/s), kB (1000 B), mm
///        and ms.
///
/// @code
/// // {"bitrate 11.8 Mbit/s", "frame P · 41.2 kB · 182 k tris",
/// //  "deblock LG · T 0.500 mm · 12 of 1,920 blocks exact",
/// //  "GPU 6.30 ms · idle 26.9 ms · queue 0.41 ms",
/// //  "latency 9.8 ms · publish 0.2 · take 8.5 · present 1.1",
/// //  "rate 29.8 / 30 fps"}
/// const auto lines = stats_lines(stats, shown, triangles, now, false);
/// // live: {"bitrate 9.7 Mbit/s · last 1 s", "frame P · ...",
/// //        "deblock off · T 0.500 mm", "GPU ...",
/// //        "latency 141.0 ms · buffer 121.3 · publish 9.1 · ...",
/// //        "delay 123 ms · program", "rate 29.8 fps · 30 rec/s"}
/// // compact: the GPU and latency lines as one, "GPU 6.30 ms · latency
/// //          9.8 ms"; live, the delay's before it
/// // with a program's text, "saved 30 meshes/s\n61.2 MB/s", its lines last
/// @endcode
/// @param stats      The figures.
/// @param shown      The newest frame drawn; empty before the first.
/// @param triangles  Its mesh's triangles.
/// @param now        The time, for the rates.
/// @param live       Whether they are a live player's (StreamPlayer::live).
/// @param compact    Whether for the compact stats (StatsPanel), a phone's.
/// @param program_line  A program's text (FrameHooks::stats_line); empty,
///                      none.
/// @return The track's mean bitrate, or live its bitrate over the last
///         second (`-` when unknown); the frame's type (I for a key record,
///         else P), payload and triangles (each `-` before the first); what
///         the deblock did to it (FrameStamp::deblock): the filter grid ran
///         (`LF`, `LG` or `R`) or `off`, for the deblock off or a T finer
///         than every filter's, then its T to the micrometre (each `-`
///         before the first), and after a filter how many of the frame's
///         blocks grid's per-block fallback kept at their exact decode
///         (`codec::DeblockInfo::exact_blocks`); the frames' device time, the
///         worker's queue idle before its batch and its queue latency
///         (PlaybackStats::frame_means), each `n/t` while no frame of the
///         window has it timed, the device time and the queue latency `n/a`
///         where timed frames have none (a queue that cannot time, a device
///         that cannot calibrate); the latency from a frame's due time to
///         its present call, over the frames presented, and its legs, live
///         due to released first, then released to published, published to
///         taken and taken to presented (each `-` while unknown); live, the
///         @ref delay_line; and the
///         rate frames are taken at, over the nominal when it is known, or
///         live beside the records a second; and each line of
///         @p program_line, split at '\n', but the empty ones. Compact, the
///         device time and the latency share one line, and live the delay's
///         comes before it.
std::vector<std::string> stats_lines(const PlaybackStats& stats,
                                     const std::optional<FrameStamp>& shown,
                                     std::uint32_t triangles,
                                     Playback::Clock::time_point now, bool live,
                                     bool compact = false,
                                     std::string_view program_line = {});

/// @brief The line over the stats' render table.
///
/// @code
/// // "display 59.9 fps"
/// const std::string line = display_line(profiler.metrics());
/// @endcode
/// @param render  The renderer's frame (gfx::Profiler::metrics).
/// @return "display <fps> fps", to one place: the rate frames are drawn at;
///         "display - fps" with no rate, as before the profiler's first
///         frame or with none attached.
std::string display_line(const gfx::FrameMetrics& render);

/// @brief The "Stats" panel, at the top left of its area (the work area, or
///        the scene's part of it: scene_area): a header
///        (gfx::ui::begin_collapsible_panel) and below it, while
///        open, on a 60 % background, the @ref stats_lines, the worker's stage
///        table, per stream frame, and the renderer's, per display frame under
///        its @ref display_line (gfx::ui::draw_stage_table, each under a line
///        stating its gfx::ui::StageWindow). Only the header takes input: a
///        finger or the mouse on the body reaches the camera.
///
/// A line wider than the room it has breaks between its parts, at its " · ",
/// each row holding as many parts as fit and the rows after the first
/// indented, so the panel stays on screen: the deblock line with its blocks on
/// a phone breaks before them.
///
/// It opens at launch, and a tap or click on the header closes and opens it.
/// While it is open its figures are rebuilt every @ref kRefresh, so the
/// digits stay readable; while it is closed nothing is rebuilt, but the
/// @ref PlaybackStats it reads keeps counting, so it reopens with current
/// figures. A program's text, given, is read at a rebuild once
/// @ref kProgramLineRefresh has passed since its last read, and shown until
/// the next. Not thread-safe: the drawing thread owns it.
///
/// **Compact**, for a phone: the worker's table holds its stages read, decode
/// and mesh alone (no breakdown, "submit", "wait: GPU" or "finish"), the
/// renderer's leaves out its loop's "submit" and names @ref kDrawRow "draw",
/// each table's window is stated
/// on a shorter line ("per stream frame · 30/30 timed"), the one "GPU ·
/// latency" line, the rate and a program's lines sit over the renderer's
/// table, and, in an area wider than tall, the renderer's table is beside
/// the worker's, whose lines have the width it leaves them. Live, the delay's
/// line is the worker's last.
///
/// @code
/// StatsPanel panel(/*compact=*/is_phone);
/// overlay.new_frame();
/// panel.draw(stats, shown, triangles, profiler.metrics(),
///            PlaybackStats::Clock::now(), player.live(), work_area(),
///            hooks.stats_line);
/// @endcode
class StatsPanel {
 public:
  /// How often the figures are rebuilt while it is open.
  static constexpr std::chrono::milliseconds kRefresh{250};
  /// The least time between two reads of a program's line.
  static constexpr std::chrono::milliseconds kProgramLineRefresh{1000};

  /// @param compact  Whether it is drawn compact (above), for a phone.
  explicit StatsPanel(bool compact = false) : compact_(compact) {}

  /// @brief Draw it; a tap or click on the header closes or opens it. Open
  ///        after the header, it first rebuilds the figures if @ref kRefresh
  ///        has passed since their last rebuild.
  /// @param stats      The figures.
  /// @param shown      The newest frame drawn, for @ref stats_lines.
  /// @param triangles  Its mesh's triangles.
  /// @param render     The renderer's frame (gfx::Profiler::metrics): its
  ///                   rate and its rows, the render loop's included.
  /// @param now        The time, for the rates and the rebuild.
  /// @param live       Whether they are a live player's, for
  ///                   @ref stats_lines.
  /// @param area       Where it goes (@ref work_area, or scene_area): its
  ///                   lines break against its width.
  /// @param program_line  A program's text (FrameHooks::stats_line), for
  ///                      @ref stats_lines; empty, none.
  /// @pre An ImGui frame is open on the view's context.
  void draw(const PlaybackStats& stats, const std::optional<FrameStamp>& shown,
            std::uint32_t triangles, const gfx::FrameMetrics& render,
            Playback::Clock::time_point now, bool live, const UiArea& area,
            const std::function<std::string()>& program_line = {});

  /// @return Whether it is open.
  bool open() const noexcept { return open_; }
  /// @return Whether it is drawn compact.
  bool compact() const noexcept { return compact_; }
  /// @return Its lines, as of their last rebuild.
  const std::vector<std::string>& lines() const noexcept { return lines_; }
  /// @return The worker's table's rows, as of their last rebuild: the stage
  ///         means (PlaybackStats::stage_means), the rows compact leaves
  ///         undrawn included.
  const std::vector<core::StageRow>& rows() const noexcept { return rows_; }
  /// @return What they cover: the window's frames and how many were timed,
  ///         per "stream frame".
  const gfx::ui::StageWindow& window() const noexcept { return window_; }
  /// @return The renderer's table's rows, as of their last rebuild: one
  ///         display frame's, its loop's submit included, which compact
  ///         leaves undrawn.
  const std::vector<core::StageRow>& render_rows() const noexcept {
    return render_rows_;
  }
  /// @return What they cover: one "display frame", timed or not; no frame
  ///         with no rows.
  const gfx::ui::StageWindow& render_window() const noexcept {
    return render_window_;
  }

 private:
  bool open_ = true;
  bool compact_ = false;
  std::vector<std::string> lines_;
  std::vector<core::StageRow> rows_;
  gfx::ui::StageWindow window_;
  std::string display_;
  std::vector<core::StageRow> render_rows_;
  gfx::ui::StageWindow render_window_;
  float render_width_ = 0.0f;  // compact: the renderer's column, last frame
  std::optional<Playback::Clock::time_point> rebuilt_;
  std::string program_line_;  // a program's line, as last read
  std::optional<Playback::Clock::time_point> program_line_read_;
};

}  // namespace volumetric_kit::player
