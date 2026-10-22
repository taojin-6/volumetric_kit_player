// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// What a Player scenario plays, on the host. open_source: a file source opens
// its stream, a missing one is StreamPlayer::open's refusal, and a live source
// connects with no I/O, on the sender's default port for 0 and on its own
// otherwise. parse_live_source, the iOS dialog's fields: the host and the port
// trimmed; an empty port the sender's default, through open_source; and
// refused, an empty host, a host with a ':', pointing at the Port field, and a
// port parse_live_address refuses, with its message. folder_source: the first
// .vrst in the names' order, in any case, in the folder; with none, nothing.

#include "volumetric_kit/player/scenario.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "test_stream.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/config/config.hpp"
#include "volumetric_kit/player/stream_player.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace cfg = volumetric_kit::core::config;
using volumetric_kit::player::folder_source;
using volumetric_kit::player::LiveAddress;
using volumetric_kit::player::LiveState;
using volumetric_kit::player::open_source;
using volumetric_kit::player::parse_live_address;
using volumetric_kit::player::parse_live_source;
using volumetric_kit::player::StreamPlayer;
using vp_test::Scratch;
using Code = core::Status::Code;
using Names = std::vector<std::string>;

// A stream source of the file at `path`, as the core loads one.
cfg::Source file_source(const std::string& path) {
  cfg::Source source;
  source.kind = cfg::SourceKind::Stream;
  source.path = path;
  return source;
}

// A live source of `host` on `port`, as the core loads one.
cfg::Source live_source(const std::string& host, std::uint16_t port) {
  cfg::Source source;
  source.kind = cfg::SourceKind::Stream;
  source.host = host;
  source.port = port;
  return source;
}

TEST(OpenSource, AFileSourceOpensItsStream) {
  const Scratch scratch;
  const std::string path = scratch.file("take.vrst");
  ASSERT_TRUE(vp_test::write_undecodable_stream(path, 3).ok());
  const core::Result<std::unique_ptr<StreamPlayer>> player =
      open_source(file_source(path));
  ASSERT_TRUE(player.ok()) << player.status().message();
  EXPECT_FALSE((*player)->live());
  EXPECT_EQ((*player)->frame_count(), 3u);
}

TEST(OpenSource, AMissingFileIsOpensRefusal) {
  const Scratch scratch;
  const std::string path = scratch.file("missing.vrst");
  const core::Result<std::unique_ptr<StreamPlayer>> player =
      open_source(file_source(path));
  EXPECT_EQ(player.status().domain(), Code::NotFound);
  EXPECT_EQ(player.status().message(),
            StreamPlayer::open(path).status().message());
}

// No I/O: nothing serves there, and nothing need.
TEST(OpenSource, ALiveSourceConnectsOnItsPortOrTheSendersDefault) {
  struct Case {
    std::uint16_t port;
    std::string address;
  };
  for (const Case& live :
       {Case{0, to_string(LiveAddress{"lab.invalid"})},
        Case{1, "lab.invalid:1"}, Case{65535, "lab.invalid:65535"}}) {
    const core::Result<std::unique_ptr<StreamPlayer>> player =
        open_source(live_source("lab.invalid", live.port));
    ASSERT_TRUE(player.ok()) << live.port << ": " << player.status().message();
    EXPECT_TRUE((*player)->live());
    EXPECT_EQ((*player)->live_status().address, live.address);
    EXPECT_EQ((*player)->live_status().state, LiveState::kConnecting);
  }
}

TEST(ParseLiveSource, TakesTheHostAndThePortTrimmed) {
  struct Case {
    const char* host;
    const char* port;
    const char* trimmed;
    std::uint16_t number;
  };
  for (const Case& typed :
       {Case{"lab", "1", "lab", 1},
        Case{"  lab.invalid ", "65535 ", "lab.invalid", 65535},
        Case{"\t192.168.21.217\r\n", " 2\n", "192.168.21.217", 2}}) {
    const core::Result<cfg::Source> source =
        parse_live_source(typed.host, typed.port);
    ASSERT_TRUE(source.ok()) << typed.host << ": " << source.status().message();
    EXPECT_EQ(source->kind, cfg::SourceKind::Stream);
    EXPECT_EQ(source->host, typed.trimmed);
    EXPECT_EQ(source->port, typed.number);
    EXPECT_TRUE(source->path.empty()) << source->path;
  }
}

