// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file measure_log.hpp
/// @brief The measurement log: one CSV per run, of each stream frame's stage
///        rows, the renderer's newest rows as it was taken, and its device
///        counts and figures, under a header naming what was measured, which
///        vp_probe and the iOS app's measure launch write and tools/measure/
///        summarizes (DECISIONS.md).
///
/// The format, version 4. Header lines are `# key: value`: `format`, the
/// commits of `player`, `core`, `grid` and `gfx`, `build`, `device`,
/// `rings`, `stream`, `run`, `timing`, `tile_depth`, `labels`,
/// `share_vertices` and `utc`, and on a phone `thermal` and `low_power`. Then
/// one CSV header and the rows:
///
/// ```
/// pass,frame,type,bytes,triangles,table,stage,host_ms,wait_ms,gpu_ms,device,submits,host_waits,blocked_ns,staging_buffers,dispatches,barriers,deblock,exact_blocks,rounds,idle_ms,queue_ms,publish_ms,take_ms,present_ms,vertices,set
/// 2,7,P,17932,199020,worker,decode,0.4101,0.0000,4.9518,measured,,,,,,,,,,,,,,,,
/// 2,7,P,17932,199020,render,present,0.0100,1.0441,,none,,,,,,,,,,,,,,,,
/// 2,7,P,17932,199020,,,,,,,1,0,0,0,39,48,LG,1920,1,26.9100,0.4100,0.2000,8.5000,1.1000,126412,77
/// ```
///
/// - `table` `worker`: the stamp's rows, in its order, per stream frame.
///   `host_ms` is busy time (`cpu_ms - wait_ms`), `gpu_ms` is set where
///   `device` is `measured`, and `device` is `none`, `measured`, `not_timed`
///   or `unavailable` (`core::DeviceTime`). A breakdown's `stage` keeps its
///   leading "  ..". `table` `render`: the renderer's rows, alike, as the
///   tick that took the frame left them: gfx's profiler's newest resolved
///   frame, which lags the frames in flight (PlayerView::render_metrics), so
///   a display frame drawn earlier, not the one that drew this frame; none
///   while the draw is untimed.
/// - Then the frame's row, with no table and no stage: the worker's device
///   counts over it (`FrameStamp::counters`); the filter the deblock ran, or
///   `off`, and the blocks it kept exact; its rounds; its queue idle and
///   queue latency; its latency legs, due to published, published to taken
///   and taken to presented; its mesh's vertices; and its record's set
///   (`FrameStamp::set`). A figure the frame lacks is empty, as the set of a
///   record that names none.
/// - `type` is `I` or `P`; `frame` is `FrameStamp::index` and `pass`
///   `FrameStamp::pass`.
/// - `share_vertices` is `on` or `off` (PlayerViewConfig::share_vertices).
/// - A paused deblock toggle's rebuild (`FrameStamp::rebuild`) is the frame
///   before again, so no frame: it writes nothing.
///
/// Version 2 had neither `share_vertices`, `vertices` nor `set`. Version 1 had
/// neither `rings`, `tile_depth`, `labels` nor `table`, and its frame row
/// held the first four counts alone. tools/measure reads all three.

#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/base/stage_metrics.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/grid/mesh/device_mesh.hpp"
#include "volumetric_kit/player/stream_player.hpp"

