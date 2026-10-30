// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// vp_audio_check: the Apple audio output checked by hand on a Mac, with no
// window. It opens the output (open_apple_audio), at gain 0 unless --gain
// says otherwise, plays a synthetic tone for --seconds, and prints the
// route, its latency, how the position runs against steady_clock (its rate
// and drift, and the reading farthest off that line, which the rate
// averages out), the position's steps back, the clock's runs and the
// underruns. ctest never runs it: it opens the audio device, which no test
// does (AGENTS.md).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/player/audio_clock.hpp"
#include "volumetric_kit/player/device_audio_out.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace vkp = volumetric_kit::player;
using SteadyClock = std::chrono::steady_clock;

constexpr const char* kUsage =
    "usage: vp_audio_check [--seconds N] [--gain G] [--rate HZ] "
    "[--channels 1|2] [--tone HZ]\n"
    "  Plays a tone through the Apple output and prints its route, latency,\n"
    "  position rate, drift and largest residual against steady_clock, its\n"
    "  steps back, the clock's runs, and underruns.\n"
    "  --seconds  how long to play: 0.5 to 3600 (10)\n"
    "  --gain     0 to 1; 0, silent, unless given (0)\n"
    "  --rate     frames a second, whole: 8000 to 384000 (48000)\n"
    "  --channels 1 or 2 (2)\n"
    "  --tone     its frequency in Hz (440)\n";

// The lead the tool keeps written ahead, and how often it looks.
constexpr std::chrono::milliseconds kLead{200};
constexpr std::chrono::milliseconds kEvery{10};

struct Args {
  double seconds = 10.0;
  double gain = 0.0;
  std::uint32_t rate = 48000;
  std::uint16_t channels = 2;
  double tone = 440.0;
};

// `text` as a number in [lo, hi]; none otherwise.
std::optional<double> number(const char* text, double lo, double hi) {
  char* end = nullptr;
  const double value = std::strtod(text, &end);
  if (end == text || *end != '\0' || !(value >= lo && value <= hi)) {
    return std::nullopt;
  }
  return value;
}

// `text` as a whole number in [lo, hi]; none otherwise, 1.5 included.
std::optional<double> whole(const char* text, double lo, double hi) {
  const std::optional<double> value = number(text, lo, hi);
  if (!value || *value != std::floor(*value)) {
    return std::nullopt;
  }
  return value;
}

std::optional<Args> parse(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (i + 1 >= argc) {
      return std::nullopt;
    }
    const char* value = argv[++i];
    std::optional<double> got;
    if (flag == "--seconds" && (got = number(value, 0.5, 3600.0))) {
      args.seconds = *got;
    } else if (flag == "--gain" && (got = number(value, 0.0, 1.0))) {
      args.gain = *got;
    } else if (flag == "--rate" && (got = whole(value, 8000.0, 384000.0))) {
      args.rate = static_cast<std::uint32_t>(*got);
    } else if (flag == "--channels" && (got = whole(value, 1.0, 2.0))) {
      args.channels = static_cast<std::uint16_t>(*got);
    } else if (flag == "--tone" && (got = number(value, 1.0, 20000.0))) {
      args.tone = *got;
    } else {
      return std::nullopt;
    }
  }
  return args;
}

double ms(std::uint64_t ns) { return static_cast<double>(ns) / 1e6; }

}  // namespace

