// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file measure_log.hpp
/// @brief The measurement log: one CSV per run, of each stream frame's stage
///        rows and device counts under a header naming what was measured,
///        which vp_probe and the iOS app's measure launch write and
///        tools/measure/ summarizes (DECISIONS.md).
///
/// The format, version 1. Header lines are `# key: value`: `format`, the
/// commits of `player`, `core`, `recon` and `gfx`, `build`, `device`,
/// `stream`, `run`, `timing` and `utc`. Then one CSV header and the rows:
///
/// ```
/// pass,frame,type,bytes,triangles,stage,host_ms,wait_ms,gpu_ms,device,submits,host_waits,blocked_ns,staging_buffers
/// 1,0,I,17932,199020,decode,3.2101,1.0442,0.9518,measured,,,,
/// 1,0,I,17932,199020,,,,,,5,5,1288731,5
/// ```
///
/// - One row per frame and stage, in the stamp's order: `host_ms` is busy
///   time (`cpu_ms - wait_ms`), `gpu_ms` is set where `device` is
///   `measured`, and `device` is `none`, `measured`, `not_timed` or
///   `unavailable` (`core::DeviceTime`). A breakdown's `stage` keeps its
///   leading "  ..".
/// - Then one row per frame with no `stage`: the worker's device counts over
///   it (`FrameStamp::counters`).
/// - `type` is `I` or `P`; `frame` is `FrameStamp::index` and `pass`
///   `FrameStamp::pass`.
/// - A paused deblock toggle's rebuild (`FrameStamp::rebuild`) is the frame
///   before again, so no frame: it writes nothing.

#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/player/stream_player.hpp"

namespace volumetric_kit::player {

/// @brief The commits and the build type this binary was built from, read at
///        configure time (cmake/vp_build_info.cmake).
struct BuildInfo {
  /// The player's commit, with "-dirty" for uncommitted changes to tracked
  /// files; "unknown" outside a git checkout. As each below.
  const char* player = "unknown";
  const char* core = "unknown";   ///< volumetric_kit_core's.
  const char* recon = "unknown";  ///< volumetric_kit_recon's.
  const char* gfx = "unknown";    ///< volumetric_kit_gfx's.
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
///         whether a memory type is device-local and host-mapped and whether
///         one such is cached, its queue's timestamp bits and period, and
///         whether `core::Device::calibrate` reads both clocks, with its
///         deviation.
std::string device_line(const core::Device& device);

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
  /// What was played: a file's name and track ("rafa2.vrst, track 0"), or a
  /// sender's address.
  std::string stream;
  /// How the frames were driven, as the writer's options: through the view
  /// or by the worker alone, and the passes or frames asked for
  /// ("vp_probe --worker --passes 2", "ios --passes 2").
  std::string run;
  /// The worker's device timing.
  DeviceTiming timing = DeviceTiming::kOff;
  /// When it started, stated as UTC.
  std::chrono::system_clock::time_point started{};
  // TODO: the deblock (StreamPlayer::deblock), which format 1 has no header
  // key or column for.
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
///   VKC_TRY(log->add(*view->shown(), view->mesh().triangle_count));
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

  /// @brief Write a frame taken for drawing: its stage rows, then its counts;
  ///        a rebuild (`FrameStamp::rebuild`) writes nothing and counts in
  ///        no @ref frames.
  /// @param stamp      Its stamp.
  /// @param triangles  Its mesh's triangles.
  /// @return OK; `core::Status::Code::IoError` once a write fails, or after
  ///         @ref finish.
  core::Status add(const FrameStamp& stamp, std::uint32_t triangles);

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