// Port 0, which open_source plays on LiveAddress's default; no I/O.
TEST(ParseLiveSource, AnEmptyPortIsTheSendersDefault) {
  for (const char* port : {"", "   "}) {
    const core::Result<cfg::Source> source =
        parse_live_source("lab.invalid", port);
    ASSERT_TRUE(source.ok())
        << "'" << port << "': " << source.status().message();
    EXPECT_EQ(source->port, 0u);
    const core::Result<std::unique_ptr<StreamPlayer>> player =
        open_source(*source);
    ASSERT_TRUE(player.ok()) << player.status().message();
    EXPECT_EQ((*player)->live_status().address,
              to_string(LiveAddress{"lab.invalid"}));
  }
}

TEST(ParseLiveSource, RefusesAnEmptyHost) {
  for (const char* host : {"", "   ", "\t\n"}) {
    for (const char* port : {"", "5"}) {
      const core::Status refused = parse_live_source(host, port).status();
      EXPECT_EQ(refused.domain(), Code::InvalidArgument)
          << "'" << host << "', '" << port << "'";
      EXPECT_EQ(refused.message(),
                "no host: type the sender's name or IPv4 address");
    }
  }
}

// "host:port" in the host field points at the Port field, whatever is in it.
TEST(ParseLiveSource, RefusesAColonInTheHostForThePortField) {
  for (const char* host : {"lab:5", " lab:5 ", "lab:", ":5"}) {
    for (const char* port : {"", "5"}) {
      const core::Status refused = parse_live_source(host, port).status();
      EXPECT_EQ(refused.domain(), Code::InvalidArgument)
          << "'" << host << "', '" << port << "'";
      EXPECT_NE(refused.message().find("Port field"), std::string::npos)
          << refused.message();
    }
  }
  EXPECT_EQ(parse_live_source(" lab:5 ", "").status().message(),
            "'lab:5' has a ':': type the port in the Port field");
}

// parse_live_address's refusal, whole, of the address the fields make.
TEST(ParseLiveSource, RefusesAPortThatIsNotANumberFrom1To65535) {
  for (const char* port :
       {"0", "65536", "x", "-1", "+5", "5 6", "5:6", "99999999999999999999"}) {
    const core::Status refused = parse_live_source("lab", port).status();
    EXPECT_EQ(refused.domain(), Code::InvalidArgument) << "'" << port << "'";
    EXPECT_EQ(
        refused.message(),
        parse_live_address(std::string("lab:") + port).status().message());
  }
}

TEST(FolderSource, TheFirstStreamInTheNamesOrder) {
  const std::filesystem::path folder("/Documents");
  struct Case {
    Names names;
    const char* first;
  };
  // The iOS app's are in Finder's order, "Take 2" before "take 10".
  for (const Case& listed :
       {Case{{"notes.txt", "Take 2.VRST", "take 10.vrst", "z.vrst"},
             "Take 2.VRST"},
        Case{{"z.vrst", "Take 2.VRST"}, "z.vrst"},
        Case{{"a.vrst.txt", "b.Vrst"}, "b.Vrst"}}) {
    const std::optional<cfg::Source> source =
        folder_source(folder.string(), listed.names);
    ASSERT_TRUE(source.has_value()) << listed.first;
    EXPECT_EQ(source->kind, cfg::SourceKind::Stream);
    EXPECT_EQ(source->path, (folder / listed.first).string());
    EXPECT_TRUE(source->host.empty());
  }
}

TEST(FolderSource, WithNoStreamNothing) {
  for (const Names& names :
       {Names{}, Names{"notes.txt", "a.vrst.txt", "vrst", ".vrst.json"}}) {
    const std::optional<cfg::Source> source =
        folder_source("/Documents", names);
    EXPECT_FALSE(source.has_value()) << source->path;
  }
}

}  // namespace
