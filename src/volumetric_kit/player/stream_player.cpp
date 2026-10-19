// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/stream_player.hpp"

#include <chrono>
#include <utility>

#include "volumetric_kit/core/base/exception_status.hpp"
#include "volumetric_kit/recon/codec/decode_grid.hpp"
#include "volumetric_kit/recon/stream/session.hpp"

namespace volumetric_kit::player {
namespace {

namespace codec = recon::codec;
namespace stream = recon::stream;
using Clock = Playback::Clock;

// How long one wait for the consumer to take a mesh lasts before the worker
// waits again; quit ends it at once.
constexpr std::chrono::seconds kCollectWait{1};

// A record or frame to skip, rather than an error that stops the worker:
// damage (InvalidArgument), what this version cannot read (Unsupported), and
// a P-frame without its reference (NotFound).
bool skippable(const core::Status& status) {
  return status.domain() == core::Status::Code::InvalidArgument ||
         status.domain() == core::Status::Code::Unsupported ||
         status.domain() == core::Status::Code::NotFound;
}

double ms_since(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}

// The error that ends a pass of the track showing no frame, with the first
// refusal in it.
core::Status nothing_shown(core::Status first_skip) {
  constexpr const char* kWhat =
      "StreamPlayer: no frame of the geometry track decodes";
  if (first_skip.ok()) {
    return core::Status::not_found(kWhat);
  }
  return std::move(first_skip).with_context(kWhat);
}

}  // namespace

core::Result<std::unique_ptr<StreamPlayer>> StreamPlayer::open(
    const std::string& path) {
  try {
    VKC_ASSIGN(std::unique_ptr<stream::StreamReader> reader,
               stream::StreamReader::open(path));
    const std::vector<stream::Track>& tracks = reader->session().tracks;
    std::size_t track = 0;
    while (track < tracks.size() &&
           tracks[track].kind != stream::TrackKind::Geometry) {
      ++track;
    }
    if (track == tracks.size() || reader->stats()[track].records == 0) {
      return core::Status::not_found(path + ": no geometry to play");
    }
    return std::unique_ptr<StreamPlayer>(
        new StreamPlayer(std::move(reader), static_cast<std::uint16_t>(track)));
  } catch (...) {
    return core::exception_status("StreamPlayer::open",
                                  core::Status::Code::IoError);
  }
}

StreamPlayer::StreamPlayer(std::unique_ptr<stream::StreamReader> reader,
                           std::uint16_t track)
    : reader_(std::move(reader)),
      track_(track),
      clock_(reader_->session().tracks[track].rate.num,
             reader_->session().tracks[track].rate.den) {}

StreamPlayer::~StreamPlayer() { stop(); }

std::uint64_t StreamPlayer::frame_count() const noexcept {
  return reader_->stats()[track_].records;
}

core::Status StreamPlayer::start(core::Device& device,
                                 core::Allocator& allocator,
                                 const recon::mesh::MarchingCubesConfig& config,
                                 FrameExchange& exchange) {
  if (decoder_.has_value() || quit_) {
    return core::Status::invalid_argument(
        "StreamPlayer::start: already started or stopped");
  }
  VKC_ASSIGN(codec::Decoder decoder, codec::Decoder::create(device, allocator));
  VKC_ASSIGN(recon::mesh::MarchingCubes extractor,
             recon::mesh::MarchingCubes::create(device, allocator, config));
  decoder_.emplace(std::move(decoder));
  extractor_.emplace(std::move(extractor));
  device_ = &device;
  allocator_ = &allocator;
  exchange_ = &exchange;
  try {
    worker_ = std::thread([this] { run(); });
  } catch (...) {
    extractor_.reset();
    decoder_.reset();
    return core::exception_status("StreamPlayer::start",
                                  core::Status::Code::IoError);
  }
  return {};
}

template <typename Change>
void StreamPlayer::control(Change&& change) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    change();
  }
  changed_.notify_all();
}

void StreamPlayer::play() {
  control([this] { clock_.play(); });
}

void StreamPlayer::pause() {
  control([this] { clock_.pause(); });
}

void StreamPlayer::step() {
  control([this] { clock_.step(); });
}

void StreamPlayer::set_loop(bool on) {
  control([this, on] { loop_ = on; });
}

bool StreamPlayer::playing() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return clock_.playing();
}

bool StreamPlayer::loop() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return loop_;
}

