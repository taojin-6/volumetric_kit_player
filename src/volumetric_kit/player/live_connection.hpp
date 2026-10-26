// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file live_connection.hpp
/// @brief A live stream's address, and the connection to its sender, made
///        again after it ends.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/recon/networking/stream_client.hpp"
#include "volumetric_kit/recon/networking/stream_server.hpp"

namespace volumetric_kit::player {

/// @brief A live stream's address: a sender's
///        (recon::networking::StreamServer).
struct LiveAddress {
  std::string host;  ///< A name or a numeric IPv4 address.
  std::uint16_t port = recon::networking::kDefaultPort;  ///< 1 to 65535.
};

/// @brief Parse a live stream's address, on the host alone.
///
/// @code
/// VKC_ASSIGN(const LiveAddress address, parse_live_address("lab:47801"));
/// @endcode
/// @param text  "host" or "host:port".
/// @return The address, on `recon::networking::kDefaultPort` when it names
///         none; `core::Status::Code::InvalidArgument` naming what is wrong
///         for an empty host or a port that is not a number from 1 to 65535.
core::Result<LiveAddress> parse_live_address(std::string_view text);

/// @return @p address as "host:port".
std::string to_string(const LiveAddress& address);

/// @brief Why a live connection ended, for its status line, which names the
///        address already.
///
/// @code
/// // "cannot connect: Connection refused"
/// const std::string why = live_reason(
///     core::Status::io_error(
///         "StreamClient: lab:47801: cannot connect: Connection refused"),
///     "lab:47801");
/// @endcode
/// @param ended    What ended it: recon's `networking::StreamClient` error, or
///                 the player's own.
/// @param address  to_string() of the address the client was given.
/// @return @p ended's message less the "StreamClient: <address>: " or
///         "StreamClient::connect: " it starts with (StreamClient's message
///         convention); whole with neither, as a bare "out of memory".
std::string live_reason(const core::Status& ended, const std::string& address);

/// The wait before a connection that ended with an error, or an attempt
/// that failed, is made again.
inline constexpr std::chrono::milliseconds kLiveRetry{1000};

/// @brief A sender's connection (recon::networking::StreamClient), made
///        again after it ends: at the next @ref poll after a clean end, and
///        the retry wait later after an error.
///
/// A new client never blocks (recon's `StreamClient::connect`): its first
/// `next` looks the name up and connects, so a refused connect is that
/// call's error, which the caller passes to @ref drop.
///
/// @code
/// LiveConnection connection(address);
/// while (running) {
///   recon::networking::StreamClient* const client = connection.poll();
///   if (client == nullptr) {  // waiting to retry, or the attempt failed
///     show(connection.last_error());
///     std::this_thread::sleep_until(connection.retry_at());
///     continue;
///   }
///   core::Status ended;  // what ended the connection; OK for a stop
///   read_until_it_ends(*client, &ended);
///   connection.drop(ended);
/// }
/// @endcode
///
/// @warning Not thread-safe: one thread uses it and its client.
class LiveConnection {
 public:
  /// @param address  The sender's. An empty host or port 0 fails every
  ///                 attempt, as `StreamClient::connect` refuses it.
  /// @param retry    The wait after an error (tests shorten it).
  explicit LiveConnection(LiveAddress address,
                          std::chrono::milliseconds retry = kLiveRetry);

  /// @return The client held. With none, once the retry wait has passed, a
  ///         new one; null while the wait lasts, or when
  ///         `StreamClient::connect` refused the attempt, which then waits
  ///         the retry and keeps why (@ref last_error).
  recon::networking::StreamClient* poll();

  /// @brief Close the connection now, if there is one.
  /// @param why  OK for a clean end (a park, a stop): the next @ref poll
  ///             connects, and @ref last_error empties; with no connection,
  ///             as for a park during the retry wait, it ends the wait alone
  ///             and keeps why the last ended. An error: the next @ref poll
  ///             waits the retry first, and @ref last_error holds
  ///             live_reason(@p why).
  void drop(const core::Status& why = {});

  /// @return When @ref poll connects next: at or before now unless waiting.
  std::chrono::steady_clock::time_point retry_at() const noexcept {
    return retry_at_;
  }
  /// @return Why the last connection ended or attempt failed, as
  ///         live_reason gives it; empty before either, and after a clean
  ///         end.
  const std::string& last_error() const noexcept { return last_error_; }
  /// @return The sender's address.
  const LiveAddress& address() const noexcept { return address_; }

 private:
  // Keep why, and wait the retry from now.
  void fail(const core::Status& why);

  LiveAddress address_;
  std::chrono::milliseconds retry_;
  std::unique_ptr<recon::networking::StreamClient> client_;  // null between
  std::chrono::steady_clock::time_point retry_at_{};  // the epoch: no wait
  std::string last_error_;
};

}  // namespace volumetric_kit::player
