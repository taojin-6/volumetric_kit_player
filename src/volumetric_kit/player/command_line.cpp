// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/command_line.hpp"

#include <charconv>
#include <system_error>
#include <utility>

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

std::optional<DeviceTiming> parse_device_timing(std::string_view text) {
  for (const DeviceTiming timing :
       {DeviceTiming::kOff, DeviceTiming::kSampled, DeviceTiming::kAll}) {
    if (text == to_string(timing)) {
      return timing;
    }
  }
  return std::nullopt;
}

core::Result<std::optional<MeasureArgs>> parse_measure_args(
    const std::vector<std::string>& args) {
  MeasureArgs measure;
  bool named = false;  // --measure came
  bool other = false;  // --passes, --frames or --timing came
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg != "--measure" && arg != "--passes" && arg != "--frames" &&
        arg != "--timing") {
      continue;  // the system's
    }
    if (i + 1 == args.size()) {
      return core::Status::invalid_argument(arg + " needs a value");
    }
    const std::string& value = args[++i];
    if (arg == "--measure") {
      if (value.empty() || value.find('/') != std::string::npos) {
        return core::Status::invalid_argument("--measure '" + value +
                                              "': a file's name in Documents");
      }
      measure.stream = value;
      named = true;
    } else if (arg == "--passes") {
      const std::optional<std::uint64_t> passes = parse_count(value);
      if (!passes) {
        return core::Status::invalid_argument("--passes '" + value +
                                              "': a count from 1");
      }
      measure.passes = *passes;
      other = true;
    } else if (arg == "--frames") {
      measure.frames = parse_count(value);
      if (!measure.frames) {
        return core::Status::invalid_argument("--frames '" + value +
                                              "': a count from 1");
      }
      other = true;
    } else {
      const std::optional<DeviceTiming> timing = parse_device_timing(value);
      if (!timing) {
        return core::Status::invalid_argument("--timing '" + value +
                                              "': off, sampled or all");
      }
      measure.timing = *timing;
      other = true;
    }
  }
  if (!named) {
    if (other) {
      return core::Status::invalid_argument(
          "--passes, --frames and --timing need --measure");
    }
    return std::optional<MeasureArgs>();
  }
  return std::optional<MeasureArgs>(std::move(measure));
}

}  // namespace volumetric_kit::player
