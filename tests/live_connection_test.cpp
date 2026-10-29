// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// LiveConnection on the host, against a grid StreamServer on 127.0.0.1
// (never 0.0.0.0, which could raise a firewall prompt) and a port nothing
// listens on, with a short retry: poll keeps one client while connected,
// which reads the server's records; a refused connect, dropped with its
// error, keeps why, and the next attempt comes no sooner than the retry; an
// attempt StreamClient::connect refuses is null with why, and waits the
// retry, the default kLiveRetry too; drop(error) waits the retry before the
// next client, and drop() during that wait ends it, keeping why; drop() of a
// connection empties why and connects at the next poll; a server that stops
// ends the client's next, and drop with that status shows why.

#include "volumetric_kit/player/live_connection.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/grid/networking/stream_client.hpp"
#include "volumetric_kit/grid/networking/stream_server.hpp"
#include "volumetric_kit/grid/stream/session.hpp"
#include "volumetric_kit/grid/stream/stream_reader.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace networking = volumetric_kit::grid::networking;
namespace stream = volumetric_kit::grid::stream;
using namespace std::chrono_literals;
using volumetric_kit::player::kLiveRetry;
using volumetric_kit::player::LiveConnection;
using volumetric_kit::player::to_string;
using SteadyClock = std::chrono::steady_clock;

// The tests' retry: short, and still long enough that a poll just after a
// drop finds the wait on, under TSan too.
constexpr std::chrono::milliseconds kRetry = 300ms;
// The longest a connection may take to reach a state the test waits for.
constexpr auto kArrival = 10s;

// A server on 127.0.0.1, at a free port, of a session of one geometry track;
// null with a failure recorded.
std::unique_ptr<networking::StreamServer> serve() {
  stream::Session session;
  session.writer = "live_connection_test";
  stream::Track track;
  track.kind = stream::TrackKind::Geometry;
  session.tracks.push_back(track);
  networking::ServerOptions options;
  options.host = "127.0.0.1";
  options.port = 0;
  core::Result<std::unique_ptr<networking::StreamServer>> server =
      networking::StreamServer::start(session, options);
  EXPECT_TRUE(server.ok()) << server.status().message();
  return server.ok() ? std::move(server).value() : nullptr;
}

// A port on 127.0.0.1 that a server had a moment ago, now closed, so a
// connect to it is refused; 0 with a failure recorded.
std::uint16_t refusing_port() {
  const std::unique_ptr<networking::StreamServer> server = serve();
  return server != nullptr ? server->port() : 0;
}

// `client` read until `done` holds, true then, or until its connection ends,
// into `*ended`, or kArrival passes (a failure recorded): false.
template <typename Done>
bool read_until(networking::StreamClient& client, Done&& done,
                core::Status* ended = nullptr) {
  const SteadyClock::time_point deadline = SteadyClock::now() + kArrival;
  stream::Record record;
  while (SteadyClock::now() < deadline) {
    const core::Result<bool> got = client.next(10ms, &record, nullptr);
    if (!got.ok()) {
      if (ended != nullptr) {
        *ended = got.status();
      }
      return false;
    }
    if (done(*got, record)) {
      return true;
    }
  }
  ADD_FAILURE() << "nothing within " << kArrival.count() << " s";
  return false;
}

// `client` read until its session has come.
bool has_session(networking::StreamClient& client) {
  core::Status ended;
  const bool got = read_until(
      client,
      [&client](bool, const stream::Record&) {
        return client.session() != nullptr;
      },
      &ended);
  EXPECT_TRUE(ended.ok()) << ended.message();
  return got;
}

// `client` read until its connection ends: why.
core::Status end_of(networking::StreamClient& client) {
  core::Status ended;
  read_until(client, [](bool, const stream::Record&) { return false; }, &ended);
  return ended;
}

// `connection` polled until it gives a client, within kArrival: the client,
// null with a failure recorded.
networking::StreamClient* next_client(LiveConnection& connection) {
  const SteadyClock::time_point deadline = SteadyClock::now() + kArrival;
  while (SteadyClock::now() < deadline) {
    if (networking::StreamClient* const client = connection.poll()) {
      return client;
    }
    std::this_thread::sleep_for(1ms);
  }
  ADD_FAILURE() << "no client within " << kArrival.count() << " s";
  return nullptr;
}

TEST(LiveConnection, PollKeepsOneClientWhileConnected) {
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  LiveConnection connection({"127.0.0.1", server->port()}, kRetry);
  EXPECT_EQ(connection.address().host, "127.0.0.1");
  EXPECT_EQ(connection.address().port, server->port());
  EXPECT_LE(connection.retry_at(), SteadyClock::now());
  EXPECT_TRUE(connection.last_error().empty());
  networking::StreamClient* const client = connection.poll();
  ASSERT_NE(client, nullptr);
  ASSERT_TRUE(has_session(*client));
  EXPECT_EQ(connection.poll(), client);
  // The one connection: a record published now reaches it.
  stream::RecordInfo info;
  info.key = true;
  info.seq = 7;
  const std::uint8_t byte = 0;
  const core::Status published = server->publish(info, &byte, 1);
  ASSERT_TRUE(published.ok()) << published.message();
  EXPECT_TRUE(read_until(*client, [](bool got, const stream::Record& record) {
    return got && record.info.seq == 7;
  }));
  EXPECT_EQ(connection.poll(), client);
  EXPECT_TRUE(connection.last_error().empty());
}

