// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file command_line.hpp
/// @brief What the player's programs (the desktop app, vp_live_probe) and
///        its addresses share in parsing their text. Player-internal.

#include <cstdint>
#include <optional>
#include <string_view>

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

}  // namespace volumetric_kit::player
