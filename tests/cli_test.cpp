// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The desktop app's command line, the app run as a process: usage, a bad
// --frames, a bad --connect and --connect with a path exit 2; a missing file
// and one that is not a .vrst exit 1 with StreamPlayer::open's refusal and
// nothing else, before glfwInit. No case hands the app a stream it can play
// but a valid --connect, which does no I/O before the window, so only
// VP_NO_WINDOW stops it. Each runs with VP_NO_WINDOW set, which stops the app
// just before glfwInit with a reason of its own: a refusal that moved past
// that point fails here, on every OS, rather than open a window (AGENTS.md).
// A run not back within kPrompt, as a window's loop would not be, is killed
// and fails the test. On the host: parse_count, which --frames and a port
// take.

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
#include <fstream>
#include <string>
#include <vector>

#include "test_stream.hpp"
#include "volumetric_kit/player/command_line.hpp"

extern char** environ;

namespace {

using namespace std::chrono_literals;
using volumetric_kit::player::parse_count;
using vp_test::Scratch;
using SteadyClock = std::chrono::steady_clock;

constexpr const char* kApp = VP_PLAYER_APP;  // tests/CMakeLists.txt
constexpr auto kPrompt = 10s;

struct Outcome {
  int exit_code = -1;  // -1 unless it exited
  std::string output;  // stdout and stderr
};

// The app on `args`, with VP_NO_WINDOW set; killed after kPrompt.
Outcome run_app(const std::vector<std::string>& args) {
  Outcome run;
  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) {
    ADD_FAILURE() << "pipe: " << std::strerror(errno);
    return run;
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
  posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
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
  close(pipe_fds[1]);
  if (spawned != 0) {
    close(pipe_fds[0]);
    ADD_FAILURE() << "posix_spawn " << kApp << ": " << std::strerror(spawned);
    return run;
  }
  // Read to the end of its output, which comes as it exits.
  const SteadyClock::time_point deadline = SteadyClock::now() + kPrompt;
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - SteadyClock::now());
    pollfd out{pipe_fds[0], POLLIN, 0};
    const int ready =
        left.count() > 0 ? poll(&out, 1, static_cast<int>(left.count())) : 0;
    char buffer[4096];
    const ssize_t got =
        ready > 0 ? read(pipe_fds[0], buffer, sizeof buffer) : ready;
    const int error = errno;
    if (got > 0) {
      run.output.append(buffer, static_cast<std::size_t>(got));
    } else if (got < 0 && error == EINTR) {
      // A signal interrupted poll or read: try again.
    } else if (got == 0 && ready > 0) {
      break;  // the end of its output
    } else {
      kill(pid, SIGKILL);
      if (ready == 0) {
        ADD_FAILURE() << "the app was not back within " << kPrompt.count()
                      << " s";
      } else {
        ADD_FAILURE() << "poll or read: " << std::strerror(error);
      }
      break;
    }
  }
  close(pipe_fds[0]);
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

// The app's one line for a refusal with `domain`, naming `path`.
void expect_refused(const Outcome& run, const std::string& domain,
                    const std::string& path) {
  EXPECT_EQ(run.exit_code, 1) << run.output;
  const std::string prefix = "volumetric_kit_player: " + domain + ": ";
  EXPECT_EQ(run.output.rfind(prefix, 0), 0u) << run.output;
  EXPECT_NE(run.output.find(path), std::string::npos) << run.output;
  EXPECT_EQ(std::count(run.output.begin(), run.output.end(), '\n'), 1)
      << run.output;
}

TEST(CliTest, UsageExits2) {
  const Scratch scratch;
  const std::string path = scratch.file("missing.vrst");
  const std::vector<std::vector<std::string>> cases = {
      {}, {"--frames", "5"}, {path, path}, {"--fast", path}, {path, "-v"}};
  for (const std::vector<std::string>& args : cases) {
    const Outcome run = run_app(args);
    EXPECT_EQ(run.exit_code, 2) << run.output;
    EXPECT_EQ(run.output.rfind("usage: volumetric_kit_player", 0), 0u)
        << run.output;
  }
}

TEST(CliTest, BadFramesExits2) {
  const Scratch scratch;
  const std::string path = scratch.file("missing.vrst");
  for (const char* frames :
       {"0", "-1", "+5", "five", "5x", "", "99999999999999999999"}) {
    const Outcome run = run_app({"--frames", frames, path});
    EXPECT_EQ(run.exit_code, 2) << "--frames '" << frames << "'\n"
                                << run.output;
  }
  const Outcome last = run_app({path, "--frames"});
  EXPECT_EQ(last.exit_code, 2) << last.output;
  const Outcome twice = run_app({"--frames", "5", "--frames", "6", path});
  EXPECT_EQ(twice.exit_code, 2) << twice.output;
}

TEST(CliTest, BadConnectExits2) {
  const Scratch scratch;
  const std::string path = scratch.file("missing.vrst");
  const std::vector<std::vector<std::string>> cases = {
      {"--connect"},
      {"--connect", ""},
      {"--connect", ":5"},
      {"--connect", "h:0"},
      {"--connect", "h:65536"},
      {"--connect", "h:x"},
      {"--connect", "h:5:6"},
      {"--connect", "h", "--connect", "g"},
      {"--connect", "h", path},
      {path, "--connect", "h"}};
  for (const std::vector<std::string>& args : cases) {
    const Outcome run = run_app(args);
    EXPECT_EQ(run.exit_code, 2) << args.back() << "\n" << run.output;
    EXPECT_EQ(run.output.rfind("usage: volumetric_kit_player", 0), 0u)
        << run.output;
  }
}

// connect does no I/O, so a valid address passes every refusal and stops at
// the backstop, whether or not anything serves there.
TEST(CliTest, AValidConnectReachesTheNoWindowBackstop) {
  for (const std::vector<std::string>& args :
       std::vector<std::vector<std::string>>{
           {"--connect", "127.0.0.1:1"},
           {"--frames", "60", "--connect", "lab.invalid"}}) {
    const Outcome run = run_app(args);
    EXPECT_EQ(run.exit_code, 1) << run.output;
    EXPECT_EQ(run.output,
              "volumetric_kit_player: Unsupported: VP_NO_WINDOW is set, so no "
              "window\n");
  }
}

TEST(CliTest, RefusesAMissingFileBeforeGlfw) {
  const Scratch scratch;
  const std::string path = scratch.file("missing.vrst");
  expect_refused(run_app({path}), "NotFound", path);
  expect_refused(run_app({"--frames", "60", path}), "NotFound", path);
}

TEST(CliTest, RefusesAFileThatIsNotAStreamBeforeGlfw) {
  const Scratch scratch;
  const std::string path = scratch.file("notes.txt");
  std::ofstream(path) << "not a stream, but long enough to have been one if "
                         "it began with the magic\n";
  expect_refused(run_app({path}), "InvalidArgument", path);
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