namespace volumetric_kit::player {

/// @brief The commits and the build type this binary was built from, read at
///        configure time (cmake/vp_build_info.cmake).
struct BuildInfo {
  /// The player's commit, with "-dirty" for uncommitted changes to tracked
  /// files; "unknown" outside a git checkout. As each below.
  const char* player = "unknown";
  const char* core = "unknown";  ///< volumetric_kit_core's.
  const char* grid = "unknown";  ///< volumetric_kit_grid's.
  const char* gfx = "unknown";   ///< volumetric_kit_gfx's.
  /// The configuration built: CMAKE_BUILD_TYPE, or Xcode's; "none" for none.
  const char* build_type = "none";
};

/// @brief This binary's build.
///
/// @code
/// const BuildInfo& build = build_info();
/// std::printf("player %s, %s\n", build.player, build.build_type);
/// @endcode
/// @return It; static storage.
const BuildInfo& build_info() noexcept;

/// @brief What a measurement's timings were taken on: one line, for its log.
///
/// @code
/// // "NVIDIA GeForce RTX 4090; vendor 0x10de; driver NVIDIA 615.71.10;
/// //  Vulkan 1.4.325; heaps 24564 MiB local, 64265 MiB; local and mapped yes,
/// //  cached no; timestamps 64 bits, 1 ns a tick; calibrated yes, within
/// //  1300 ns"
/// const std::string line = device_line(view->compute_device());
/// @endcode
/// @param device  The worker's device (PlayerView::compute_device).
/// @return Its name, its vendor's ID, its driver's name and version (on
///         Apple, MoltenVK's), the Vulkan version it runs, its memory heaps,
///         the core's memory facts (`core::PhysicalDeviceInfo`'s
///         `device_mapped_memory` and `device_mapped_cached`), its queue's
///         timestamp bits and period, and whether `core::Device::calibrate`
///         reads both clocks, with its deviation.
std::string device_line(const core::Device& device);

/// @brief How grid's rings run on @p device, for a log's `rings` key: by
///        the core's `HostRing::in_place` rule, in place where its
///        device-mapped memory is cached
///        (`core::PhysicalDeviceInfo::device_mapped_cached`), else copied
///        through a device-only mirror.
///
/// @code
/// context.rings = rings_line(view->compute_device());  // "copied"
/// @endcode
/// @param device  The worker's device.
/// @return "in place" or "copied".
std::string rings_line(const core::Device& device);

/// @brief A time as UTC, to the second.
/// @param at       The time.
/// @param compact  Whether for a file's name: "20261023T051200Z" rather than
///                 "2026-10-23T05:12:00Z".
/// @return It, ISO 8601.
std::string utc_text(std::chrono::system_clock::time_point at,
                     bool compact = false);

/// @brief A log's file name, as the iOS app names it in Documents/measure/.
///
/// @code
/// // "20261023T051200Z-1a2b3c4.csv"
/// const std::string name = measure_file_name(started);
/// @endcode
/// @param started    When the measurement started.
/// @param extension  ".csv" for a log; ".failed" for the file a measure
///                   launch that logs nothing writes its reason into.
/// @return "<utc>-<player commit><extension>": @ref utc_text compact, and the
///         first 7 characters of build_info's player commit, "-dirty" kept.
std::string measure_file_name(std::chrono::system_clock::time_point started,
                              std::string_view extension = ".csv");

/// @brief What a log's header states beside the build.
struct MeasureContext {
  std::string device;  ///< @ref device_line.
  std::string rings;   ///< @ref rings_line.
  /// What was played: a file's name and track ("rafa2.vrst, track 0"), or a
  /// sender's address.
  std::string stream;
  /// How the frames were driven, as the writer's options: through the view
  /// or by the worker alone, and the passes or frames asked for
  /// ("vp_probe --worker --passes 2", "ios --passes 2").
  std::string run;
  /// The worker's device timing.
  DeviceTiming timing = DeviceTiming::kOff;
  /// The worker's tile depth (StreamPlayer::set_tile_depth).
  std::uint32_t tile_depth = 0;
  /// Whether debug labels were asked for (PlayerViewConfig::debug_labels).
  bool debug_labels = true;
  /// Whether triangles shared their vertices
  /// (PlayerViewConfig::share_vertices).
  bool share_vertices = true;
  /// When it started, stated as UTC.
  std::chrono::system_clock::time_point started{};
  /// A phone's thermal state as it started (`ProcessInfo.thermalState`):
  /// "nominal", "fair", "serious" or "critical"; empty, and no header key,
  /// elsewhere.
  std::string thermal;
  /// Whether a phone's Low Power Mode was on as it started; none, and no
  /// header key, elsewhere.
  std::optional<bool> low_power;
};

/// @brief Writes one measurement's log (the format above). Each frame is
///        written as it comes, into `<path>.part`, which @ref finish renames
///        to `<path>`, so a file at `path` is a whole run.
///
/// @code
/// VKC_ASSIGN(std::unique_ptr<MeasureLog> log,
///            MeasureLog::open("rafa2-all.csv", context));
/// if (view->frames_taken() != seen) {
///   seen = view->frames_taken();
///   VKC_TRY(log->add(*view->shown(), view->mesh()));
/// }
/// VKC_TRY(log->finish());
/// @endcode
class MeasureLog {
 public:
  /// @brief Create `<path>.part` and write the header.
  /// @param path     Where the finished log goes; its folder must exist.
  /// @param context  What the header states beside build_info.
  /// @return The log; `core::Status::Code::IoError` naming the file when it
  ///         cannot be written.
  static core::Result<std::unique_ptr<MeasureLog>> open(
      const std::string& path, const MeasureContext& context);

  /// @brief Leaves an unfinished log as `<path>.part`.
  ~MeasureLog() = default;
  MeasureLog(const MeasureLog&) = delete;
  MeasureLog& operator=(const MeasureLog&) = delete;
  MeasureLog(MeasureLog&&) = delete;
  MeasureLog& operator=(MeasureLog&&) = delete;

  /// @brief Write a frame taken for drawing: its worker rows, the render rows
  ///        as of the tick that took it, then its frame row; a rebuild
  ///        (`FrameStamp::rebuild`) writes nothing and counts in no
  ///        @ref frames.
  /// @param stamp   Its stamp, taken, and presented where there is a present.
  /// @param mesh    Its mesh, for its triangles and vertices
  ///                (PlayerView::mesh, or the exchange's live mesh).
  /// @param render  PlayerView::render_metrics().sections; empty without a
  ///                view or while the draw is untimed.
  /// @return OK; `core::Status::Code::IoError` once a write fails, or after
  ///         @ref finish.
  core::Status add(const FrameStamp& stamp, const grid::mesh::DeviceMesh& mesh,
                   const std::vector<core::StageRow>& render = {});

  /// @brief Flush, close, and rename `<path>.part` to `<path>`.
  /// @return OK; `core::Status::Code::IoError` for a write or the rename
  ///         failing, or a second call.
  core::Status finish();

  /// @return The finished log's path.
  const std::string& path() const noexcept { return path_; }
  /// @return The frames written.
  std::uint64_t frames() const noexcept { return frames_; }

 private:
  explicit MeasureLog(std::string path) : path_(std::move(path)) {}

  std::string path_;
  std::ofstream out_;  // into path_ + ".part"; closed once finished
  std::uint64_t frames_ = 0;
  bool finished_ = false;
};

}  // namespace volumetric_kit::player
