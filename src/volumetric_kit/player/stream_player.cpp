// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/stream_player.hpp"

#include <chrono>
#include <utility>

#include "volumetric_kit/core/base/exception_status.hpp"
#include "volumetric_kit/player/command_line.hpp"
#include "volumetric_kit/recon/codec/decode_grid.hpp"
#include "volumetric_kit/recon/stream/session.hpp"

namespace volumetric_kit::player {
namespace {

namespace codec = recon::codec;
namespace networking = recon::networking;
namespace stream = recon::stream;
using Clock = Playback::Clock;

// Live: the longest one receive, or one wait for the consumer's take, lasts,
// so quit and a park are seen within it; the least time between two meshes
// while records keep coming; and the wait before a connection is made again.
constexpr std::chrono::milliseconds kSlice{10};
constexpr std::chrono::milliseconds kMeshEvery{100};
constexpr std::chrono::seconds kRetry{1};

// A record or frame to skip, rather than an error that stops the worker:
// damage (InvalidArgument), what this version cannot read (Unsupported), and
// a P-frame without its reference (NotFound).
bool skippable(const core::Status& status) {
  return status.domain() == core::Status::Code::InvalidArgument ||
         status.domain() == core::Status::Code::Unsupported ||
         status.domain() == core::Status::Code::NotFound;
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

// The index of `session`'s first geometry track; none without one.
std::optional<std::uint16_t> first_geometry_track(
    const stream::Session& session) {
  for (std::size_t i = 0; i < session.tracks.size(); ++i) {
    if (session.tracks[i].kind == stream::TrackKind::Geometry) {
      return static_cast<std::uint16_t>(i);
    }
  }
  return std::nullopt;
}

}  // namespace

core::Result<LiveAddress> parse_live_address(std::string_view text) {
  const std::size_t colon = text.find(':');
  LiveAddress address;
  address.host = std::string(text.substr(0, colon));
  if (address.host.empty()) {
    return core::Status::invalid_argument("'" + std::string(text) +
                                          "' names no host");
  }
  if (colon == std::string_view::npos) {
    return address;
  }
  const std::optional<std::uint64_t> port = parse_count(text.substr(colon + 1));
  if (!port || *port > 65535) {
    return core::Status::invalid_argument(
        "'" + std::string(text) + "': the port is a number from 1 to 65535");
  }
  address.port = static_cast<std::uint16_t>(*port);
  return address;
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

// Constructed at a step's start: waits while parked, or with kNoWait does not
// begin then, and marks the worker busy until end() or the destructor. False
// on quit, or parked with kNoWait.
class StreamPlayer::Step {
 public:
  enum Wait { kWhileParked, kNoWait };

  explicit Step(StreamPlayer& player, Wait wait = kWhileParked)
      : player_(player) {
    std::unique_lock<std::mutex> lock(player_.mutex_);
    if (wait == kWhileParked) {
      player_.changed_.wait(
          lock, [this] { return player_.quit_ || !player_.parked_; });
    }
    begun_ = !player_.quit_ && !player_.parked_;
    player_.busy_ = begun_;
  }
  ~Step() { end(); }
  Step(const Step&) = delete;
  Step& operator=(const Step&) = delete;

  explicit operator bool() const { return begun_; }

  // Idle again, before a wait that is no step; wakes a park.
  void end() {
    if (!begun_) {
      return;
    }
    begun_ = false;
    {
      std::lock_guard<std::mutex> lock(player_.mutex_);
      player_.busy_ = false;
    }
    player_.changed_.notify_all();
  }

 private:
  StreamPlayer& player_;
  bool begun_ = false;
};

core::Result<std::unique_ptr<StreamPlayer>> StreamPlayer::open(
    const std::string& path) {
  try {
    VKC_ASSIGN(std::unique_ptr<stream::StreamReader> reader,
               stream::StreamReader::open(path));
    const std::optional<std::uint16_t> track =
        first_geometry_track(reader->session());
    if (!track || reader->stats()[*track].records == 0) {
      return core::Status::not_found(path + ": no geometry to play");
    }
    return std::unique_ptr<StreamPlayer>(
        new StreamPlayer(std::move(reader), *track));
  } catch (...) {
    return core::exception_status("StreamPlayer::open",
                                  core::Status::Code::IoError);
  }
}

core::Result<std::unique_ptr<StreamPlayer>> StreamPlayer::connect(
    LiveAddress address) {
  if (address.host.empty() || address.port == 0) {
    return core::Status::invalid_argument(
        "StreamPlayer::connect needs a host and a port from 1: '" +
        to_string(address) + "'");
  }
  try {
    return std::unique_ptr<StreamPlayer>(new StreamPlayer(std::move(address)));
  } catch (...) {
    return core::exception_status("StreamPlayer::connect",
                                  core::Status::Code::IoError);
  }
}

StreamPlayer::StreamPlayer(std::unique_ptr<stream::StreamReader> reader,
                           std::uint16_t track)
    : reader_(std::move(reader)),
      track_(track),
      clock_(reader_->session().tracks[track].rate.num,
             reader_->session().tracks[track].rate.den) {}

StreamPlayer::StreamPlayer(LiveAddress address)
    : address_(std::move(address)), clock_(0, 0) {
  live_status_.address = to_string(address_);
}

StreamPlayer::~StreamPlayer() { stop(); }

std::uint64_t StreamPlayer::frame_count() const noexcept {
  return live() ? 0 : reader_->stats()[track_].records;
}

const stream::TrackStats& StreamPlayer::track_stats() const noexcept {
  static const stream::TrackStats kNone;
  return live() ? kNone : reader_->stats()[track_];
}

stream::Rate StreamPlayer::rate() const noexcept {
  return live() ? stream::Rate{} : reader_->session().tracks[track_].rate;
}

LiveStatus StreamPlayer::live_status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return live_status_;
}

core::Status StreamPlayer::start(core::Device& device,
                                 core::Allocator& allocator,
                                 const recon::mesh::MarchingCubesConfig& config,
                                 FrameExchange& exchange) {
  if (worker_.joinable() || quit_) {
    return core::Status::invalid_argument(
        "StreamPlayer::start: already started or stopped");
  }
  device_ = &device;
  allocator_ = &allocator;
  config_ = config;
  exchange_ = &exchange;
  try {
    worker_ = std::thread([this] { run(); });
  } catch (...) {
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

// Live, the playback controls do nothing: the sender paces.
void StreamPlayer::play() {
  if (!live()) {
    control([this] { clock_.play(); });
  }
}

void StreamPlayer::pause() {
  if (!live()) {
    control([this] { clock_.pause(); });
  }
}

void StreamPlayer::step() {
  if (!live()) {
    control([this] { clock_.step(); });
  }
}

void StreamPlayer::set_loop(bool on) {
  if (!live()) {
    control([this, on] { loop_ = on; });
  }
}

bool StreamPlayer::playing() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return clock_.playing();
}

bool StreamPlayer::loop() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return loop_;
}

bool StreamPlayer::park(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  parked_ = true;
  changed_.notify_all();  // ends a wait to retry
  return changed_.wait_for(lock, timeout, [this] { return !busy_; });
}

void StreamPlayer::unpark() {
  control([this] { parked_ = false; });
}

void StreamPlayer::taken() {
  control([] {});
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
    outcome = build();
    if (outcome.ok()) {
      outcome = live() ? play_live() : play_track();
    }
  } catch (...) {
    outcome = core::exception_status("StreamPlayer worker",
                                     core::Status::Code::IoError);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  status_ = std::move(outcome);
}

core::Status StreamPlayer::build() {
  const Step step(*this);
  if (!step) {
    return {};  // quit: play_track returns at its first step, play_live at once
  }
  VKC_ASSIGN(codec::Decoder decoder,
             codec::Decoder::create(*device_, *allocator_));
  VKC_ASSIGN(
      recon::mesh::MarchingCubes extractor,
      recon::mesh::MarchingCubes::create(*device_, *allocator_, config_));
  decoder_.emplace(std::move(decoder));
  extractor_.emplace(std::move(extractor));
  return {};
}

core::Status StreamPlayer::play_track() {
  std::vector<std::uint8_t> payload;
  for (;;) {
    stream::Position pos = reader_->begin();
    std::uint64_t index = 0;  // the next record's place in this pass
    bool shown = false;       // a frame was shown in this pass
    core::Status first_skip;  // the first record or frame it skipped
    // The next frame's stamp's rows, which other tracks' records add to.
    core::StageMetrics stages;
    for (;;) {
      Step step(*this);  // the record's read and decode
      if (!step) {
        return {};
      }
      stream::Record record;
      const core::Result<bool> more = [&] {
        const core::StageScope read(stages, "read");
        return reader_->next(&pos, &record);
      }();
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
        const core::StageScope read(stages, "read");
        status = reader_->read(record, &payload);
      }
      if (status.ok()) {
        status = decode(payload, stages);
      }
      if (!status.ok()) {
        if (!skippable(status)) {
          return status;
        }
        if (first_skip.ok()) {
          first_skip = std::move(status);
        }
        stages.clear();
        continue;
      }
      step.end();
      if (!wait_due(record.info.pts_ns)) {
        return {};
      }
      VKC_ASSIGN(
          const bool published,
          publish({frame, record.info.pts_ns, record.size, record.info.key,
                   std::exchange(stages, {}), 1, record.size}));
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

core::Status StreamPlayer::play_live() {
  std::vector<std::uint8_t> payload;
  while (wait_unparked()) {
    // A new grid each connection, the decoder kept (DECISIONS.md).
    grid_.reset();
    core::Status ended;
    {
      core::Result<std::unique_ptr<networking::StreamClient>> client =
          networking::StreamClient::connect(address_.host, address_.port);
      if (client.ok()) {
        VKC_TRY(play_connection(**client, payload, &ended));
      } else {
        ended = client.status();
      }
    }  // closed at once
    if (!wait_to_retry(ended)) {
      break;
    }
  }
  return {};
}

core::Status StreamPlayer::play_connection(networking::StreamClient& client,
                                           std::vector<std::uint8_t>& payload,
                                           core::Status* ended) {
  std::optional<std::uint16_t> track;  // the session's, once it is in
  bool pending = false;                // decoded, not yet meshed
  FrameStamp newest;                   // its stamp
  std::uint64_t records = 0;           // received since the last publish
  std::uint64_t record_bytes = 0;
  std::optional<Clock::time_point> published;  // the last, on this connection
  stream::Record record;
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (quit_ || parked_) {
        return {};
      }
    }
    // Drain before waiting for a take. Even without a whole record, a
    // partial receive is progress: another nonblocking read may advance it.
    bool progressed = false;
    const core::Result<bool> got =
        client.next(pending ? std::chrono::milliseconds{0} : kSlice, &record,
                    &payload, &progressed);
    if (!got.ok()) {
      *ended = got.status();
      return {};
    }
    if (!track && client.session() != nullptr) {
      track = first_geometry_track(*client.session());
      if (!track) {
        *ended = core::Status::not_found(
            "the sender's session has no geometry track");
        return {};
      }
      std::lock_guard<std::mutex> lock(mutex_);
      live_status_.state = LiveState::kConnected;
    }
    if (*got && record.info.track == *track) {
      ++records;
      record_bytes += record.size;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        live_status_.state = LiveState::kLive;
        live_status_.seq = record.info.seq;
        live_status_.last_record = Clock::now();
      }
      const Step step(*this, Step::kNoWait);  // the decode
      if (!step) {
        return {};  // quit or parked: the record is dropped
      }
      core::StageMetrics stages;
      const core::Status decoded = decode(payload, stages);
      if (decoded.ok()) {
        newest = {record.info.seq, record.info.pts_ns, record.size,
                  record.info.key, std::move(stages)};
        pending = true;
      } else if (!skippable(decoded)) {
        return decoded;
      }
      // TODO: show why records are skipped while none of this connection's
      // has decoded: a sender this version cannot decode shows as live, with
      // no picture.
    }
    // Free: the consumer took the last mesh, so the newest frame may be
    // meshed. Never bypassed: a superseded mesh keeps its ring slot. Wait
    // for taken() only after input is drained, so queued records and partial
    // receives do not each pay a slice while a mesh is left untaken.
    bool free = false;
    if (pending) {
      std::unique_lock<std::mutex> lock(mutex_);
      if (progressed) {
        free = exchange_->release_and_may_publish(*extractor_);
      } else {
        free = changed_.wait_for(lock, kSlice, [this] {
          return quit_ || parked_ ||
                 exchange_->release_and_may_publish(*extractor_);
        });
      }
      if (quit_ || parked_) {
        return {};
      }
    }
    // The newest: once nothing more has come, at once for the connection's
    // first frame, or every kMeshEvery while records keep coming
    // (DECISIONS.md).
    if (free && (!progressed || !published ||
                 Clock::now() - *published >= kMeshEvery)) {
      const Step step(*this, Step::kNoWait);  // the extract
      if (!step) {
        return {};
      }
      newest.records = std::exchange(records, 0);
      newest.record_bytes = std::exchange(record_bytes, 0);
      VKC_TRY(mesh_and_publish(std::move(newest)));
      pending = false;
      published = Clock::now();
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

core::Status StreamPlayer::decode(const std::vector<std::uint8_t>& payload,
                                  core::StageMetrics& stages) {
  core::StageMetrics decoded;  // recon's rows, for the device's half
  {
    const core::StageScope decode(stages, "decode");
    if (!grid_.has_value()) {
      VKC_TRY(make_grid(payload));
    }
    VKC_TRY(codec::decode_growing(*decoder_, payload.data(), payload.size(),
                                  *grid_, &decoded));
  }
  // "codec decode" alone has a device span; its breakdowns are host time.
  for (const core::StageRow& row : decoded.rows()) {
    if (row.has_gpu) {
      stages.add_gpu("decode", row.gpu_ms);
    }
  }
  return {};
}

bool StreamPlayer::wait_due(std::uint64_t pts_ns) {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    if (quit_) {
      return false;
    }
    const Clock::time_point now = Clock::now();
    // Parked, nothing is due: a control takes effect at unpark.
    const std::optional<Clock::time_point> due =
        parked_ ? std::nullopt : clock_.due(pts_ns, now);
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

core::Result<bool> StreamPlayer::publish(FrameStamp stamp) {
  {
    // Asleep until taken() or another control wakes it.
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [this] {
      return quit_ || exchange_->release_and_may_publish(*extractor_);
    });
    if (quit_) {
      return false;
    }
  }
  const Step step(*this);  // the extract
  if (!step) {
    return false;
  }
  VKC_TRY(mesh_and_publish(std::move(stamp)));
  return true;
}

core::Status StreamPlayer::mesh_and_publish(FrameStamp stamp) {
  core::Result<recon::mesh::DeviceMesh> mesh = [&] {
    const core::StageScope extract(stamp.stages, "mesh");
    return extractor_->extract_device(*grid_);
  }();
  VKC_TRY(mesh.status());
  stamp.published = Clock::now();
  exchange_->publish(mesh.value(), std::move(stamp));
  return {};
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

bool StreamPlayer::wait_unparked() {
  std::unique_lock<std::mutex> lock(mutex_);
  changed_.wait(lock, [this] { return quit_ || !parked_; });
  return !quit_;
}

bool StreamPlayer::wait_to_retry(const core::Status& ended) {
  std::unique_lock<std::mutex> lock(mutex_);
  live_status_.state = LiveState::kConnecting;
  live_status_.last_error =
      ended.ok() ? std::string() : live_reason(ended, live_status_.address);
  if (!ended.ok()) {
    changed_.wait_for(lock, kRetry, [this] { return quit_ || parked_; });
  }
  return !quit_;
}

}  // namespace volumetric_kit::player
