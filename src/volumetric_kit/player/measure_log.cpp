// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/measure_log.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "volumetric_kit/core/vulkan/vulkan.hpp"
#include "volumetric_kit/player/playback_stats.hpp"

// cmake/vp_build_info.cmake sets them on this file, and VP_BUILD_TYPE on the
// library.
#ifndef VP_COMMIT_PLAYER
#define VP_COMMIT_PLAYER "unknown"
#endif
#ifndef VP_COMMIT_CORE
#define VP_COMMIT_CORE "unknown"
#endif
#ifndef VP_COMMIT_RECON
#define VP_COMMIT_RECON "unknown"
#endif
#ifndef VP_COMMIT_GFX
#define VP_COMMIT_GFX "unknown"
#endif
#ifndef VP_BUILD_TYPE
#define VP_BUILD_TYPE ""
#endif

namespace volumetric_kit::player {
namespace {

// The format's version, which a reader checks (tools/measure/summarize.py).
constexpr int kFormat = 2;
constexpr const char* kColumns =
    "pass,frame,type,bytes,triangles,table,stage,host_ms,wait_ms,gpu_ms,"
    "device,submits,host_waits,blocked_ns,staging_buffers,dispatches,"
    "barriers,deblock,exact_blocks,rounds,idle_ms,queue_ms,publish_ms,"
    "take_ms,present_ms";
// After a stage row's device: the frame row's 14 columns, empty.
constexpr const char* kNoFigures = ",,,,,,,,,,,,,,";

std::string hex(std::uint32_t value) {
  char text[16];
  std::snprintf(text, sizeof text, "0x%04x", value);
  return text;
}

std::string version(std::uint32_t v) {
  return std::to_string(VK_API_VERSION_MAJOR(v)) + "." +
         std::to_string(VK_API_VERSION_MINOR(v)) + "." +
         std::to_string(VK_API_VERSION_PATCH(v));
}

std::string mib(VkDeviceSize bytes) {
  return std::to_string(bytes / (1024 * 1024)) + " MiB";
}

// The driver's name and version: the driver's own words from Vulkan 1.2 on,
// else its raw version.
std::string driver(const core::Device& device) {
  if (device.caps().api_version() < VK_API_VERSION_1_2) {
    return "version " + hex(device.caps().properties().driverVersion);
  }
  VkPhysicalDeviceDriverProperties properties{};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
  VkPhysicalDeviceProperties2 all{};
  all.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  all.pNext = &properties;
  vkGetPhysicalDeviceProperties2(device.physical_device(), &all);
  return std::string(properties.driverName) + " " + properties.driverInfo;
}

// A value of the header's `# key: value`, on its one line.
std::string one_line(std::string text) {
  std::replace(text.begin(), text.end(), '\n', ' ');
  std::replace(text.begin(), text.end(), '\r', ' ');
  return text;
}

// A CSV field: quoted where it holds a comma or a quote.
std::string field(const char* text) {
  const std::string value = text;
  if (value.find_first_of(",\"") == std::string::npos) {
    return value;
  }
  std::string quoted = "\"";
  for (const char c : value) {
    quoted += c;
    if (c == '"') {
      quoted += '"';
    }
  }
  return quoted + "\"";
}

std::string ms(double value) {
  char text[32];
  std::snprintf(text, sizeof text, "%.4f", value);
  return text;
}

// A figure the frame may lack: empty when it does.
std::string ms(const std::optional<double>& value) {
  return value ? ms(*value) : std::string();
}

const char* name_of(core::DeviceTime device) {
  switch (device) {
    case core::DeviceTime::None:
      return "none";
    case core::DeviceTime::Measured:
      return "measured";
    case core::DeviceTime::NotTimed:
      return "not_timed";
    case core::DeviceTime::Unavailable:
      return "unavailable";
  }
  return "none";
}

}  // namespace

const BuildInfo& build_info() noexcept {
  static const BuildInfo info{
      VP_COMMIT_PLAYER, VP_COMMIT_CORE, VP_COMMIT_RECON, VP_COMMIT_GFX,
      sizeof(VP_BUILD_TYPE) > 1 ? VP_BUILD_TYPE : "none"};
  return info;
}

std::string device_line(const core::Device& device) {
  const VkPhysicalDeviceProperties& properties = device.caps().properties();
  const VkPhysicalDeviceMemoryProperties& memory =
      device.caps().memory_properties();
  std::string line = std::string(properties.deviceName) + "; vendor " +
                     hex(properties.vendorID) + "; driver " + driver(device) +
                     "; Vulkan " + version(device.caps().api_version()) +
                     "; heaps";
  for (std::uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
    const VkMemoryHeap& heap = memory.memoryHeaps[i];
    line +=
        (i == 0 ? " " : ", ") + mib(heap.size) +
        ((heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0 ? " local" : "");
  }
  line += std::string("; local and mapped ") +
          (device.caps().device_mapped_memory() ? "yes" : "no") + ", cached " +
          (device.caps().device_mapped_cached() ? "yes" : "no");
  char timestamps[64];
  std::snprintf(timestamps, sizeof timestamps,
                "; timestamps %u bits, %.4g ns a tick",
                device.timestamp_valid_bits(),
                static_cast<double>(properties.limits.timestampPeriod));
  line += timestamps;
  const core::Result<core::ClockPair> pair = device.calibrate();
  line += pair.ok() ? "; calibrated yes, within " +
                          std::to_string(pair.value().deviation_ns) + " ns"
                    : std::string("; calibrated no");
  return line;
}

std::string rings_line(const core::Device& device) {
  return device.caps().device_mapped_cached() ? "in place" : "copied";
}

std::string utc_text(std::chrono::system_clock::time_point at, bool compact) {
  const std::time_t seconds = std::chrono::system_clock::to_time_t(at);
  std::tm utc{};
  gmtime_r(&seconds, &utc);
  char text[32];
  std::strftime(text, sizeof text,
                compact ? "%Y%m%dT%H%M%SZ" : "%Y-%m-%dT%H:%M:%SZ", &utc);
  return text;
}

std::string measure_file_name(std::chrono::system_clock::time_point started,
                              std::string_view extension) {
  const std::string commit = build_info().player;
  const std::size_t dirty = commit.rfind("-dirty");
  std::string shown = commit.substr(0, std::min<std::size_t>(7, dirty));
  if (dirty != std::string::npos) {
    shown += "-dirty";
  }
  return utc_text(started, /*compact=*/true) + "-" + shown +
         std::string(extension);
}

core::Result<std::unique_ptr<MeasureLog>> MeasureLog::open(
    const std::string& path, const MeasureContext& context) {
  std::unique_ptr<MeasureLog> log(new MeasureLog(path));
  log->out_.open(path + ".part", std::ios::out | std::ios::trunc);
  const BuildInfo& build = build_info();
  log->out_ << "# format: volumetric_kit_player measure " << kFormat << '\n'
            << "# player: " << build.player << '\n'
            << "# core: " << build.core << '\n'
            << "# recon: " << build.recon << '\n'
            << "# gfx: " << build.gfx << '\n'
            << "# build: " << build.build_type << '\n'
            << "# device: " << one_line(context.device) << '\n'
            << "# rings: " << one_line(context.rings) << '\n'
            << "# stream: " << one_line(context.stream) << '\n'
            << "# run: " << one_line(context.run) << '\n'
            << "# timing: " << to_string(context.timing) << '\n'
            << "# tile_depth: " << context.tile_depth << '\n'
            << "# labels: " << (context.debug_labels ? "on" : "off") << '\n'
            << "# utc: " << utc_text(context.started) << '\n';
  if (!context.thermal.empty()) {
    log->out_ << "# thermal: " << one_line(context.thermal) << '\n';
  }
  if (context.low_power) {
    log->out_ << "# low_power: " << (*context.low_power ? "on" : "off") << '\n';
  }
  log->out_ << kColumns << '\n';
  if (!log->out_) {
    return core::Status::io_error("MeasureLog: cannot write " + path + ".part");
  }
  return log;
}

core::Status MeasureLog::add(const FrameStamp& stamp, std::uint32_t triangles,
                             const std::vector<core::StageRow>& render) {
  if (finished_) {
    return core::Status::io_error("MeasureLog: " + path_ + " is finished");
  }
  if (stamp.rebuild) {
    return {};  // the frame before, again
  }
  const std::string frame =
      std::to_string(stamp.pass) + "," + std::to_string(stamp.index) + "," +
      (stamp.key ? "I" : "P") + "," + std::to_string(stamp.bytes) + "," +
      std::to_string(triangles) + ",";
  const auto rows = [&](const char* table,
                        const std::vector<core::StageRow>& of) {
    for (const core::StageRow& row : of) {
      out_ << frame << table << ',' << field(row.name) << ','
           << ms(std::max(0.0, row.cpu_ms - row.wait_ms)) << ','
           << ms(row.wait_ms) << ','
           << (row.device == core::DeviceTime::Measured ? ms(row.gpu_ms) : "")
           << ',' << name_of(row.device) << kNoFigures << '\n';
    }
  };
  rows("worker", stamp.stages.rows());
  rows("render", render);
  const core::DeviceCounters& counted = stamp.counters;
  const FrameFigures figures = frame_figures(stamp);
  out_ << frame << ",,,,,," << counted.submits << ',' << counted.host_waits
       << ',' << counted.blocked_ns << ',' << counted.staging_buffers << ','
       << counted.dispatches << ',' << counted.barriers << ','
       << field(stamp.deblock_on ? stamp.deblock.filter : "off") << ','
       << stamp.deblock.exact_blocks << ',' << stamp.rounds << ','
       << ms(figures.device_idle_ms) << ',' << ms(figures.queue_ms) << ','
       << ms(figures.publish_ms) << ',' << ms(figures.take_ms) << ','
       << ms(figures.present_ms) << '\n';
  if (!out_) {
    return core::Status::io_error("MeasureLog: cannot write " + path_ +
                                  ".part");
  }
  ++frames_;
  return {};
}

core::Status MeasureLog::finish() {
  if (finished_) {
    return core::Status::io_error("MeasureLog: " + path_ + " is finished");
  }
  finished_ = true;
  out_.close();
  if (!out_) {
    return core::Status::io_error("MeasureLog: cannot write " + path_ +
                                  ".part");
  }
  if (std::rename((path_ + ".part").c_str(), path_.c_str()) != 0) {
    return core::Status::io_error("MeasureLog: cannot rename " + path_ +
                                  ".part to " + path_);
  }
  return {};
}

}  // namespace volumetric_kit::player
