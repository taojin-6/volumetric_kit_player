// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/command_line.hpp"

#include <charconv>
#include <system_error>
#include <utility>

#include "volumetric_kit/core/base/exception_status.hpp"

namespace volumetric_kit::player {

namespace {

constexpr std::uint64_t kMaxSide = 16384;  // parse_size's, pixels

}  // namespace

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

std::optional<std::uint32_t> parse_tile_depth(std::string_view text) {
  if (text == "0") {
    return 0;
  }
  if (text == "1") {
    return 1;
  }
  return std::nullopt;
}

std::optional<bool> parse_switch(std::string_view text) {
  if (text == "on") {
    return true;
  }
  if (text == "off") {
    return false;
  }
  return std::nullopt;
}

std::optional<VkExtent2D> parse_size(std::string_view text) {
  const std::size_t x = text.find('x');
  if (x == std::string_view::npos) {
    return std::nullopt;
  }
  const std::optional<std::uint64_t> width = parse_count(text.substr(0, x));
  const std::optional<std::uint64_t> height = parse_count(text.substr(x + 1));
  if (!width || !height || *width > kMaxSide || *height > kMaxSide) {
    return std::nullopt;
  }
  return VkExtent2D{static_cast<std::uint32_t>(*width),
                    static_cast<std::uint32_t>(*height)};
}

std::optional<glm::vec3> parse_up_axis(std::string_view text) {
  float sign = 1.0f;
  if (!text.empty() && (text[0] == '+' || text[0] == '-')) {
    sign = text[0] == '-' ? -1.0f : 1.0f;
    text.remove_prefix(1);
  }
  if (text.size() != 1 || text[0] < 'x' || text[0] > 'z') {
    return std::nullopt;
  }
  glm::vec3 up(0.0f);
  up[text[0] - 'x'] = sign;
  return up;
}

core::Result<DesktopArgs> parse_desktop_args(int argc,
                                             const char* const* argv) try {
  DesktopArgs args;
  std::vector<const char*> rest;  // the core's
  for (int i = 0; i < argc && argv != nullptr; ++i) {
    if (i == 0 || argv[i] == nullptr ||
        std::string_view(argv[i]) != "--up-axis") {
      rest.push_back(argv[i]);
      continue;
    }
    if (args.up) {
      return core::Status::invalid_argument("--up-axis was supplied twice");
    }
    if (i + 1 == argc || argv[i + 1] == nullptr) {
      return core::Status::invalid_argument("--up-axis requires an axis");
    }
    const std::string_view axis = argv[++i];
    args.up = parse_up_axis(axis);
    if (!args.up) {
      return core::Status::invalid_argument(
          "--up-axis '" + std::string(axis) +
          "': expected x, y, z, -x, -y or -z");
    }
  }
  VKC_ASSIGN(
      static_cast<core::config::Arguments&>(args),
      core::config::parse_args(static_cast<int>(rest.size()), rest.data()));
  return args;
} catch (...) {
  return core::exception_status("parse_desktop_args",
                                core::Status::Code::InvalidArgument);
}

core::Result<std::optional<MeasureArgs>> parse_measure_args(
    const std::vector<std::string>& args) {
  MeasureArgs measure;
  bool named = false;  // --measure came
  bool other = false;  // another of its options came
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg != "--measure" && arg != "--passes" && arg != "--frames" &&
        arg != "--timing" && arg != "--tile-depth" && arg != "--labels" &&
        arg != "--share-vertices" && arg != "--panel") {
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
    } else if (arg == "--timing") {
      const std::optional<DeviceTiming> timing = parse_device_timing(value);
      if (!timing) {
        return core::Status::invalid_argument("--timing '" + value +
                                              "': off, sampled or all");
      }
      measure.timing = *timing;
      other = true;
    } else if (arg == "--tile-depth") {
      const std::optional<std::uint32_t> depth = parse_tile_depth(value);
      if (!depth) {
        return core::Status::invalid_argument("--tile-depth '" + value +
                                              "': 0 or 1");
      }
      measure.tile_depth = *depth;
      other = true;
    } else {
      // --labels, --share-vertices or --panel: a switch.
      const std::optional<bool> on = parse_switch(value);
      if (!on) {
        return core::Status::invalid_argument(arg + " '" + value +
                                              "': on or off");
      }
      bool& to = arg == "--labels"           ? measure.debug_labels
                 : arg == "--share-vertices" ? measure.share_vertices
                                             : measure.panel;
      to = *on;
      other = true;
    }
  }
  if (!named) {
    if (other) {
      return core::Status::invalid_argument(
          "--passes, --frames, --timing, --tile-depth, --labels, "
          "--share-vertices and --panel need --measure");
    }
    return std::optional<MeasureArgs>();
  }
  return std::optional<MeasureArgs>(std::move(measure));
}

}  // namespace volumetric_kit::player
