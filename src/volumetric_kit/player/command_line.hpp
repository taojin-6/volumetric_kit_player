// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file command_line.hpp
/// @brief What vp_probe's command line, the iOS app's measure launch and the
///        player's addresses share in parsing their text. Player-internal.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/player/stream_player.hpp"

namespace volumetric_kit::player {

/// @brief Parse a count from 1, such as `--frames`' N or a port.
///
/// @code
/// const std::optional<std::uint64_t> frames = parse_count(argv[++i]);
/// if (!frames) return std::nullopt;  // the usage
/// @endcode
/// @param text  Decimal digits alone.
/// @return The count; none for 0, an empty text, a sign, any other character
///         or a count past 2^64 - 1.
std::optional<std::uint64_t> parse_count(std::string_view text);

/// @brief Parse `--timing`'s value.
///
/// @code
/// const std::optional<DeviceTiming> timing = parse_device_timing("sampled");
/// @endcode
/// @param text  "off", "sampled" or "all" (to_string's names).
/// @return The timing; none for any other text.
std::optional<DeviceTiming> parse_device_timing(std::string_view text);

/// @brief A measurement the iOS app's launch arguments ask for (README.md,
///        tools/measure/).
struct MeasureArgs {
  /// The `.vrst` to play, by its name in the app's Documents folder.
  std::string stream;
  /// The passes of its track to log, as vp_probe's `--passes`.
  std::uint64_t passes = 1;
  /// The frames to log at most; empty, every frame of those passes.
  std::optional<std::uint64_t> frames;
  /// The worker's device timing.
  DeviceTiming timing = DeviceTiming::kAll;
};

/// @brief Parse the iOS app's launch arguments: `--measure <name>`, with
///        `--passes P`, `--frames N` and `--timing off|sampled|all`, in any
///        order, on the host alone. Arguments it does not name are left to
///        the system, which adds its own.
///
/// @code
/// VKC_ASSIGN(const std::optional<MeasureArgs> measure,
///            parse_measure_args({"--measure", "rafa2.vrst", "--passes",
///                                "2"}));
/// @endcode
/// @param args  The arguments after the program's name.
/// @return None without `--measure`; the measurement;
///         `core::Status::Code::InvalidArgument` naming the argument for a
///         missing or bad value, a name that is no file's in a folder (empty,
///         or holding a '/'), or `--passes`, `--frames` or `--timing` with no
///         `--measure`.
core::Result<std::optional<MeasureArgs>> parse_measure_args(
    const std::vector<std::string>& args);

}  // namespace volumetric_kit::player
