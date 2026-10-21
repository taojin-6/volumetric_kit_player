// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/command_line.hpp"

#include <charconv>
#include <system_error>

namespace volumetric_kit::player {

std::optional<std::uint64_t> parse_count(std::string_view text) {
  std::uint64_t count = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), count);
  if (error != std::errc() || end != text.data() + text.size() || count == 0) {
    return std::nullopt;
  }
  return count;
}

}  // namespace volumetric_kit::player
