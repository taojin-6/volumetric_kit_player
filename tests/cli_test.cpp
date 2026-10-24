// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The desktop app's entry point, the app run as a process. A command line the
// core's parse_args refuses, the old one's included, exits 2 with its message
// and the usage on stderr; --help and -h exit 0 with the usage on stdout. A
// scenario the core cannot load, or refuses, exits 2 with its message. One it
// loads runs: a missing stream and a file that is not one exit 1 with
// StreamPlayer::open's refusal and nothing else, before glfwInit. Each runs
// with VP_NO_WINDOW set, which stops the app just before glfwInit with a
// refusal that names what the window would play, its title (DECISIONS.md); a
// stream that opens, and a live scenario, which nothing refuses, exit 1 with
// that refusal. A run not back within kPrompt, as a window's loop would not
// be, is killed and fails the test. On the host: the scenarios in
// config/examples/, and parse_count, which vp_probe's --frames and a
// live address's port take.

#include <gtest/gtest.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "test_stream.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/config/config.hpp"
#include "volumetric_kit/player/command_line.hpp"
#include "volumetric_kit/player/stream_player.hpp"

extern char** environ;

namespace {

namespace core = volumetric_kit::core;
namespace cfg = volumetric_kit::core::config;
using namespace std::chrono_literals;
using volumetric_kit::player::LiveAddress;
using volumetric_kit::player::parse_count;
using vp_test::Scratch;
using SteadyClock = std::chrono::steady_clock;

constexpr const char* kApp = VP_PLAYER_APP;            // tests/CMakeLists.txt
constexpr const char* kExamples = VP_CONFIG_EXAMPLES;  // likewise
constexpr auto kPrompt = 10s;

// The backstop's line, for a window titled `title`.
std::string no_window(const std::string& title) {
  return "volumetric_kit_player: Unsupported: VP_NO_WINDOW is set, so no "
         "window for " +
         title + "\n";
}

struct Outcome {
  int exit_code = -1;  // -1 unless it exited
  std::string out;     // stdout
  std::string err;     // stderr
};

// The app on `args`, with VP_NO_WINDOW set; killed after kPrompt.
Outcome run_app(const std::vector<std::string>& args) {
  Outcome run;
  int out_fds[2];
  int err_fds[2];
  if (pipe(out_fds) != 0) {
    ADD_FAILURE() << "pipe: " << std::strerror(errno);
    return run;
  }
  if (pipe(err_fds) != 0) {
    ADD_FAILURE() << "pipe: " << std::strerror(errno);
    close(out_fds[0]);
    close(out_fds[1]);
    return run;
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, out_fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, err_fds[1], STDERR_FILENO);
  for (const int fd : {out_fds[0], out_fds[1], err_fds[0], err_fds[1]}) {
    posix_spawn_file_actions_addclose(&actions, fd);
  }
  std::vector<char*> argv{const_cast<char*>(kApp)};
  for (const std::string& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);
  std::vector<char*> env{const_cast<char*>("VP_NO_WINDOW=1")};
  for (char** var = environ; *var != nullptr; ++var) {
    env.push_back(*var);
  }
  env.push_back(nullptr);
  pid_t pid = 0;
  const int spawned =
      posix_spawn(&pid, kApp, &actions, nullptr, argv.data(), env.data());
  posix_spawn_file_actions_destroy(&actions);
  close(out_fds[1]);
  close(err_fds[1]);
  if (spawned != 0) {
    close(out_fds[0]);
    close(err_fds[0]);
    ADD_FAILURE() << "posix_spawn " << kApp << ": " << std::strerror(spawned);
    return run;
  }
  // Read both to the end of their output, which comes as it exits.
  pollfd fds[2] = {{out_fds[0], POLLIN, 0}, {err_fds[0], POLLIN, 0}};
  std::string* const into[2] = {&run.out, &run.err};
  const SteadyClock::time_point deadline = SteadyClock::now() + kPrompt;
  bool failed = false;
  while (!failed && (fds[0].fd >= 0 || fds[1].fd >= 0)) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - SteadyClock::now());
    const int ready =
        left.count() > 0 ? poll(fds, 2, static_cast<int>(left.count())) : 0;
    const int error = errno;
    if (ready < 0 && error == EINTR) {
      continue;  // a signal interrupted poll: try again
    }
    if (ready == 0) {
      ADD_FAILURE() << "the app was not back within " << kPrompt.count()
                    << " s";
      failed = true;
    } else if (ready < 0) {
      ADD_FAILURE() << "poll: " << std::strerror(error);
      failed = true;
    }
    for (int i = 0; i < 2 && !failed; ++i) {
      if (fds[i].fd < 0 || fds[i].revents == 0) {
        continue;
      }
      char buffer[4096];
      const ssize_t got = read(fds[i].fd, buffer, sizeof buffer);
      if (got > 0) {
        into[i]->append(buffer, static_cast<std::size_t>(got));
      } else if (got == 0) {  // the end of this one
        close(fds[i].fd);
        fds[i].fd = -1;
      } else if (errno != EINTR) {
        ADD_FAILURE() << "read: " << std::strerror(errno);
        failed = true;
      }
    }
  }
  if (failed) {
    kill(pid, SIGKILL);
  }
  for (const pollfd& fd : fds) {
    if (fd.fd >= 0) {
      close(fd.fd);
    }
  }
  int status = 0;
  pid_t waited = 0;
  do {
    waited = waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited == pid && WIFEXITED(status)) {
    run.exit_code = WEXITSTATUS(status);
  }
  return run;
}

