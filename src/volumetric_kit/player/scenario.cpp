// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/scenario.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string_view>
#include <utility>

#include "volumetric_kit/core/base/exception_status.hpp"

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

core::Result<std::optional<cfg::Source>> folder_source(
    const std::string& directory, const std::vector<std::string>& names) {
  try {
    const std::filesystem::path folder(directory);
    if (std::find(names.begin(), names.end(), kFolderScenario) != names.end()) {
      VKC_ASSIGN(cfg::Config scenario,
                 cfg::load((folder / kFolderScenario).string(),
                           cfg::Application::Player));
      return std::move(scenario.source);
    }
    for (const std::string& name : names) {
      if (is_stream(name)) {
        cfg::Source source;
        source.kind = cfg::SourceKind::Stream;
        source.path = (folder / name).string();
        return source;
      }
    }
    return std::nullopt;
  } catch (...) {
    return core::exception_status("folder_source", core::Status::Code::IoError);
  }
}

}  // namespace volumetric_kit::player
