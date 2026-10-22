// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file scenario.hpp
/// @brief What a Player scenario (the core's CONFIG.md) plays: the player of
///        its source, and the source the iOS app finds in its Documents
///        folder.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/config/config.hpp"
#include "volumetric_kit/player/stream_player.hpp"

namespace volumetric_kit::player {

/// @brief The scenario @ref folder_source looks for first.
inline constexpr const char* kFolderScenario = "player.json";

/// @brief The player of a Player scenario's source, on the host alone.
///
/// @code
/// VKC_ASSIGN(const core::config::Config settings,
///            core::config::load(path, core::config::Application::Player));
/// VKC_ASSIGN(std::unique_ptr<StreamPlayer> player,
///            open_source(settings.source));
/// @endcode
/// @param source  A source the core loaded for `Application::Player`.
/// @return With no `host`, StreamPlayer::open of its `path`; else
///         StreamPlayer::connect, which does no I/O, to `host` on `port`, or
///         for 0 on LiveAddress's default, the sender's; or their refusal.
core::Result<std::unique_ptr<StreamPlayer>> open_source(
    const core::config::Source& source);

/// @brief The source a folder holds, as the iOS app looks in its Documents:
///        @ref kFolderScenario, a Player scenario, if the folder has one;
///        else its first `.vrst`.
///
/// @code
/// VKC_ASSIGN(const std::optional<core::config::Source> source,
///            folder_source(documents, names));
/// if (source) {
///   VKC_ASSIGN(std::unique_ptr<StreamPlayer> player, open_source(*source));
/// }
/// @endcode
/// @param directory  The folder.
/// @param names      Its entries' names, in the order its first `.vrst` is
///                   taken by: the iOS app's is Finder's.
/// @return The scenario's source, a relative path resolved against
///         @p directory as `core::config::load` resolves it; else a stream
///         source of the first name in @p names whose extension is `.vrst`,
///         in any case; `std::nullopt` with neither; or
///         `core::config::load`'s refusal of the scenario, never passed over
///         for a `.vrst`.
core::Result<std::optional<core::config::Source>> folder_source(
    const std::string& directory, const std::vector<std::string>& names);

}  // namespace volumetric_kit::player
