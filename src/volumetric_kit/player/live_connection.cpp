// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/live_connection.hpp"

#include <utility>

#include "volumetric_kit/core/net/address.hpp"

namespace volumetric_kit::player {

core::Result<LiveAddress> parse_live_address(std::string_view text) {
  VKC_ASSIGN(core::net::HostPort at,
             core::net::parse_host_port(text, grid::networking::kDefaultPort));
  return LiveAddress{std::move(at.host), at.port};
}

std::string to_string(const LiveAddress& address) {
  return address.host + ":" + std::to_string(address.port);
}

std::string live_reason(const core::Status& ended, const std::string& address) {
  std::string_view why = ended.message();
  for (const std::string& prefix : {"StreamClient: " + address + ": ",
                                    std::string("StreamClient::connect: ")}) {
    if (why.substr(0, prefix.size()) == prefix) {
      why.remove_prefix(prefix.size());
      break;
    }
  }
  return std::string(why);
}

LiveConnection::LiveConnection(LiveAddress address,
                               std::chrono::milliseconds retry)
    : address_(std::move(address)), retry_(retry) {}

grid::networking::StreamClient* LiveConnection::poll() {
  if (client_ != nullptr) {
    return client_.get();
  }
  if (std::chrono::steady_clock::now() < retry_at_) {
    return nullptr;
  }
  core::Result<std::unique_ptr<grid::networking::StreamClient>> client =
      grid::networking::StreamClient::connect(address_.host, address_.port);
  if (!client.ok()) {
    fail(client.status());
    return nullptr;
  }
  client_ = std::move(client).value();
  return client_.get();
}

void LiveConnection::drop(const core::Status& why) {
  const bool connected = client_ != nullptr;
  client_.reset();  // closed at once
  if (!why.ok()) {
    fail(why);
    return;
  }
  retry_at_ = {};
  if (connected) {
    last_error_.clear();
  }
}

void LiveConnection::fail(const core::Status& why) {
  last_error_ = live_reason(why, to_string(address_));
  retry_at_ = std::chrono::steady_clock::now() + retry_;
}

}  // namespace volumetric_kit::player
