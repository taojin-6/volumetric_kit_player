// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/scenario.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <utility>

namespace volumetric_kit::player {
namespace {

namespace cfg = core::config;

// Whether `name`'s extension is .vrst, in any case.
bool is_stream(const std::string& name) {
  const std::string extension =
      std::filesystem::path(name).extension().string();
  constexpr std::string_view kStream = ".vrst";
  return std::equal(extension.begin(), extension.end(), kStream.begin(),
                    kStream.end(), [](char a, char b) {
                      return std::tolower(static_cast<unsigned char>(a)) == b;
                    });
}

// `text` less the spaces, tabs and line breaks around it.
std::string_view trimmed(std::string_view text) {
  constexpr std::string_view kSpaces = " \t\r\n";
  const std::size_t first = text.find_first_not_of(kSpaces);
  if (first == std::string_view::npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(kSpaces) - first + 1);
}

}  // namespace

core::Result<std::unique_ptr<StreamPlayer>> open_source(
    const cfg::Source& source) {
  if (source.host.empty()) {
    return StreamPlayer::open(source.path);
  }
  LiveAddress address;
  address.host = source.host;
  if (source.port != 0) {
    address.port = source.port;
  }
  return StreamPlayer::connect(std::move(address));
}

core::Result<cfg::Source> parse_live_source(std::string_view host,
                                            std::string_view port) {
  host = trimmed(host);
  port = trimmed(port);
  if (host.empty()) {
    return core::Status::invalid_argument(
        "no host: type the sender's name or IPv4 address");
  }
  if (host.find(':') != std::string_view::npos) {
    return core::Status::invalid_argument(
        "'" + std::string(host) +
        "' has a ':': type the port in the Port field");
  }
  cfg::Source source;
  source.kind = cfg::SourceKind::Stream;
  source.host = std::string(host);
  if (!port.empty()) {
    VKC_ASSIGN(const LiveAddress address,
               parse_live_address(source.host + ":" + std::string(port)));
    source.port = address.port;
  }
  return source;
}

std::optional<cfg::Source> folder_source(
    const std::string& directory, const std::vector<std::string>& names) {
  for (const std::string& name : names) {
    if (is_stream(name)) {
      cfg::Source source;
      source.kind = cfg::SourceKind::Stream;
      source.path = (std::filesystem::path(directory) / name).string();
      return source;
    }
  }
  return std::nullopt;
}

}  // namespace volumetric_kit::player
