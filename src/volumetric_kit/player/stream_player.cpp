// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/stream_player.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <utility>

#include "volumetric_kit/core/base/exception_status.hpp"
#include "volumetric_kit/core/vulkan/host_ring.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#include "volumetric_kit/grid/codec/decode_grid.hpp"
#include "volumetric_kit/grid/stream/session.hpp"
#include "volumetric_kit/player/frame_hooks.hpp"

namespace volumetric_kit::player {
namespace {

namespace codec = grid::codec;
namespace mesh = grid::mesh;
namespace networking = grid::networking;
namespace stream = grid::stream;
using Clock = Playback::Clock;

// Live: the longest one receive, or one wait for the consumer's take, lasts,
// so quit and a park are seen within it; and the least time between two
// meshes while records keep coming.
constexpr std::chrono::milliseconds kSlice{10};
constexpr std::chrono::milliseconds kMeshEvery{100};

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

// `to` the host and wait halves of the rows of `from`, grid's, that are
// breakdowns: its stage's own row is the player's, open around the call. A
// breakdown's device half comes from the batch (StreamPlayer::note_batch),
// or, for a blocking recovery, with its row.
void forward_breakdowns(const core::StageMetrics& from,
                        core::StageMetrics& to) {
  const std::size_t prefix = std::strlen(core::StageMetrics::kBreakdownPrefix);
  for (const core::StageRow& row : from.rows()) {
    if (std::strncmp(row.name, core::StageMetrics::kBreakdownPrefix, prefix) !=
        0) {
      continue;
    }
    to.add_cpu(row.name, row.cpu_ms);
    to.add_wait(row.name, row.wait_ms);
    to.add_device(row.name, row.device, row.gpu_ms);
  }
}

// What a device did between two of its counts.
core::DeviceCounters since(const core::DeviceCounters& now,
                           const core::DeviceCounters& then) {
  core::DeviceCounters done;
  done.submits = now.submits - then.submits;
  done.host_waits = now.host_waits - then.host_waits;
  done.blocked_ns = now.blocked_ns - then.blocked_ns;
  done.staging_buffers = now.staging_buffers - then.staging_buffers;
  done.dispatches = now.dispatches - then.dispatches;
  done.barriers = now.barriers - then.barriers;
  return done;
}

// `at` in nanoseconds since its clock's epoch, as core::DeviceSpan states a
// host time.
std::int64_t steady_ns(Clock::time_point at) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             at.time_since_epoch())
      .count();
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

const char* to_string(DeviceTiming timing) noexcept {
  switch (timing) {
    case DeviceTiming::kOff:
      return "off";
    case DeviceTiming::kSampled:
      return "sampled";
    case DeviceTiming::kAll:
      return "all";
  }
  return "off";
}

// Begun at a step's start: waits while parked, or with kNoWait does not
// begin then, and counts the worker busy until end(), its destructor, or the
// end of the step it is moved into. False on quit, or parked with kNoWait.
class StreamPlayer::Step {
 public:
  enum Wait { kWhileParked, kNoWait };

  Step() noexcept = default;
  explicit Step(StreamPlayer& player, Wait wait = kWhileParked) {
    std::unique_lock<std::mutex> lock(player.mutex_);
    if (wait == kWhileParked) {
      player.changed_.wait(
          lock, [&player] { return player.quit_ || !player.parked_; });
    }
    if (!player.quit_ && !player.parked_) {
      ++player.busy_;
      player_ = &player;
    }
  }
  Step(Step&& other) noexcept
      : player_(std::exchange(other.player_, nullptr)) {}
  Step& operator=(Step&& other) noexcept {
    if (this != &other) {
      end();
      player_ = std::exchange(other.player_, nullptr);
    }
    return *this;
  }
  ~Step() { end(); }
  Step(const Step&) = delete;
  Step& operator=(const Step&) = delete;

  explicit operator bool() const { return player_ != nullptr; }

  // Idle again, before a wait that is no step; wakes a park.
  void end() {
    StreamPlayer* const player = std::exchange(player_, nullptr);
    if (player == nullptr) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(player->mutex_);
      --player->busy_;
    }
    player->changed_.notify_all();
  }

 private:
  StreamPlayer* player_ = nullptr;  // set while begun
};

struct StreamPlayer::Pass {
  explicit Pass(std::uint64_t pass) : number(pass) {}

  // Skip what `status` refused, keeping the first refusal, and drop the next
  // frame's rows so far; an error that is no refusal is returned.
  core::Status skip(core::Status status) {
    if (!skippable(status)) {
      return status;
    }
    if (first_skip.ok()) {
      first_skip = std::move(status);
    }
    stages.clear();
    return {};
  }

  std::uint64_t number;       // from 1
  std::uint64_t index = 0;    // the next record's place in this pass
  bool shown = false;         // a frame was shown in this pass
  core::Status first_skip;    // the first record or frame it skipped
  core::StageMetrics stages;  // the next frame's: reads, other tracks' too
};

struct StreamPlayer::Next {
  core::HostSlot slot;  // the record, at its start
  std::size_t size = 0;
  FrameStamp stamp;
};

struct StreamPlayer::Batch {
  core::PendingBatch pending;
  std::uint64_t submits_before = 0;  // the device's, just before it
  std::int64_t submitted_ns = 0;     // the submit call, on steady_clock
};

