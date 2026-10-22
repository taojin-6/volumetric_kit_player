// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// What a Player scenario plays, on the host. open_source: a file source opens
// its stream, a missing one is StreamPlayer::open's refusal, and a live source
// connects with no I/O, on the sender's default port for 0 and on its own
// otherwise. folder_source, over a folder of the test's own: its scenario wins
// over a .vrst wherever it is in the names; one the core refuses or cannot
// read is the error, never passed over for a .vrst; with none, the first
// .vrst in the names' order, in any case; with neither, NotFound; and the
// scenario's relative path resolves against the folder.

#include "volumetric_kit/player/scenario.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
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
using volumetric_kit::player::kFolderScenario;
using volumetric_kit::player::LiveAddress;
using volumetric_kit::player::LiveState;
using volumetric_kit::player::open_source;
using volumetric_kit::player::StreamPlayer;
using vp_test::Scratch;
using Code = core::Status::Code;
using Names = std::vector<std::string>;

// A Player scenario: the envelope, and a stream source of `fields`.
std::string scenario(const std::string& fields) {
  return R"({"version":1,"operation":"view","source":{"kind":"stream",)" +
         fields + "}}";
}

// `text` in the file at `path`.
void write_file(const std::string& path, const std::string& text) {
  std::ofstream(path) << text;
}

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

TEST(FolderSource, ItsScenarioWinsOverAStream) {
  const Scratch scratch;
  write_file(scratch.file(kFolderScenario),
             scenario(R"("host":"lab.invalid","port":1)"));
  ASSERT_TRUE(
      vp_test::write_undecodable_stream(scratch.file("a.vrst"), 3).ok());
  for (const Names& names :
       {Names{"a.vrst", kFolderScenario}, Names{kFolderScenario, "a.vrst"}}) {
    const core::Result<cfg::Source> source =
        folder_source(scratch.path(), names);
    ASSERT_TRUE(source.ok()) << source.status().message();
    EXPECT_EQ(source->host, "lab.invalid");
    EXPECT_EQ(source->port, 1u);
    EXPECT_TRUE(source->path.empty()) << source->path;
  }
}

TEST(FolderSource, AScenarioItCannotUseIsTheErrorNotAStream) {
  const Scratch scratch;
  ASSERT_TRUE(
      vp_test::write_undecodable_stream(scratch.file("a.vrst"), 3).ok());
  const std::string path = scratch.file(kFolderScenario);
  const Names names = {"a.vrst", kFolderScenario};
  // The core's refusal, whole.
  const auto expect_refused = [&](const char* what) {
    const core::Result<cfg::Config> loaded =
        cfg::load(path, cfg::Application::Player);
    ASSERT_FALSE(loaded.ok()) << what;
    const core::Result<cfg::Source> source =
        folder_source(scratch.path(), names);
    ASSERT_FALSE(source.ok()) << what << ": " << source->path;
    EXPECT_EQ(source.status().domain(), loaded.status().domain()) << what;
    EXPECT_EQ(source.status().message(), loaded.status().message()) << what;
  };
  // Listed, but gone since.
  expect_refused("gone");
  for (const char* text :
       {"{", R"({"version":1,"operation":"view"})",
        R"({"version":1,"operation":"view","source":{"kind":"stream",)"
        R"("path":"a.vrst","host":"lab.invalid"}})"}) {
    write_file(path, text);
    expect_refused(text);
  }
  std::filesystem::remove(path);
  std::filesystem::create_directory(path);
  expect_refused("a directory");
}

TEST(FolderSource, WithNoScenarioTheFirstStreamInTheNamesOrder) {
  const Scratch scratch;
  struct Case {
    Names names;
    const char* first;
  };
  // The iOS app's are in Finder's order, "Take 2" before "take 10".
  for (const Case& listed :
       {Case{{"notes.txt", "Take 2.VRST", "take 10.vrst", "z.vrst"},
             "Take 2.VRST"},
        Case{{"z.vrst", "Take 2.VRST"}, "z.vrst"},
        Case{{"a.vrst.txt", "player.json.txt", "b.Vrst"}, "b.Vrst"}}) {
    const core::Result<cfg::Source> source =
        folder_source(scratch.path(), listed.names);
    ASSERT_TRUE(source.ok()) << source.status().message();
    EXPECT_EQ(source->kind, cfg::SourceKind::Stream);
    EXPECT_EQ(source->path,
              (std::filesystem::path(scratch.path()) / listed.first).string());
    EXPECT_TRUE(source->host.empty());
  }
}

TEST(FolderSource, WithNeitherNotFound) {
  const Scratch scratch;
  for (const Names& names :
       {Names{}, Names{"notes.txt", "a.vrst.txt", "player.json.txt", "vrst"}}) {
    EXPECT_EQ(folder_source(scratch.path(), names).status().domain(),
              Code::NotFound);
  }
}

TEST(FolderSource, ItsScenariosRelativePathResolvesAgainstTheFolder) {
  const Scratch scratch;
  const std::filesystem::path folder(scratch.path());
  std::filesystem::create_directory(folder / "takes");
  const std::string take = (folder / "takes" / "take.vrst").string();
  ASSERT_TRUE(vp_test::write_undecodable_stream(take, 3).ok());
  write_file(scratch.file(kFolderScenario),
             scenario(R"("path":"takes/take.vrst")"));
  const core::Result<cfg::Source> source =
      folder_source(scratch.path(), {kFolderScenario});
  ASSERT_TRUE(source.ok()) << source.status().message();
  EXPECT_EQ(source->path,
            std::filesystem::path(take).lexically_normal().string());
  const core::Result<std::unique_ptr<StreamPlayer>> player =
      open_source(*source);
  ASSERT_TRUE(player.ok()) << player.status().message();
  EXPECT_EQ((*player)->frame_count(), 3u);
}

}  // namespace