void StreamPlayer::stop() {
  control([this] { quit_ = true; });
  if (worker_.joinable()) {
    worker_.join();
  }
}

core::Status StreamPlayer::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

void StreamPlayer::run() {
  core::Status outcome;
  try {
    outcome = play_track();
  } catch (...) {
    outcome = core::exception_status("StreamPlayer worker",
                                     core::Status::Code::IoError);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  status_ = std::move(outcome);
}

core::Status StreamPlayer::play_track() {
  std::vector<std::uint8_t> payload;
  for (;;) {
    stream::Position pos = reader_->begin();
    std::uint64_t index = 0;  // the next record's place in this pass
    bool shown = false;       // a frame was shown in this pass
    core::Status first_skip;  // the first record or frame it skipped
    for (;;) {
      if (quit_) {
        return {};
      }
      stream::Record record;
      const core::Result<bool> more = reader_->next(&pos, &record);
      if (more.ok() && !more.value()) {
        break;  // the end
      }
      if (more.ok() && record.info.track != track_) {
        continue;
      }
      // A damaged chunk header leaves `pos` past the damage.
      core::Status status = more.status();
      const std::uint64_t frame = index;
      if (status.ok()) {
        ++index;
        status = reader_->read(record, &payload);
      }
      if (status.ok() && !grid_.has_value()) {
        status = make_grid(payload);
      }
      const Clock::time_point start = Clock::now();
      if (status.ok()) {
        status = codec::decode_growing(*decoder_, payload.data(),
                                       payload.size(), *grid_);
      }
      if (!status.ok()) {
        if (!skippable(status)) {
          return status;
        }
        if (first_skip.ok()) {
          first_skip = std::move(status);
        }
        continue;
      }
      const double decode_ms = ms_since(start);
      if (!wait_due(record.info.pts_ns)) {
        return {};
      }
      VKC_ASSIGN(const bool published,
                 publish({frame, record.info.pts_ns, decode_ms}));
      if (!published) {
        return {};
      }
      shown = true;
    }
    if (!shown) {
      return nothing_shown(std::move(first_skip));
    }
    if (!wait_at_end()) {
      return {};
    }
  }
}

core::Status StreamPlayer::make_grid(const std::vector<std::uint8_t>& payload) {
  VKC_ASSIGN(recon::volume::VoxelBlockGrid grid,
             codec::make_decode_grid(*device_, *allocator_, payload.data(),
                                     payload.size()));
  grid_.emplace(std::move(grid));
  return {};
}

bool StreamPlayer::wait_due(std::uint64_t pts_ns) {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    if (quit_) {
      return false;
    }
    const Clock::time_point now = Clock::now();
    const std::optional<Clock::time_point> due = clock_.due(pts_ns, now);
    if (due && *due <= now) {
      break;
    }
    if (due) {
      changed_.wait_until(lock, *due);
    } else {
      changed_.wait(lock);
    }
  }
  last_by_step_ = !clock_.playing();
  clock_.shown(pts_ns, Clock::now());
  last_pts_ns_ = pts_ns;
  return true;
}

core::Result<bool> StreamPlayer::publish(const FrameStamp& stamp) {
  while (!exchange_->release_and_may_publish(*extractor_)) {
    exchange_->wait_collected(kCollectWait, quit_);
    if (quit_) {
      return false;
    }
  }
  const Clock::time_point start = Clock::now();
  VKC_ASSIGN(const recon::mesh::DeviceMesh mesh,
             extractor_->extract_device(*grid_));
  exchange_->publish(mesh, {stamp.index, stamp.pts_ns,
                            stamp.decode_mesh_ms + ms_since(start)});
  return true;
}

bool StreamPlayer::wait_at_end() {
  std::unique_lock<std::mutex> lock(mutex_);
  // With loop off the last frame holds: played into, it pauses there; stepped
  // onto, or paused on, it is held already, so a play, even one since that
  // step, restarts.
  if (!loop_ && clock_.playing() && !last_by_step_) {
    clock_.pause();
  }
  for (;;) {
    // Until a frame may go: playing, or a step.
    changed_.wait(lock, [this] {
      return quit_ || clock_.due(last_pts_ns_, Clock::now()).has_value();
    });
    if (quit_) {
      return false;
    }
    if (loop_ || clock_.playing()) {
      return true;
    }
    clock_.shown(last_pts_ns_, Clock::now());  // a step: the held frame
  }
}

}  // namespace volumetric_kit::player