// A Player scenario: the envelope, a stream source of `fields`, then
// `sections`, each with its leading comma.
std::string scenario(const std::string& fields,
                     const std::string& sections = "") {
  return R"({"version":1,"operation":"view","source":{"kind":"stream",)" +
         fields + "}" + sections + "}";
}

// `text` in the file at `path`.
void write_file(const std::string& path, const std::string& text) {
  std::ofstream(path) << text;
}

// The app's one line for `refused`, as main prints a Status.
std::string line_of(const core::Status& refused) {
  return "volumetric_kit_player: " +
         std::string(core::to_string(refused.domain())) + ": " +
         refused.message() + "\n";
}

// The app on `args`, which the core's parse_args refuses: exit 2, with its
// message and then the usage on stderr.
void expect_usage(const std::vector<std::string>& args) {
  std::vector<const char*> argv{kApp};
  for (const std::string& arg : args) {
    argv.push_back(arg.c_str());
  }
  const core::Result<cfg::Arguments> parsed =
      cfg::parse_args(static_cast<int>(argv.size()), argv.data());
  ASSERT_FALSE(parsed.ok());
  const Outcome run = run_app(args);
  EXPECT_EQ(run.exit_code, 2) << run.err;
  EXPECT_EQ(run.out, "");
  EXPECT_EQ(run.err, "volumetric_kit_player: " + parsed.status().message() +
                         "\n" + cfg::usage());
}

// The app on the scenario at `path`, which the core refuses for `reason`:
// exit 2, with the core's message alone.
void expect_scenario_refused(const std::string& path,
                             const std::string& reason) {
  const core::Result<cfg::Config> loaded =
      cfg::load(path, cfg::Application::Player);
  ASSERT_FALSE(loaded.ok());
  EXPECT_NE(loaded.status().message().find(reason), std::string::npos)
      << loaded.status().message();
  const Outcome run = run_app({"--config", path});
  EXPECT_EQ(run.exit_code, 2) << run.err;
  EXPECT_EQ(run.out, "");
  EXPECT_EQ(run.err, line_of(loaded.status()));
}

// The app's one line for a refusal of the run with `domain`, naming `path`.
void expect_refused(const Outcome& run, const std::string& domain,
                    const std::string& path) {
  EXPECT_EQ(run.exit_code, 1) << run.err;
  EXPECT_EQ(run.out, "");
  const std::string prefix = "volumetric_kit_player: " + domain + ": ";
  EXPECT_EQ(run.err.rfind(prefix, 0), 0u) << run.err;
  EXPECT_NE(run.err.find(path), std::string::npos) << run.err;
  EXPECT_EQ(std::count(run.err.begin(), run.err.end(), '\n'), 1) << run.err;
}

// The core's config_test has parse_args's rules; these show only that the app
// passes on its refusal: of no argument, of --config with no path, and of the
// command line before the scenario.
TEST(CliTest, ACommandLineTheCoreRefusesExits2WithTheUsage) {
  for (const std::vector<std::string>& args :
       std::vector<std::vector<std::string>>{
           {}, {"--config"}, {"--frames", "60", "stream.vrst"}}) {
    expect_usage(args);
  }
}

TEST(CliTest, HelpPrintsTheUsageOnStdout) {
  for (const char* help : {"--help", "-h"}) {
    const Outcome run = run_app({help});
    EXPECT_EQ(run.exit_code, 0) << help << "\n" << run.err;
    EXPECT_EQ(run.out, cfg::usage());
    EXPECT_EQ(run.err, "");
  }
}

TEST(CliTest, AScenarioTheCoreCannotLoadExits2) {
  const Scratch scratch;
  const std::string path = scratch.file("missing.json");
  expect_scenario_refused(path, path);
}

TEST(CliTest, AScenarioTheCoreRefusesExits2WithItsField) {
  const Scratch scratch;
  const std::string path = scratch.file("play.json");
  struct Case {
    std::string text;
    const char* reason;
  };
  // The core's config_test has its rules; these show only that the app
  // passes on its refusal, by the field it names.
  const std::vector<Case> cases = {
      {scenario(R"("path":"s.vrst","host":"lab")"), "source:"},
      {scenario(R"("path":"s.vrst")", R"(,"window":{"overlay":true})"),
       "window.overlay:"},
      {R"({"version":1,"operation":"view",)"
       R"("source":{"kind":"dataset","path":"room0"}})",
       "source.kind:"}};
  for (const Case& refused : cases) {
    write_file(path, refused.text);
    expect_scenario_refused(path, refused.reason);
  }
}