int main(int argc, char** argv) {
  const std::optional<Args> args = parse(argc, argv);
  if (!args) {
    std::fputs(kUsage, stderr);
    return 2;
  }
  core::Result<std::unique_ptr<vkp::DeviceAudioOut>> opened =
      vkp::open_apple_audio({args->rate, args->channels});
  if (!opened.ok()) {
    std::fprintf(stderr, "vp_audio_check: %s\n",
                 opened.status().message().c_str());
    return 1;
  }
  vkp::DeviceAudioOut& out = **opened;
  const vkp::DeviceRoute route = out.device_route();
  std::printf(
      "route %s · presentation %.1f ms · session %.1f ms · "
      "latency %.1f ms\n",
      route.name.c_str(), route.presentation_latency_s * 1e3,
      route.session_latency_s * 1e3, ms(out.route().latency_ns));
  std::printf("playing %.0f Hz at %u Hz, %u channel(s), gain %.2f%s, %.1f s\n",
              args->tone, args->rate, args->channels, args->gain,
              args->gain == 0.0 ? " (silent)" : "", args->seconds);
  std::fflush(stdout);

  // A tone, 10 ms at a time, its phase carried.
  const std::size_t chunk = args->rate / 100;
  std::vector<float> samples(chunk * args->channels);
  double phase = 0.0;
  const double step = 2.0 * 3.14159265358979 * args->tone / args->rate;
  const auto next_chunk = [&] {
    for (std::size_t i = 0; i < chunk; ++i) {
      const auto value = static_cast<float>(0.25 * std::sin(phase));
      phase = std::fmod(phase + step, 2.0 * 3.14159265358979);
      for (std::uint16_t c = 0; c < args->channels; ++c) {
        samples[i * args->channels + c] = value;
      }
    }
  };
  const std::uint64_t lead = vkp::audio_frames(
      static_cast<std::uint64_t>(std::chrono::nanoseconds(kLead).count()),
      args->rate);
  const auto top_up = [&] {
    while (out.queued() < lead) {
      next_chunk();
      out.write(samples.data(), chunk);
    }
  };

  out.set_gain(static_cast<float>(args->gain));
  out.flush(0);
  top_up();
  out.play();
  const SteadyClock::time_point start = SteadyClock::now();
  const auto run = std::chrono::duration_cast<SteadyClock::duration>(
      std::chrono::duration<double>(args->seconds));
  std::vector<vkp::ClockSample> readings;
  // The position's steps back between readings, and the largest.
  std::uint64_t steps_back = 0;
  std::uint64_t largest_back_ns = 0;
  SteadyClock::time_point printed = start;
  while (SteadyClock::now() - start < run) {
    top_up();
    const SteadyClock::time_point now = SteadyClock::now();
    const std::optional<std::uint64_t> heard = out.position();
    if (heard && *heard > 0) {  // once the first frame is heard
      const auto value = static_cast<std::int64_t>(*heard);
      if (!readings.empty() && value < readings.back().value_ns) {
        ++steps_back;
        largest_back_ns = std::max(
            largest_back_ns,
            static_cast<std::uint64_t>(readings.back().value_ns - value));
      }
      readings.push_back({static_cast<std::int64_t>(
                              std::chrono::nanoseconds(now - start).count()),
                          value});
    }
    if (now - printed >= std::chrono::seconds(1)) {
      printed = now;
      std::printf("%5.1f s · heard %.3f s · underruns %llu\n",
                  std::chrono::duration<double>(now - start).count(),
                  heard ? static_cast<double>(*heard) / 1e9 : 0.0,
                  static_cast<unsigned long long>(out.stats().underruns));
      std::fflush(stdout);
    }
    std::this_thread::sleep_for(kEvery);
  }
  out.pause();

  const vkp::AudioOutStats stats = out.stats();
  const std::optional<vkp::ClockFit> fit = vkp::clock_fit(readings);
  std::printf("route %s · %s · latency %.1f ms\n", out.route().name.c_str(),
              out.route().audible ? "audible" : "stopped",
              ms(out.route().latency_ns));
  if (fit) {
    const double span =
        static_cast<double>(readings.back().at_ns - readings.front().at_ns) /
        1e9;
    std::printf(
        "position against steady_clock: rate %.6f · drift %+.1f ppm "
        "(%+.2f ms over %.1f s, %zu readings) · largest residual %.3f ms\n",
        fit->rate, (fit->rate - 1.0) * 1e6, (fit->rate - 1.0) * span * 1e3,
        span, readings.size(), fit->max_residual_ns / 1e6);
  } else {
    std::printf("position never moved\n");
  }
  std::printf("steps back %llu (largest %.3f ms) · runs %llu\n",
              static_cast<unsigned long long>(steps_back), ms(largest_back_ns),
              static_cast<unsigned long long>(out.runs()));
  std::printf("underruns %llu (%.1f ms) · played %llu frames\n",
              static_cast<unsigned long long>(stats.underruns),
              static_cast<double>(stats.underrun_frames) * 1e3 / args->rate,
              static_cast<unsigned long long>(stats.played_frames));
  return fit ? 0 : 1;
}