TEST(LiveConnection, ARefusedConnectWaitsTheRetryBeforeTheNext) {
  const std::uint16_t port = refusing_port();
  ASSERT_NE(port, 0);
  LiveConnection connection({"127.0.0.1", port}, kRetry);
  // StreamClient::connect never blocks: the client's next connects.
  networking::StreamClient* const client = connection.poll();
  ASSERT_NE(client, nullptr);
  const core::Status refused = end_of(*client);
  ASSERT_FALSE(refused.ok());
  const SteadyClock::time_point dropped = SteadyClock::now();
  connection.drop(refused);
  EXPECT_EQ(connection.last_error(), "cannot connect: Connection refused");
  const SteadyClock::time_point retry_at = connection.retry_at();
  EXPECT_GE(retry_at, dropped + kRetry);
  EXPECT_EQ(connection.poll(), nullptr);
  // The next attempt, once the retry is due, keeps why the last failed.
  ASSERT_NE(next_client(connection), nullptr);
  EXPECT_GE(SteadyClock::now(), retry_at);
  EXPECT_EQ(connection.last_error(), "cannot connect: Connection refused");
}

// One StreamClient::connect refuses: an empty host.
TEST(LiveConnection, AFailedAttemptIsNullWithWhyAndWaitsTheRetry) {
  LiveConnection connection({"", 47801}, kRetry);
  const SteadyClock::time_point first = SteadyClock::now();
  EXPECT_EQ(connection.poll(), nullptr);
  EXPECT_EQ(connection.last_error(), "an empty host");
  EXPECT_GE(connection.retry_at(), first + kRetry);
  // Waiting: no attempt, and the wait stands.
  const SteadyClock::time_point retry_at = connection.retry_at();
  EXPECT_EQ(connection.poll(), nullptr);
  EXPECT_EQ(connection.retry_at(), retry_at);
  // Due: another attempt, which fails the same and waits again.
  std::this_thread::sleep_until(retry_at);
  const SteadyClock::time_point second = SteadyClock::now();
  EXPECT_EQ(connection.poll(), nullptr);
  EXPECT_GE(connection.retry_at(), second + kRetry);
  EXPECT_EQ(connection.last_error(), "an empty host");
  // The default retry is kLiveRetry.
  LiveConnection defaulted({"", 47801});
  const SteadyClock::time_point before = SteadyClock::now();
  EXPECT_EQ(defaulted.poll(), nullptr);
  EXPECT_GE(defaulted.retry_at(), before + kLiveRetry);
}

TEST(LiveConnection, ADropWithAnErrorWaitsTheRetryAndACleanOneDoesNot) {
  const std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  LiveConnection connection({"127.0.0.1", server->port()}, kRetry);
  networking::StreamClient* const first = connection.poll();
  ASSERT_NE(first, nullptr);
  ASSERT_TRUE(has_session(*first));
  // An error: why, less StreamClient's prefix, and no client until the retry.
  const SteadyClock::time_point dropped = SteadyClock::now();
  connection.drop(core::Status::io_error(
      "StreamClient: " + to_string(connection.address()) +
      ": no byte for 10000 ms"));
  EXPECT_EQ(connection.last_error(), "no byte for 10000 ms");
  EXPECT_GE(connection.retry_at(), dropped + kRetry);
  EXPECT_EQ(connection.poll(), nullptr);
  // A clean drop during the wait, as a park's, ends it and keeps why.
  connection.drop();
  EXPECT_LE(connection.retry_at(), SteadyClock::now());
  EXPECT_EQ(connection.last_error(), "no byte for 10000 ms");
  networking::StreamClient* const second = connection.poll();
  ASSERT_NE(second, nullptr);
  ASSERT_TRUE(has_session(*second));
  // A clean end of a connection leaves no reason, and the next poll connects.
  connection.drop();
  EXPECT_TRUE(connection.last_error().empty());
  EXPECT_LE(connection.retry_at(), SteadyClock::now());
  networking::StreamClient* const third = connection.poll();
  ASSERT_NE(third, nullptr);
  EXPECT_TRUE(has_session(*third));
}

TEST(LiveConnection, AServerThatStopsEndsTheClientAndDropShowsWhy) {
  std::unique_ptr<networking::StreamServer> server = serve();
  ASSERT_NE(server, nullptr);
  LiveConnection connection({"127.0.0.1", server->port()}, kRetry);
  networking::StreamClient* const client = connection.poll();
  ASSERT_NE(client, nullptr);
  ASSERT_TRUE(has_session(*client));
  server.reset();
  const core::Status ended = end_of(*client);
  ASSERT_FALSE(ended.ok());
  connection.drop(ended);
  EXPECT_EQ(connection.last_error(), "the sender closed the connection");
  EXPECT_EQ(connection.poll(), nullptr);
}

}  // namespace