TEST(CliTest, RefusesAMissingStreamBeforeGlfw) {
  const Scratch scratch;
  const std::string path = scratch.file("play.json");
  write_file(path, scenario(R"("path":"missing.vrst")"));
  expect_refused(run_app({"--config", path}), "NotFound",
                 scratch.file("missing.vrst"));
  write_file(path,
             scenario(R"("path":"missing.vrst")", R"(,"run":{"frames":60})"));
  expect_refused(run_app({"--config", path}), "NotFound",
                 scratch.file("missing.vrst"));
}

TEST(CliTest, RefusesAFileThatIsNotAStreamBeforeGlfw) {
  const Scratch scratch;
  const std::string notes = scratch.file("notes.txt");
  std::ofstream(notes) << "not a stream, but long enough to have been one if "
                          "it began with the magic\n";
  const std::string path = scratch.file("play.json");
  write_file(path, scenario(R"("path":"notes.txt")"));
  expect_refused(run_app({"--config", path}), "InvalidArgument", notes);
}

// A stream that opens passes every refusal and stops at the backstop.
TEST(CliTest, AStreamThatOpensReachesTheNoWindowBackstop) {
  const Scratch scratch;
  ASSERT_TRUE(
      vp_test::write_undecodable_stream(scratch.file("stream.vrst"), 3).ok());
  const std::string path = scratch.file("play.json");
  for (const char* sections :
       {"", R"(,"window":{"width":1600,"height":900,"validation":true})",
        R"(,"run":{"frames":60})"}) {
    write_file(path, scenario(R"("path":"stream.vrst")", sections));
    const Outcome run = run_app({"--config", path});
    EXPECT_EQ(run.exit_code, 1) << sections << "\n" << run.err;
    EXPECT_EQ(run.out, "");
    EXPECT_EQ(run.err, no_window(scratch.file("stream.vrst")));
  }
}

// connect does no I/O, so a valid sender passes every refusal and stops at
// the backstop, whether or not anything serves there, naming the address it
// would play: the scenario's port, or for 0 or none the sender's default.
TEST(CliTest, ALiveScenarioReachesTheNoWindowBackstop) {
  const Scratch scratch;
  const std::string path = scratch.file("play.json");
  struct Case {
    std::string text;
    std::string address;
  };
  const std::vector<Case> cases = {
      {scenario(R"("host":"127.0.0.1")"), to_string(LiveAddress{"127.0.0.1"})},
      {scenario(R"("host":"lab.invalid","port":0)"),
       to_string(LiveAddress{"lab.invalid"})},
      {scenario(R"("host":"127.0.0.1","port":1)"), "127.0.0.1:1"},
      {scenario(R"("host":"lab.invalid")", R"(,"run":{"frames":60})"),
       to_string(LiveAddress{"lab.invalid"})}};
  for (const Case& live : cases) {
    write_file(path, live.text);
    const Outcome run = run_app({"--config", path});
    EXPECT_EQ(run.exit_code, 1) << live.text << "\n" << run.err;
    EXPECT_EQ(run.out, "");
    EXPECT_EQ(run.err, no_window(live.address));
  }
}

// Each example loads as a Player scenario: the file one plays the stream it
// names beside it, the live one this machine's sender on the sender's default
// port.
TEST(CliTest, TheExamplesAreScenariosTheCoreLoads) {
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(kExamples)) {
    if (entry.path().extension() == ".json") {
      const core::Result<cfg::Config> loaded =
          cfg::load(entry.path().string(), cfg::Application::Player);
      EXPECT_TRUE(loaded.ok()) << loaded.status().message();
    }
  }
  const std::filesystem::path dir(kExamples);
  const core::Result<cfg::Config> file =
      cfg::load((dir / "play_file.json").string(), cfg::Application::Player);
  ASSERT_TRUE(file.ok()) << file.status().message();
  EXPECT_EQ(file->source.path, (dir / "data" / "rafa2.vrst").string());
  EXPECT_TRUE(file->source.host.empty());
  const core::Result<cfg::Config> live =
      cfg::load((dir / "play_live.json").string(), cfg::Application::Player);
  ASSERT_TRUE(live.ok()) << live.status().message();
  EXPECT_EQ(live->source.host, "127.0.0.1");
  EXPECT_EQ(live->source.port, 0u);
  EXPECT_TRUE(live->source.path.empty());
}

TEST(ParseCount, TakesDecimalDigitsAloneFromOne) {
  EXPECT_EQ(parse_count("1"), 1u);
  EXPECT_EQ(parse_count("60"), 60u);
  EXPECT_EQ(parse_count("18446744073709551615"), UINT64_MAX);
  for (const char* text : {"", "0", "-1", "+5", "five", "5x", " 5", "5 ",
                           "18446744073709551616"}) {
    EXPECT_FALSE(parse_count(text).has_value()) << "'" << text << "'";
  }
}

}  // namespace