struct StreamPlayer::InFlight {
  FrameStamp stamp;
  codec::PendingDecode decode;                  // holds its input slot
  std::optional<mesh::PendingExtract> extract;  // none live
  Step step;                                    // what a park waits for
  Batch batch;  // last, so destroyed first: a quit waits for the GPU
};

core::Result<std::unique_ptr<StreamPlayer>> StreamPlayer::open(
    const std::string& path, AudioOutMaker make_audio) {
  try {
    VKC_ASSIGN(std::unique_ptr<stream::StreamReader> reader,
               stream::StreamReader::open(path));
    const std::optional<std::uint16_t> track =
        first_geometry_track(reader->session());
    if (!track || reader->stats()[*track].records == 0) {
      return core::Status::not_found(path + ": no geometry to play");
    }
    std::optional<AudioTrackChoice> audio =
        find_audio_track(reader->session(), reader->stats(), *track);
    return std::unique_ptr<StreamPlayer>(new StreamPlayer(
        std::move(reader), *track, std::move(audio), std::move(make_audio)));
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
                           std::uint16_t track,
                           std::optional<AudioTrackChoice> audio,
                           AudioOutMaker make_audio)
    : reader_(std::move(reader)),
      track_(track),
      audio_track_(std::move(audio)),
      make_audio_(std::move(make_audio)),
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
                                 const grid::mesh::MarchingCubesConfig& config,
                                 FrameExchange& exchange,
                                 const FrameHooks* hooks) {
  if (worker_.joinable() || quit_) {
    return core::Status::invalid_argument(
        "StreamPlayer::start: already started or stopped");
  }
  device_ = &device;
  allocator_ = &allocator;
  config_ = config;
  exchange_ = &exchange;
  hooks_ = hooks;
  if (audio_track_) {
    VKC_TRY(start_audio());
  }
  try {
    worker_ = std::thread([this] { run(); });
  } catch (...) {
    core::Status failed = core::exception_status("StreamPlayer::start",
                                                 core::Status::Code::IoError);
    drop_audio();  // a start again makes its own
    return failed;
  }
  return {};
}

core::Status StreamPlayer::start_audio() {
  const AudioFormat format = played_format(audio_track_->params);
  std::unique_ptr<AudioOut> out;
  try {
    if (make_audio_) {
      VKC_ASSIGN(out, make_audio_(format));
    } else {
      VKC_ASSIGN(out, NullAudioOut::create(format));
    }
  } catch (...) {
    return core::exception_status("StreamPlayer's audio output",
                                  core::Status::Code::IoError);
  }
  if (out == nullptr) {
    return core::Status::invalid_argument(
        "StreamPlayer: the audio output maker made none");
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    out->set_gain(muted_ ? 0.0f : 1.0f);
    if (clock_.playing()) {
      out->play();
    }
    audio_ = std::move(out);
  }
  // Audio starts at the first geometry pts (DECISIONS.md).
  core::Result<std::unique_ptr<AudioFeeder>> feeder = AudioFeeder::start(
      *reader_, *audio_track_, reader_->stats()[track_].min_pts_ns, *audio_);
  if (!feeder.ok()) {
    drop_audio();
    return std::move(feeder).status();
  }
  feeder_ = std::move(feeder).value();
  return {};
}

void StreamPlayer::drop_audio() {
  feeder_.reset();  // joined first: it writes into the output
  std::lock_guard<std::mutex> lock(mutex_);
  audio_.reset();
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
    control([this] {
      clock_.play();
      if (audio_) {
        audio_->play();
      }
    });
  }
}

void StreamPlayer::pause() {
  if (!live()) {
    control([this] {
      clock_.pause();
      if (audio_) {
        audio_->pause();
      }
    });
  }
}

// A step is the frames' alone: the audio pauses.
void StreamPlayer::step() {
  if (!live()) {
    control([this] {
      clock_.step();
      if (audio_) {
        audio_->pause();
      }
    });
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

void StreamPlayer::set_muted(bool on) {
  control([this, on] {
    muted_ = on;
    if (audio_) {
      audio_->set_gain(on ? 0.0f : 1.0f);
    }
  });
}

bool StreamPlayer::muted() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return muted_;
}

std::optional<std::uint64_t> StreamPlayer::audio_position() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return audio_ ? audio_->position() : std::nullopt;
}

std::optional<AudioStatus> StreamPlayer::audio_status() const {
  if (!audio_track_) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  AudioStatus status;
  status.muted = muted_;
  if (audio_) {
    status.route = audio_->route();
  }
  return status;
}

bool StreamPlayer::follow_audio() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (audio_ == nullptr || !audio_->take_pause_request()) {
    return false;
  }
  clock_.pause();
  audio_->pause();
  changed_.notify_all();
  return true;
}

// Live too: the decoder takes it at the next record.
void StreamPlayer::set_deblock(bool on) {
  control([this, on] { deblock_ = on; });
}

bool StreamPlayer::deblock() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return deblock_;
}

// A device output stops at a park and starts at the unpark; the null output
// and the feeder run on through it.
bool StreamPlayer::park(std::chrono::milliseconds timeout) {
  const Clock::time_point deadline = Clock::now() + timeout;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    parked_ = true;
    if (audio_) {
      audio_->park();
    }
    changed_.notify_all();  // ends a wait to retry, and wakes a frame's pacing
    if (!changed_.wait_until(lock, deadline, [this] { return busy_ == 0; })) {
      return false;
    }
  }
  // Idle, so no step starts: after_mesh's batch is the park's to finish, by
  // the same deadline.
  if (!finish_hook_batch_by(deadline)) {
    return false;
  }
  if (hooks_ != nullptr && hooks_->on_park && !hooks_parked_.exchange(true)) {
    hooks_->on_park();
  }
  return true;
}

