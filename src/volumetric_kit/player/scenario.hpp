// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file scenario.hpp
/// @brief What a Player scenario (the core's CONFIG.md) plays: the player of
///        its source, and the sources the iOS app chooses from, a sender's
///        typed in its dialog or the first stream in its Documents folder.

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/config/config.hpp"
#include "volumetric_kit/player/stream_player.hpp"

namespace volumetric_kit::player {

/// @brief The player of a Player scenario's source, on the host alone.
///
/// @code
/// VKC_ASSIGN(const core::config::Config settings,
///            core::config::load(path, core::config::Application::Player));
/// VKC_ASSIGN(std::unique_ptr<StreamPlayer> player,
///            open_source(settings.source, AudioOut::open_default));
/// @endcode
/// @param source      A source the core loaded for `Application::Player`, or
///                    one of @ref parse_live_source's or @ref folder_source's.
/// @param make_audio  The audio output (StreamPlayer::open and
///                    StreamPlayer::connect): the apps give
///                    AudioOut::open_default; empty for the null output.
/// @return With no `host`, StreamPlayer::open of its `path`; else
///         StreamPlayer::connect, which does no I/O, to `host` on `port`, or
///         for 0 on LiveAddress's default, the sender's; or their refusal.
core::Result<std::unique_ptr<StreamPlayer>> open_source(
    const core::config::Source& source, AudioOutMaker make_audio = {});

/// @brief Parse a sender's source from a host and a port typed apart, as the
///        iOS app's dialog takes them, on the host alone. Each is trimmed of
///        surrounding spaces; the port's rule is @ref parse_live_address's.
///
/// @code
/// VKC_ASSIGN(const core::config::Source source,
///            parse_live_source(host_field, port_field));
/// VKC_ASSIGN(std::unique_ptr<StreamPlayer> player, open_source(source));
/// @endcode
/// @param host  A name or a numeric IPv4 address.
/// @param port  A number from 1 to 65535, or empty for the sender's default.
/// @return A stream source of @p host on @p port, its port 0 for an empty
///         one, which @ref open_source takes as LiveAddress's default;
///         `core::Status::Code::InvalidArgument`, a message for the person
///         typing, for an empty host, a host with a ':' (the port has its own
///         field), or a port that is not a number from 1 to 65535.
core::Result<core::config::Source> parse_live_source(std::string_view host,
                                                     std::string_view port);

/// @brief The first stream a folder holds, as the iOS app looks in its
///        Documents.
///
/// @code
/// const std::optional<core::config::Source> source =
///     folder_source(documents, names);
/// if (source) {
///   VKC_ASSIGN(std::unique_ptr<StreamPlayer> player, open_source(*source));
/// }
/// @endcode
/// @param directory  The folder.
/// @param names      Its entries' names, in the order its first `.vrst` is
///                   taken by: the iOS app's is Finder's.
/// @return A stream source of the first name in @p names whose extension is
///         `.vrst`, in any case, in @p directory; `std::nullopt` with none.
std::optional<core::config::Source> folder_source(
    const std::string& directory, const std::vector<std::string>& names);

}  // namespace volumetric_kit::player