void StreamPlayer::unpark() {
  if (hooks_parked_.exchange(false) && hooks_->on_resume) {
    hooks_->on_resume();
  }
  control([this] {
    if (parked_ && audio_) {
      audio_->unpark();
    }
    parked_ = false;
  });
}

void StreamPlayer::taken() {
  control([] {});
}

void StreamPlayer::stop() {
  control([this] {
    quit_ = true;
    if (audio_) {
      audio_->pause();
    }
  });
  if (worker_.joinable()) {
    worker_.join();
  }
  if (feeder_) {
    feeder_->stop();
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
  // Before stop returns, so its readbacks are written.
  core::Status finished = finish_hook_batch();
  if (outcome.ok()) {
    outcome = std::move(finished);
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
  VKC_ASSIGN(mesh::MarchingCubes extractor,
             mesh::MarchingCubes::create(*device_, *allocator_, config_));
  VKC_ASSIGN(core::GpuTimer timer, core::GpuTimer::create(*device_));
  if (hooks_ != nullptr && hooks_->after_mesh) {
    VKC_ASSIGN(core::TimelineSemaphore timeline,
               core::TimelineSemaphore::create(*device_));
    hook_timeline_.emplace(std::move(timeline));
  }
  decoder_.emplace(std::move(decoder));
  extractor_.emplace(std::move(extractor));
  timer_.emplace(std::move(timer));
  counted_ = device_->counters();  // the first frame's from here
  return {};
}

bool StreamPlayer::time_next_frame() {
  const std::uint64_t frame = frames_started_++;
  switch (device_timing()) {
    case DeviceTiming::kOff:
      return false;
    case DeviceTiming::kSampled:
      return frame % kSampleEvery == 0;
    case DeviceTiming::kAll:
      return true;
  }
  return false;
}

core::CommandBatch StreamPlayer::new_batch(bool timed) {
  core::CommandBatch batch(*device_, *allocator_);
  if (timed) {
    timer_->set_max_depth(tile_depth());
    batch.time(*timer_);
  }
  return batch;
}

core::Status StreamPlayer::play_track() {
  for (std::uint64_t number = 1;; ++number) {
    // Each pass after the first, a loop's or a play's after the end, plays
    // the audio from the start again: the feeder flushes the output.
    if (number > 1 && feeder_) {
      feeder_->restart();
    }
    Pass pass(number);
    stream::Position pos = reader_->begin();
    std::optional<InFlight> frame;  // recorded and submitted
    for (;;) {
      if (frame) {
        VKC_ASSIGN(const bool went, finish_frame(std::move(*frame), pass));
        frame.reset();
        if (!went) {
          return {};
        }
      }
      std::optional<Next> next;
      VKC_ASSIGN(const bool read, read_next(&pos, pass, &next));
      if (!read) {
        return {};
      }
      if (!next) {
        break;  // the end
      }
      // Ahead of its due time only while playing, so whenever the clock is
      // paused the grid holds the frame shown (DECISIONS.md).
      if (!playing()) {
        VKC_ASSIGN(const std::optional<Due> due,
                   wait_due(next->stamp.pts_ns, nullptr));
        if (!due) {
          return {};
        }
        next->stamp.due = due->at;
      }
      VKC_TRY(record_frame(std::move(*next), pass, &frame));
    }
    if (!pass.shown) {
      return nothing_shown(std::move(pass.first_skip));
    }
    VKC_ASSIGN(const bool again, wait_at_end());
    if (!again) {
      return {};
    }
  }
}

core::Result<bool> StreamPlayer::read_next(stream::Position* pos, Pass& pass,
                                           std::optional<Next>* next) {
  for (;;) {
    const Step step(*this);  // the record's read
    if (!step) {
      return false;
    }
    stream::Record record;
    const core::Result<bool> more = [&] {
      const core::StageScope read(pass.stages, "read");
      return reader_->next(pos, &record);
    }();
    if (more.ok() && !more.value()) {
      next->reset();
      return true;  // the end
    }
    if (more.ok() && record.info.track != track_) {
      continue;
    }
    // A damaged chunk header leaves `pos` past the damage.
    core::Status status = more.status();
    if (status.ok()) {
      const std::uint64_t frame = pass.index++;
      const core::StageScope read(pass.stages, "read");
      core::Result<core::HostSlot> slot = decoder_->claim_input(record.size);
      status = slot.status();
      if (status.ok()) {
        status = reader_->read(record, slot->data(),
                               static_cast<std::size_t>(slot->capacity()));
      }
      if (status.ok()) {
        Next& got = next->emplace();
        got.slot = std::move(slot).value();
        got.size = static_cast<std::size_t>(record.size);
        got.stamp.index = frame;
        got.stamp.pts_ns = record.info.pts_ns;
        got.stamp.bytes = record.size;
        got.stamp.key = record.info.key;
        got.stamp.set = record.info.set;
        got.stamp.record_bytes = record.size;
        got.stamp.pass = pass.number;
      }
    }
    if (status.ok()) {
      break;
    }
    VKC_TRY(pass.skip(std::move(status)));
  }
  // Once the scope of its read has closed.
  (*next)->stamp.stages = std::exchange(pass.stages, {});
  return true;
}

core::Status StreamPlayer::record_frame(Next next, Pass& pass,
                                        std::optional<InFlight>* frame) {
  Step step(*this);  // from its recording to its finish
  if (!step) {
    return {};  // quit: the next read sees it
  }
  FrameStamp& stamp = next.stamp;
  stamp.timed = time_next_frame();
  VKC_TRY(finish_hook_batch_before_growing(next.slot, next.size, stamp));
  core::CommandBatch batch = new_batch(stamp.timed);
  core::Result<codec::PendingDecode> decode =
      record_decode(batch, next.slot, next.size, stamp);
  if (!decode.ok()) {
    return pass.skip(decode.status());  // the batch is dropped
  }
  core::StageMetrics breakdowns;  // as record_decode's
  core::Result<mesh::PendingExtract> extract = [&] {
    const core::StageScope host(stamp.stages, "mesh");
    exchange_->release_and_may_publish(*extractor_);  // the consumer's first
    return extractor_->record_extract(batch, *grid_, decode->blocks,
                                      &decode->gate, 0.0f,
                                      stamp.timed ? &breakdowns : nullptr);
  }();
  if (!extract.ok()) {
    decoder_->abandon(*decode);
    return extract.status();  // a ring too short is fatal
  }
  forward_breakdowns(breakdowns, stamp.stages);
  core::Result<Batch> submitted =
      submit(batch, &extract->signal, &stamp.stages);
  if (!submitted.ok()) {
    extractor_->abandon(*extract);
    decoder_->abandon(*decode);
    return submitted.status();
  }
  frame->emplace(InFlight{std::move(stamp), std::move(decode).value(), *extract,
                          std::move(step), std::move(submitted).value()});
  return {};
}

core::Result<bool> StreamPlayer::finish_frame(InFlight frame, Pass& pass) {
  VKC_ASSIGN(const std::optional<Due> due,
             wait_due(frame.stamp.pts_ns, &frame));
  if (!due) {
    return false;
  }
  // Recorded once due, while paused, it was let go at once, so seen then.
  Clock::time_point seen = frame.stamp.due;
  if (frame.stamp.due == Clock::time_point{}) {
    frame.stamp.due = due->at;
    seen = due->seen;
  }
  if (!frame.step) {  // a park waited out its batch and ended its step
    frame.step = Step(*this);
    if (!frame.step) {
      return false;
    }
  }
  // About 0: the GPU kept up, and its fence signalled before it was due.
  // after_mesh's batch too, before the finish, which may grow the grid.
  VKC_TRY(wait_batch(frame));
  core::StageMetrics breakdowns;
  core::StageMetrics* const timed = frame.stamp.timed ? &breakdowns : nullptr;
  core::Status refused;
  std::optional<mesh::DeviceMesh> shown;
  {
    const core::StageScope host(frame.stamp.stages, "finish");
    const core::Result<codec::Finished> done = codec::finish_growing(
        *decoder_, frame.decode, *grid_, kMaxFrameBlocks, timed);
    if (!done.ok() || done->redone) {
      extractor_->abandon(*frame.extract);  // refused, or meshed gated off
    }
    if (!done.ok()) {
      refused = done.status();
    } else if (done->redone) {
      // Rare: decoded again, blocking; meshed in an untimed batch of its own.
      VKC_ASSIGN(shown, mesh_alone(frame.stamp, frame.decode.blocks, nullptr));
    } else {
      VKC_ASSIGN(shown,
                 extractor_->finish(*frame.extract, *grid_, nullptr, timed));
    }
  }
  forward_breakdowns(breakdowns, frame.stamp.stages);
  if (!refused.ok() || !shown) {
    VKC_TRY(pass.skip(std::move(refused)));  // the shown mesh stays
    return true;
  }
  stamp_deblock(frame.stamp);
  count_shown(frame.stamp.pts_ns, seen);
  last_ = frame.stamp;  // before after_mesh, so with no extra
  last_->stages.clear();
  pass.shown = true;
  VKC_TRY(run_after_mesh(*shown, frame.stamp));
  frame.step.end();  // nothing in flight but after_mesh's: a park may return
  return publish(*shown, std::move(frame.stamp));
}

core::Status StreamPlayer::play_live() {
  LiveConnection connection(address_);
  while (wait_unparked()) {
    if (networking::StreamClient* const client = connection.poll()) {
      // A new grid each connection, the decoder kept (DECISIONS.md), once
      // after_mesh's batch, which may read the last one, has finished.
      VKC_TRY(finish_hook_batch());
      grid_.reset();
      core::Status ended;
      VKC_TRY(play_connection(*client, &ended));
      connection.drop(ended);  // closed at once
    }
    // Why it ended or failed, none after a park or quit, and the wait to
    // retry, none after a clean end. A park, made before the wait or during
    // it, ends it, so the unpark connects at once.
    show_connecting(connection.last_error());
    if (wait_to_retry(connection.retry_at())) {
      connection.drop();
    }
  }
  return {};
}

core::Status StreamPlayer::play_connection(networking::StreamClient& client,
                                           core::Status* ended) {
  std::optional<std::uint16_t> track;  // the session's, once it is in
  std::optional<InFlight> decoding;    // a record's decode in flight
  bool pending = false;                // newest holds a frame not yet meshed
  FrameStamp newest;                   // the newest frame decoded
  std::uint32_t newest_blocks = 0;     // its blocks
  std::uint64_t records = 0;           // received since the last publish
  std::uint64_t record_bytes = 0;
  std::optional<Clock::time_point> published;  // the last, on this connection
  stream::Record record;
  // The connection's end, for `why`: a decode in flight is waited out,
  // submitting nothing, and dropped, as is a record unread.
  const auto end = [&](core::Status why) {
    if (decoding) {
      (void)decoding->batch.pending.wait();
      decoder_->abandon(decoding->decode);
      decoding.reset();
    }
    *ended = std::move(why);
    return core::Status();
  };
  for (;;) {
    bool stopping = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping = quit_ || parked_;
    }
    if (stopping) {
      return end({});
    }
    // Drain before waiting for a take. Even without a whole record, a
    // partial receive is progress: another nonblocking read may advance it.
    bool progressed = false;
    const bool in_hand = decoding.has_value() || pending;
    const core::Result<bool> got =
        client.next(in_hand ? std::chrono::milliseconds{0} : kSlice, &record,
                    nullptr, &progressed);
    if (!got.ok()) {
      return end(got.status());
    }
    // A complete record is due now, including any wait or recovery needed
    // to finish the preceding decode before this record can be decoded.
    const Clock::time_point arrived = Clock::now();
    if (!track && client.session() != nullptr) {
      track = first_geometry_track(*client.session());
      if (!track) {
        return end(core::Status::not_found(
            "the sender's session has no geometry track"));
      }
      std::lock_guard<std::mutex> lock(mutex_);
      live_status_.state = LiveState::kConnected;
    }
    // TODO: live audio, on the live playout clock: today every record of
    // another track is passed over.
    if (*got && record.info.track == *track) {
      ++records;
      record_bytes += record.size;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        live_status_.state = LiveState::kLive;
        live_status_.seq = record.info.seq;
        live_status_.set = record.info.set;
        live_status_.last_record = arrived;
      }
      // The decode before it finishes first: the next parses against the
      // reference it leaves.
      if (decoding) {
        VKC_ASSIGN(const bool decoded, finish_decode(std::move(*decoding),
                                                     &newest, &newest_blocks));
        decoding.reset();
        pending = pending || decoded;
      }
      Step step(*this, Step::kNoWait);  // from its recording to its finish
      if (!step) {
        return end({});  // quit or parked: the record is dropped
      }
      FrameStamp stamp;
      stamp.index = record.info.seq;
      stamp.pts_ns = record.info.pts_ns;
      stamp.bytes = record.size;
      stamp.key = record.info.key;
      stamp.set = record.info.set;
      stamp.due = arrived;
      stamp.timed = time_next_frame();
      // The body straight into one of the decoder's input slots: the other
      // is free, as the decode before has finished.
      core::Result<core::HostSlot> slot = decoder_->claim_input(record.size);
      if (slot.ok()) {
        const core::Status read = client.read(
            record, slot->data(), static_cast<std::size_t>(slot->capacity()));
        if (!read.ok()) {
          return end(read);
        }
        VKC_TRY(finish_hook_batch_before_growing(
            *slot, static_cast<std::size_t>(record.size), stamp));
        core::CommandBatch batch = new_batch(stamp.timed);
        core::Result<codec::PendingDecode> decode = record_decode(
            batch, *slot, static_cast<std::size_t>(record.size), stamp);
        if (decode.ok()) {
          core::Result<Batch> submitted = submit(batch, nullptr, &stamp.stages);
          if (!submitted.ok()) {
            decoder_->abandon(*decode);
            return submitted.status();
          }
          decoding.emplace(InFlight{std::move(stamp), std::move(decode).value(),
                                    std::nullopt, std::move(step),
                                    std::move(submitted).value()});
        } else if (!skippable(decode.status())) {
          return decode.status();
        }
      } else if (!skippable(slot.status())) {
        return slot.status();
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
    if (decoding || pending) {
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
        lock.unlock();
        return end({});
      }
    }
    // The newest, once its decode is judged: once nothing more has come, at
    // once for the connection's first frame, or every kMeshEvery while
    // records keep coming (DECISIONS.md).
    if (free && (!progressed || !published ||
                 Clock::now() - *published >= kMeshEvery)) {
      if (decoding) {
        VKC_ASSIGN(const bool decoded, finish_decode(std::move(*decoding),
                                                     &newest, &newest_blocks));
        decoding.reset();
        pending = pending || decoded;
      }
      if (pending) {
        const Step step(*this, Step::kNoWait);  // the mesh, published within
        if (!step) {
          return end({});
        }
        newest.records = std::exchange(records, 0);
        newest.record_bytes = std::exchange(record_bytes, 0);
        VKC_ASSIGN(std::optional<mesh::DeviceMesh> shown,
                   mesh_alone(newest, newest_blocks, "mesh"));
        if (shown) {
          VKC_TRY(run_after_mesh(*shown, newest));
          std::lock_guard<std::mutex> lock(mutex_);
          publish_now(*shown, std::move(newest));
        }
        newest = FrameStamp();
        pending = false;
        published = Clock::now();
      }
    }
  }
}

core::Result<bool> StreamPlayer::finish_decode(InFlight decoding,
                                               FrameStamp* newest,
                                               std::uint32_t* blocks) {
  FrameStamp& stamp = decoding.stamp;
  VKC_TRY(wait_batch(decoding));  // as at a file's frame's finish
  core::StageMetrics breakdowns;
  const core::Result<codec::Finished> done = [&] {
    const core::StageScope host(stamp.stages, "finish");
    return codec::finish_growing(*decoder_, decoding.decode, *grid_,
                                 kMaxFrameBlocks,
                                 stamp.timed ? &breakdowns : nullptr);
  }();
  forward_breakdowns(breakdowns, stamp.stages);
  if (!done.ok()) {
    if (!skippable(done.status())) {
      return done.status();
    }
    return false;
  }
  stamp_deblock(stamp);
  *blocks = decoding.decode.blocks;
  *newest = std::move(stamp);
  return true;
}

core::Result<codec::PendingDecode> StreamPlayer::record_decode(
    core::CommandBatch& batch, const core::HostSlot& slot, std::size_t size,
    FrameStamp& stamp) {
  // grid's rows: with them it tags its commands, whose tiles count to every
  // scope open around them, the player's included; without, it writes no
  // timestamp, and the player's rows have their host and wait halves alone.
  core::StageMetrics breakdowns;
  core::Result<codec::PendingDecode> decode =
      [&]() -> core::Result<codec::PendingDecode> {
    const core::StageScope host(stamp.stages, "decode");
    if (!grid_.has_value()) {
      VKC_TRY(make_grid(slot, size));
    }
    decoder_->set_deblock(deblock());
    return codec::record_growing(*decoder_, batch, slot, size, *grid_,
                                 kMaxFrameBlocks,
                                 stamp.timed ? &breakdowns : nullptr);
  }();
  if (decode.ok()) {
    forward_breakdowns(breakdowns, stamp.stages);
  }
  return decode;
}

core::Status StreamPlayer::wait_batch(InFlight& frame) {
  {
    const core::WaitScope wait(&frame.stamp.stages, "wait: GPU");
    VKC_TRY(frame.batch.pending.wait());
    // Before the frame's finish, which may grow the grid: after_mesh's
    // batch went to the queue before this one, so it has run (DECISIONS.md).
    VKC_TRY(finish_hook_batch());
  }
  note_batch(frame.batch, frame.stamp);
  return {};
}

core::Status StreamPlayer::make_grid(const core::HostSlot& slot,
                                     std::size_t size) {
  VKC_ASSIGN(grid::volume::VoxelBlockGrid grid,
             codec::make_decode_grid(*device_, *allocator_, slot.data(), size,
                                     kMaxFrameBlocks));
  grid_.emplace(std::move(grid));
  return {};
}

core::Result<StreamPlayer::Batch> StreamPlayer::submit(
    core::CommandBatch& batch, const core::TimelinePoint* signal,
    core::StageMetrics* rows) {
  const core::StageScope host(rows, "submit");
  Batch submitted;
  submitted.submits_before = device_->counters().submits;
  submitted.submitted_ns = steady_ns(Clock::now());
  std::vector<core::TimelinePoint> signals;
  if (signal != nullptr) {
    signals.push_back(*signal);
  }
  VKC_ASSIGN(submitted.pending, batch.submit_async({}, signals));
  return submitted;
}

void StreamPlayer::note_batch(const Batch& batch, FrameStamp& stamp) {
  // Device rows come for every scope a tile counts to: the stamp's own, and
  // grid's breakdowns, whose host halves it has, and grid's stage rows
  // ("codec decode", "marching cubes"), which the player's hold already.
  for (const core::StageRow& row : batch.pending.device_rows()) {
    for (const core::StageRow& own : stamp.stages.rows()) {
      if (std::strcmp(own.name, row.name) == 0) {
        stamp.stages.add_device(own.name, row.device, row.gpu_ms);
        break;
      }
    }
  }
  const std::optional<core::DeviceSpan> span = batch.pending.device_span();
  if (!span) {
    last_end_ticks_.reset();
    return;
  }
  // The queue's idle since the worker's last batch, when that was timed and
  // nothing was submitted between them: raw ticks of one counter, so no
  // calibration.
  if (!stamp.device_idle_ms && last_end_ticks_ &&
      last_submits_ == batch.submits_before) {
    const std::uint32_t bits = device_->timestamp_valid_bits();
    const std::uint64_t mask =
        bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
    const std::uint64_t ticks = (span->begin_ticks - *last_end_ticks_) & mask;
    stamp.device_idle_ms =
        static_cast<double>(ticks) *
        static_cast<double>(
            device_->caps().properties().limits.timestampPeriod) *
        1e-6;
  }
  if (!stamp.queue_ms && span->host_clock) {
    stamp.queue_ms =
        static_cast<double>(span->begin_ns - batch.submitted_ns) * 1e-6;
  }
  last_end_ticks_ = span->end_ticks;
  last_submits_ = batch.submits_before + 1;
}

core::Result<std::optional<mesh::DeviceMesh>> StreamPlayer::mesh_alone(
    FrameStamp& stamp, std::uint32_t blocks, const char* row) {
  const bool timed = row != nullptr && stamp.timed;
  core::StageMetrics breakdowns;  // as record_decode's
  std::optional<Batch> ran;
  core::Result<std::optional<mesh::DeviceMesh>> meshed =
      [&]() -> core::Result<std::optional<mesh::DeviceMesh>> {
    const core::StageScope host(row != nullptr ? &stamp.stages : nullptr,
                                row != nullptr ? row : "mesh");
    core::CommandBatch batch = new_batch(timed);
    exchange_->release_and_may_publish(*extractor_);  // the consumer's first
    VKC_ASSIGN(const mesh::PendingExtract extract,
               extractor_->record_extract(batch, *grid_, blocks, nullptr, 0.0f,
                                          timed ? &breakdowns : nullptr));
    core::Result<Batch> submitted = submit(batch, &extract.signal, nullptr);
    if (!submitted.ok()) {
      extractor_->abandon(extract);
      return submitted.status();
    }
    ran.emplace(std::move(submitted).value());
    VKC_TRY(ran->pending.wait());
    return extractor_->finish(extract, *grid_, nullptr,
                              timed ? &breakdowns : nullptr);
  }();
  forward_breakdowns(breakdowns, stamp.stages);
  if (ran && meshed.ok()) {
    note_batch(*ran, stamp);
  }
  return meshed;
}

void StreamPlayer::stamp_deblock(FrameStamp& stamp) const {
  stamp.deblock_on = decoder_->deblock();
  stamp.deblock = decoder_->last_deblock();
  stamp.rounds = decoder_->last_rounds();
}

core::Result<std::optional<StreamPlayer::Due>> StreamPlayer::wait_due(
    std::uint64_t pts_ns, InFlight* frame) {
  for (;;) {
    bool settle = false;  // parked with frame's batch in flight
    {
      std::unique_lock<std::mutex> lock(mutex_);
      for (;;) {
        if (quit_) {
          return std::optional<Due>();
        }
        const Clock::time_point now = Clock::now();
        // Parked, nothing is due: a control takes effect at unpark.
        if (parked_) {
          if (frame != nullptr && frame->step) {
            settle = true;
            break;
          }
          changed_.wait(lock);
          continue;
        }
        // A pause publishes the frame recorded ahead, so the grid holds the
        // frame shown (DECISIONS.md).
        if (frame != nullptr && !clock_.playing()) {
          return std::optional<Due>(Due{now, now});
        }
        const std::optional<Clock::time_point> due = clock_.due(pts_ns, now);
        if (due && *due <= now) {
          return std::optional<Due>(Due{*due, now});
        }
        if (frame == nullptr && rebuild_wanted()) {
          break;
        }
        if (due) {
          changed_.wait_until(lock, *due);
        } else {
          changed_.wait(lock);
        }
      }
    }
    if (settle) {
      // Its batch waited out, submitting nothing; its finish, a recovery
      // included, waits for unpark. A failed wait fails again there.
      (void)frame->batch.pending.wait();
      frame->step.end();
      continue;
    }
    VKC_ASSIGN(const bool rebuilt, rebuild());
    if (!rebuilt) {
      return std::optional<Due>();
    }
  }
}

void StreamPlayer::count_shown(std::uint64_t pts_ns, Clock::time_point seen) {
  std::lock_guard<std::mutex> lock(mutex_);
  last_by_step_ = !clock_.playing();
  // Shown as of when it was seen due, so a slow decode does not push the
  // frames after it back; a pause since, with no step to take, leaves the
  // clock as it is (Playback::shown's precondition).
  if (clock_.due(pts_ns, seen)) {
    clock_.shown(pts_ns, seen);
  }
  last_pts_ns_ = pts_ns;
}

bool StreamPlayer::rebuild_wanted() const {
  return last_ && !parked_ && !clock_.playing() &&
         deblock_ != last_->deblock_on;
}

core::Result<bool> StreamPlayer::rebuild() {
  FrameStamp stamp = *last_;
  stamp.rebuild = true;
  // Never timed: no frame, so no reader for its device halves (DECISIONS.md).
  stamp.timed = false;
  std::optional<mesh::DeviceMesh> shown;
  {
    const Step step(*this);  // the rebuild
    if (!step) {
      return false;
    }
    const bool on = deblock();
    decoder_->set_deblock(on);
    last_->deblock_on = on;  // tried: once a change
    core::Status status;
    {
      const core::StageScope decode(stamp.stages, "decode");
      status = decoder_->rebuild(*grid_, nullptr);
    }
    if (!status.ok()) {
      // A grid a failed decode left as neither frame: the next frame shows
      // the setting.
      if (skippable(status)) {
        return true;
      }
      return status;
    }
    stamp_deblock(stamp);
    last_->deblock = stamp.deblock;
    VKC_ASSIGN(shown,
               mesh_alone(stamp, grid_->map().allocated_blocks(), "mesh"));
    if (shown) {
      VKC_TRY(run_after_mesh(*shown, stamp));
    }
  }
  if (!shown) {
    return true;
  }
  return publish(*shown, std::move(stamp));
}

core::Result<bool> StreamPlayer::publish(const mesh::DeviceMesh& mesh,
                                         FrameStamp stamp) {
  // Asleep until taken() or another control wakes it.
  std::unique_lock<std::mutex> lock(mutex_);
  changed_.wait(lock, [this] {
    return quit_ ||
           (!parked_ && exchange_->release_and_may_publish(*extractor_));
  });
  if (quit_) {
    lock.unlock();
    // Never published, its extra goes with `stamp`: after_mesh's batch, which
    // may use what the extra holds, finishes first. A failure reaches status()
    // at the worker's end.
    (void)finish_hook_batch();
    return false;
  }
  publish_now(mesh, std::move(stamp));  // under the lock: no park between
  return true;
}

void StreamPlayer::publish_now(const mesh::DeviceMesh& mesh, FrameStamp stamp) {
  const core::DeviceCounters counted = device_->counters();
  stamp.counters = since(counted, counted_);
  counted_ = counted;
  stamp.published = Clock::now();
  exchange_->publish(mesh, std::move(stamp));
}

core::Status StreamPlayer::run_after_mesh(mesh::DeviceMesh& mesh,
                                          FrameStamp& stamp) {
  if (hooks_ == nullptr || !hooks_->after_mesh) {
    return {};
  }
  // Its fence has normally signalled: the GPU ran it a frame ago.
  VKC_TRY(finish_hook_batch());
  core::CommandBatch batch(*device_, *allocator_);
  VKC_TRY(hooks_->after_mesh(batch, *grid_, mesh, stamp));
  std::vector<core::TimelinePoint> waits;
  if (mesh.ready.semaphore != nullptr) {
    waits.push_back(mesh.ready);
  }
  const core::TimelinePoint ready{&*hook_timeline_, hook_value_ + 1};
  VKC_ASSIGN(core::PendingBatch pending, batch.submit_async(waits, {ready}));
  hook_value_ = ready.value;
  {
    std::lock_guard<std::timed_mutex> lock(hook_mutex_);
    hook_batch_.emplace(std::move(pending));
  }
  mesh.ready = ready;
  return {};
}

core::Status StreamPlayer::finish_hook_batch_before_growing(
    const core::HostSlot& slot, std::size_t size, FrameStamp& stamp) {
  if (hooks_ == nullptr || !hooks_->after_mesh || !grid_.has_value()) {
    return {};
  }
  // codec::record_growing grows the grid, before it records, only for a
  // frame of more blocks than its heap (FrameInfo::block_count against
  // num_blocks). A header it cannot read it refuses before any grow.
  const core::Result<codec::FrameInfo> info =
      codec::read_frame_info(slot.data(), size);
  if (!info.ok() || info->block_count <=
                        static_cast<std::uint32_t>(grid_->grid().num_blocks)) {
    return {};
  }
  // May block, on such a frame alone, whose grow waits for the queue anyway.
  const core::StageScope host(stamp.stages, "decode");
  return finish_hook_batch();
}

core::Status StreamPlayer::finish_hook_batch() {
  std::lock_guard<std::timed_mutex> lock(hook_mutex_);
  (void)wait_hook_batch(UINT64_MAX);  // never times out
  return hook_failure_;
}

bool StreamPlayer::finish_hook_batch_by(Clock::time_point deadline) {
  std::unique_lock<std::timed_mutex> lock(hook_mutex_, deadline);
  if (!lock.owns_lock()) {
    return false;  // the worker's, finishing it
  }
  const std::chrono::nanoseconds left =
      std::chrono::duration_cast<std::chrono::nanoseconds>(deadline -
                                                           Clock::now());
  return wait_hook_batch(
      static_cast<std::uint64_t>(std::max<std::int64_t>(left.count(), 0)));
}

bool StreamPlayer::wait_hook_batch(std::uint64_t timeout_ns) {
  if (hook_batch_) {
    core::Status waited = hook_batch_->wait(timeout_ns);
    if (core::is_timeout(waited)) {
      return false;
    }
    hook_batch_.reset();
    if (!waited.ok() && hook_failure_.ok()) {
      hook_failure_ = std::move(waited);
    }
  }
  return true;
}

core::Result<bool> StreamPlayer::wait_at_end() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // With loop off the last frame holds: played into, it pauses there, and
    // the audio with it; stepped onto, or paused on, it is held already, so
    // a play, even one since that step, restarts.
    if (!loop_ && clock_.playing() && !last_by_step_) {
      clock_.pause();
      if (audio_) {
        audio_->pause();
      }
    }
  }
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      for (;;) {
        // Until a frame may go, playing or a step, or a rebuild is wanted.
        changed_.wait(lock, [this] {
          return quit_ || clock_.due(last_pts_ns_, Clock::now()).has_value() ||
                 rebuild_wanted();
        });
        if (quit_) {
          return false;
        }
        if (!clock_.due(last_pts_ns_, Clock::now())) {
          break;  // a rebuild
        }
        if (loop_ || clock_.playing()) {
          return true;
        }
        clock_.shown(last_pts_ns_, Clock::now());  // a step: the held frame
      }
    }
    VKC_ASSIGN(const bool rebuilt, rebuild());
    if (!rebuilt) {
      return false;
    }
  }
}

bool StreamPlayer::wait_unparked() {
  std::unique_lock<std::mutex> lock(mutex_);
  changed_.wait(lock, [this] { return quit_ || !parked_; });
  return !quit_;
}

void StreamPlayer::show_connecting(const std::string& last_error) {
  std::lock_guard<std::mutex> lock(mutex_);
  live_status_.state = LiveState::kConnecting;
  live_status_.last_error = last_error;
}

bool StreamPlayer::wait_to_retry(std::chrono::steady_clock::time_point at) {
  std::unique_lock<std::mutex> lock(mutex_);
  changed_.wait_until(lock, at, [this] { return quit_ || parked_; });
  return parked_;
}

}  // namespace volumetric_kit::player
