// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// A file's audio on the host, with synthetic ramps: no device, no sound. The
// track played is the first Audio track on the geometry's clock with
// records and params the player plays; one on another clock, of a codec
// other than PcmS16, with params that do not decode, of a rate outside the
// played range or with no record is skipped, as is every Audio track after
// it. Samples become f32 over 32768, and frames of more than two channels
// their first two. A record within one chunk of the running position
// follows on; past it, a forward jump is silence and an overlap is dropped;
// the first record after a flush is placed exactly. The feeder, on a file:
// audio starts at the first geometry pts, records before it dropped or
// trimmed and a later first record led by silence; a gap past one chunk is
// silence and jitter within one follows on; an overlap past one chunk is
// dropped; it plays the first playable track on the geometry's clock,
// downmixed; a restart flushes the output and reads from the start; it
// keeps between its low and high lead; and on threads, under
// ThreadSanitizer, the null output on its own clock plays what it writes.

#include "volumetric_kit/player/audio_feeder.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "volumetric_kit/grid/stream/stream_writer.hpp"

namespace {

namespace core = volumetric_kit::core;
namespace stream = volumetric_kit::grid::stream;
using namespace std::chrono_literals;
using volumetric_kit::player::audio_frames;
using volumetric_kit::player::audio_ns;
using volumetric_kit::player::AudioFeeder;
using volumetric_kit::player::AudioFeedStats;
using volumetric_kit::player::AudioFormat;
using volumetric_kit::player::AudioOut;
using volumetric_kit::player::AudioTrackChoice;
using volumetric_kit::player::find_audio_track;
using volumetric_kit::player::kMaxSampleRate;
using volumetric_kit::player::kMinSampleRate;
using volumetric_kit::player::NullAudioOut;
using volumetric_kit::player::place;
using volumetric_kit::player::Placement;
using volumetric_kit::player::played_channels;
using volumetric_kit::player::played_format;
using volumetric_kit::player::to_played;
using SteadyClock = std::chrono::steady_clock;

constexpr std::uint32_t kRate = 8000;     // a frame is 125 us
constexpr std::uint64_t kChunk = 80;      // frames a record: 10 ms
constexpr std::uint64_t kMs = 1'000'000;  // ns
constexpr std::uint64_t kFrameNs = 125'000;

// An Audio track of `channels` at kRate on `clock`, coded as `codec`.
stream::Track audio_track(
    std::uint16_t channels, std::uint16_t clock = 1, std::uint32_t rate = kRate,
    stream::AudioCodec codec = stream::AudioCodec::PcmS16) {
  stream::Track track;
  track.kind = stream::TrackKind::Audio;
  track.clock = clock;
  track.rate = {rate, static_cast<std::uint32_t>(kChunk)};
  stream::AudioParams params;
  params.codec = codec;
  params.channels = channels;
  params.sample_rate = rate;
  track.params = stream::encode_audio_params(params);
  return track;
}

stream::Track geometry_track() {
  stream::Track track;
  track.kind = stream::TrackKind::Geometry;
  track.rate = {25, 1};
  return track;
}

// A record to write: a geometry record of one byte when `samples` is empty.
struct Rec {
  std::uint16_t track = 0;
  std::uint64_t pts_ns = 0;
  std::vector<std::int16_t> samples;
};

// Frames of `channels` counting up: frame i's channel c is
// `base + (c + 1) * 1000 + i`.
std::vector<std::int16_t> ramp(int base, std::uint64_t frames,
                               std::uint16_t channels = 1) {
  std::vector<std::int16_t> out;
  for (std::uint64_t i = 0; i < frames; ++i) {
    for (std::uint16_t c = 0; c < channels; ++c) {
      out.push_back(static_cast<std::int16_t>(base + (c + 1) * 1000 +
                                              static_cast<int>(i)));
    }
  }
  return out;
}

// As the output plays `samples` of `channels`: f32, the first two channels.
std::vector<float> played(const std::vector<std::int16_t>& samples,
                          std::uint16_t channels = 1, std::uint64_t from = 0) {
  std::vector<float> out;
  const std::size_t frames = samples.size() / channels;
  for (std::size_t f = static_cast<std::size_t>(from); f < frames; ++f) {
    for (std::uint16_t c = 0; c < played_channels(channels); ++c) {
      out.push_back(static_cast<float>(samples[f * channels + c]) / 32768.0f);
    }
  }
  return out;
}

void append(std::vector<float>* to, const std::vector<float>& more) {
  to->insert(to->end(), more.begin(), more.end());
}

// A directory of the test's own, removed with what it holds.
class Scratch {
 public:
  Scratch() {
    const ::testing::TestInfo* test =
        ::testing::UnitTest::GetInstance()->current_test_info();
    path_ = std::filesystem::temp_directory_path() /
            ("vp_audio_" + std::string(test->name()) + "_" +
             std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_);
  }
  ~Scratch() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;

  std::string file(const char* name) const { return (path_ / name).string(); }

 private:
  std::filesystem::path path_;
};

// A file of `tracks`, track 0 geometry, with `records` in order.
core::Status write_file(const std::string& path,
                        const std::vector<stream::Track>& tracks,
                        const std::vector<Rec>& records) {
  stream::Session session;
  session.writer = "volumetric_kit_player tests";
  session.tracks = tracks;
  VKC_ASSIGN(std::unique_ptr<stream::StreamWriter> writer,
             stream::StreamWriter::create(path, session));
  std::vector<std::uint64_t> seq(tracks.size(), 0);
  for (const Rec& rec : records) {
    stream::RecordInfo info;
    info.track = rec.track;
    info.key = true;
    info.pts_ns = rec.pts_ns;
    info.seq = seq[rec.track]++;
    std::vector<std::uint8_t> bytes;
    for (const std::int16_t sample : rec.samples) {
      const auto u = static_cast<std::uint16_t>(sample);
      bytes.push_back(static_cast<std::uint8_t>(u & 0xff));
      bytes.push_back(static_cast<std::uint8_t>(u >> 8));
    }
    if (bytes.empty()) {
      bytes.push_back(0);
    }
    VKC_TRY(writer->append(info, bytes.data(), bytes.size()));
  }
  return writer->finish();
}

// An output the test renders itself, as a device's render thread would.
class TestOut final : public AudioOut {
 public:
  explicit TestOut(const AudioFormat& format) : AudioOut(format) { play(); }
  using AudioOut::render;

  // Every frame queued, rendered.
  std::vector<float> rest() {
    render(nullptr, 0);  // the flush first
    std::vector<float> out(static_cast<std::size_t>(queued()) *
                           format().channels);
    render(out.data(), out.size() / format().channels);
    return out;
  }
};

// Whether `done()` holds within 10 s, asked every millisecond.
template <typename Done>
bool eventually(Done&& done) {
  const SteadyClock::time_point deadline = SteadyClock::now() + 10s;
  while (!done()) {
    if (SteadyClock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

// A file of `tracks` and `records` opened, its track to play found, an
// output of its format made, and a feeder started on it from `start_pts`,
// run until it has read the track to its end.
class Fed {
 public:
  Fed(const std::string& path, const std::vector<stream::Track>& tracks,
      const std::vector<Rec>& records, std::uint64_t start_pts) {
    const core::Status written = write_file(path, tracks, records);
    EXPECT_TRUE(written.ok()) << written.message();
    core::Result<std::unique_ptr<stream::StreamReader>> reader =
        stream::StreamReader::open(path);
    EXPECT_TRUE(reader.ok()) << reader.status().message();
    if (!reader.ok()) {
      return;
    }
    reader_ = std::move(reader).value();
    choice_ = find_audio_track(reader_->session(), reader_->stats(), 0);
    EXPECT_TRUE(choice_.has_value()) << "no track to play";
    if (!choice_) {
      return;
    }
    out_ = std::make_unique<TestOut>(played_format(choice_->params));
    core::Result<std::unique_ptr<AudioFeeder>> feeder =
        AudioFeeder::start(*reader_, *choice_, start_pts, *out_);
    EXPECT_TRUE(feeder.ok()) << feeder.status().message();
    if (feeder.ok()) {
      feeder_ = std::move(feeder).value();
    }
  }

  bool ok() const { return feeder_ != nullptr; }
  // Once it has read the track to its end, its stats.
  AudioFeedStats ended() {
    EXPECT_TRUE(eventually([&] { return feeder_->stats().ended; }))
        << "the feeder never reached the track's end";
    return feeder_->stats();
  }
  AudioFeeder& feeder() { return *feeder_; }
  TestOut& out() { return *out_; }
  const AudioTrackChoice& choice() const { return *choice_; }

 private:
  std::unique_ptr<stream::StreamReader> reader_;
  std::optional<AudioTrackChoice> choice_;
  std::unique_ptr<TestOut> out_;
  std::unique_ptr<AudioFeeder> feeder_;  // first to go
};

TEST(FindAudioTrack, TakesTheFirstPlayableTrackOnTheGeometrysClock) {
  stream::Session session;
  session.tracks = {
      geometry_track(),
      audio_track(1, 2),  // another clock
      audio_track(1, 1, kRate, static_cast<stream::AudioCodec>(9)),
      audio_track(0),                         // params that do not decode
      audio_track(1, 1, kMinSampleRate - 1),  // too slow
      audio_track(1, 1, kMaxSampleRate + 1),  // too fast
      audio_track(2),                         // no record
      audio_track(2, 1, 48000),               // the one
      audio_track(1),                         // after it
  };
  stream::TrackStats some;
  some.records = 3;
  std::vector<stream::TrackStats> stats(session.tracks.size(), some);
  stats[6].records = 0;
  std::optional<AudioTrackChoice> chosen = find_audio_track(session, stats, 0);
  ASSERT_TRUE(chosen.has_value());
  EXPECT_EQ(chosen->index, 7u);
  EXPECT_EQ(chosen->params.channels, 2u);
  EXPECT_EQ(chosen->params.sample_rate, 48000u);
  // One before the geometry's, on its clock, is the first.
  session.tracks.insert(session.tracks.begin(), audio_track(1));
  stats.insert(stats.begin(), some);
  chosen = find_audio_track(session, stats, 1);
  ASSERT_TRUE(chosen.has_value());
  EXPECT_EQ(chosen->index, 0u);
  // The rates at each end of the range play.
  for (const std::uint32_t rate : {kMinSampleRate, kMaxSampleRate}) {
    session.tracks = {geometry_track(), audio_track(1, 1, rate)};
    stats.assign(2, some);
    chosen = find_audio_track(session, stats, 0);
    ASSERT_TRUE(chosen.has_value()) << rate;
    EXPECT_EQ(chosen->index, 1u);
  }
}

TEST(FindAudioTrack, NoneWithoutOne) {
  stream::Session session;
  session.tracks = {geometry_track(), geometry_track()};
  std::vector<stream::TrackStats> stats(2);
  stats[0].records = stats[1].records = 1;
  EXPECT_FALSE(find_audio_track(session, stats, 0).has_value());
  session.tracks.push_back(audio_track(1));
  stats.push_back(stats[0]);
  EXPECT_FALSE(find_audio_track(session, stats, 7).has_value())
      << "no geometry track there";
}

TEST(ToPlayed, SamplesAreOver32768) {
  const std::vector<std::int16_t> samples = {-32768, -16384, 0,
                                             1,      16384,  32767};
  std::vector<std::uint8_t> pcm;
  for (const std::int16_t s : samples) {
    const auto u = static_cast<std::uint16_t>(s);
    pcm.push_back(static_cast<std::uint8_t>(u & 0xff));
    pcm.push_back(static_cast<std::uint8_t>(u >> 8));
  }
  std::vector<float> out(samples.size());
  to_played(pcm.data(), samples.size(), 1, out.data());
  EXPECT_EQ(out, (std::vector<float>{-1.0f, -0.5f, 0.0f, 1.0f / 32768.0f, 0.5f,
                                     32767.0f / 32768.0f}));
}

TEST(ToPlayed, MoreThanTwoChannelsAreDownmixedToTheFirstTwo) {
  EXPECT_EQ(played_channels(1), 1u);
  EXPECT_EQ(played_channels(2), 2u);
  EXPECT_EQ(played_channels(6), 2u);
  for (const std::uint16_t channels : {1, 2, 4}) {
    const std::vector<std::int16_t> samples = ramp(0, 3, channels);
    std::vector<std::uint8_t> pcm;
    for (const std::int16_t s : samples) {
      pcm.push_back(static_cast<std::uint8_t>(s & 0xff));
      pcm.push_back(static_cast<std::uint8_t>(s >> 8));
    }
    std::vector<float> out(3 * played_channels(channels), -9.0f);
    to_played(pcm.data(), 3, channels, out.data());
    EXPECT_EQ(out, played(samples, channels)) << channels << " channels";
  }
  const AudioFormat format =
      played_format({stream::AudioCodec::PcmS16, 6, 44100});
  EXPECT_EQ(format.sample_rate, 44100u);
  EXPECT_EQ(format.channels, 2u);
}

// Mutant: the re-sync threshold (> one chunk) as >= or as 0.
TEST(Place, WithinOneChunkARecordFollowsOnAndPastItItIsPlaced) {
  constexpr std::uint64_t kAt = 1'000 * kMs;
  const auto ahead = [](std::uint64_t frames) {
    return kAt + frames * kFrameNs;
  };
  const auto behind = [](std::uint64_t frames) {
    return kAt - frames * kFrameNs;
  };
  // Within one chunk either way: follows on.
  for (const std::uint64_t pts :
       {kAt, ahead(1), ahead(kChunk), behind(1), behind(kChunk)}) {
    const Placement at = place(kAt, pts, kChunk, kRate, false);
    EXPECT_EQ(at.silence, 0u) << pts;
    EXPECT_EQ(at.skip, 0u) << pts;
  }
  // Past it: silence for a forward jump, the overlap dropped (the whole
  // record, as it lies wholly behind).
  EXPECT_EQ(place(kAt, ahead(kChunk + 1), kChunk, kRate, false).silence,
            kChunk + 1);
  EXPECT_EQ(place(kAt, ahead(1000), kChunk, kRate, false).silence, 1000u);
  EXPECT_EQ(place(kAt, behind(kChunk + 1), kChunk, kRate, false).skip, kChunk);
  // A chunk is the record's own: a longer one follows on further out.
  EXPECT_EQ(place(kAt, ahead(150), 160, kRate, false).silence, 0u);
  // Exact: placed at its pts, part of a record dropped.
  EXPECT_EQ(place(kAt, ahead(3), kChunk, kRate, true).silence, 3u);
  EXPECT_EQ(place(kAt, behind(30), kChunk, kRate, true).skip, 30u);
  EXPECT_EQ(place(kAt, behind(300), kChunk, kRate, true).skip, kChunk);
  // Half a frame rounds to the nearest.
  EXPECT_EQ(place(kAt, kAt + kFrameNs / 2, kChunk, kRate, true).silence, 1u);
  EXPECT_EQ(place(kAt, kAt + kFrameNs / 2 - 1, kChunk, kRate, true).silence,
            0u);
}

// Records before the first geometry pts are dropped, one across it trimmed
// to start there, and the records after follow on; a first record after it
// is led by silence from it.
TEST(AudioFeeder, AudioStartsAtTheFirstGeometryPts) {
  Scratch scratch;
  constexpr std::uint64_t kG = 100 * kMs;
  const std::vector<std::int16_t> r0 = ramp(0, kChunk);
  const std::vector<std::int16_t> r1 = ramp(100, kChunk);
  const std::vector<std::int16_t> r2 = ramp(200, kChunk);
  {
    Fed fed(scratch.file("across.vrst"), {geometry_track(), audio_track(1)},
            {{1, 85 * kMs, r0},
             {0, kG, {}},
             {1, 95 * kMs, r1},
             {1, 105 * kMs, r2},
             {0, kG + 40 * kMs, {}}},
            kG);
    ASSERT_TRUE(fed.ok());
    const AudioFeedStats stats = fed.ended();
    std::vector<float> want = played(r1, 1, 40);  // its 5 ms after kG
    append(&want, played(r2));
    EXPECT_EQ(fed.out().rest(), want);
    EXPECT_EQ(fed.out().position(), kG + audio_ns(40 + kChunk, kRate));
    EXPECT_EQ(stats.records, 2u);
    EXPECT_EQ(stats.dropped_frames, kChunk + 40);
    EXPECT_EQ(stats.silence_frames, 0u);
    EXPECT_EQ(stats.passes, 1u);
  }
  {
    Fed fed(scratch.file("after.vrst"), {geometry_track(), audio_track(1)},
            {{0, kG, {}}, {1, kG + 3 * kMs, r0}, {1, kG + 13 * kMs, r1}}, kG);
    ASSERT_TRUE(fed.ok());
    EXPECT_EQ(fed.ended().silence_frames, 24u);
    std::vector<float> want(24, 0.0f);
    append(&want, played(r0));
    append(&want, played(r1));
    EXPECT_EQ(fed.out().rest(), want);
  }
}

// Mutant: the re-sync threshold. Jitter within one chunk follows on, a gap
// of exactly one chunk too, and one a frame longer is filled with silence.
TEST(AudioFeeder, AGapPastOneChunkIsSilenceAndJitterFollowsOn) {
  Scratch scratch;
  std::vector<std::vector<std::int16_t>> r;
  for (int i = 0; i < 4; ++i) {
    r.push_back(ramp(100 * i, kChunk));
  }
  Fed fed(scratch.file("gap.vrst"), {geometry_track(), audio_track(1)},
          {{0, 0, {}},
           {1, 0, r[0]},
           {1, 13 * kMs, r[1]},              // 3 ms late: follows on at 10
           {1, 30 * kMs, r[2]},              // one chunk on: follows on at 20
           {1, 40 * kMs + kFrameNs, r[3]}},  // 81 frames on: placed
          0);
  ASSERT_TRUE(fed.ok());
  const AudioFeedStats stats = fed.ended();
  std::vector<float> want;
  for (int i = 0; i < 3; ++i) {
    append(&want, played(r[i]));
  }
  want.resize(want.size() + kChunk + 1, 0.0f);
  append(&want, played(r[3]));
  EXPECT_EQ(fed.out().rest(), want);
  EXPECT_EQ(stats.records, 4u);
  EXPECT_EQ(stats.silence_frames, kChunk + 1);
  EXPECT_EQ(stats.dropped_frames, 0u);
}

// An overlap of one chunk follows on; past one chunk, the record, wholly
// behind, is dropped, and the next follows on.
TEST(AudioFeeder, AnOverlapPastOneChunkIsDropped) {
  Scratch scratch;
  std::vector<std::vector<std::int16_t>> r;
  for (int i = 0; i < 5; ++i) {
    r.push_back(ramp(100 * i, kChunk));
  }
  Fed fed(scratch.file("overlap.vrst"), {geometry_track(), audio_track(1)},
          {{0, 0, {}},
           {1, 0, r[0]},
           {1, 10 * kMs, r[1]},
           {1, 10 * kMs, r[2]},  // one chunk behind: follows on at 20
           {1, 15 * kMs, r[3]},  // 15 ms behind 30: dropped
           {1, 30 * kMs, r[4]}},
          0);
  ASSERT_TRUE(fed.ok());
  const AudioFeedStats stats = fed.ended();
  std::vector<float> want;
  for (const int i : {0, 1, 2, 4}) {
    append(&want, played(r[i]));
  }
  EXPECT_EQ(fed.out().rest(), want);
  EXPECT_EQ(stats.records, 4u);
  EXPECT_EQ(stats.dropped_frames, kChunk);
}

// Of an Audio track on another clock, one of an unknown codec, the first
// playable one (4 channels, downmixed) and one after it, the third plays.
TEST(AudioFeeder, ItPlaysTheFirstPlayableTrackOnTheGeometrysClock) {
  Scratch scratch;
  const std::vector<std::int16_t> other = ramp(10000, kChunk);
  const std::vector<std::int16_t> chosen = ramp(0, 2 * kChunk, 4);
  Fed fed(scratch.file("tracks.vrst"),
          {geometry_track(), audio_track(1, 2),
           audio_track(1, 1, kRate, static_cast<stream::AudioCodec>(9)),
           audio_track(4), audio_track(1)},
          {{0, 0, {}},
           {1, 0, other},
           {2, 0, other},
           {3, 0, chosen},
           {4, 0, other},
           {4, 10 * kMs, other}},
          0);
  ASSERT_TRUE(fed.ok());
  EXPECT_EQ(fed.choice().index, 3u);
  EXPECT_EQ(fed.out().format().channels, 2u);
  const AudioFeedStats stats = fed.ended();
  EXPECT_EQ(stats.records, 1u);
  EXPECT_EQ(fed.out().rest(), played(chosen, 4));
}

TEST(AudioFeeder, ARestartFlushesTheOutputAndReadsFromTheStart) {
  Scratch scratch;
  constexpr std::uint64_t kG = 50 * kMs;
  std::vector<Rec> records = {{0, kG, {}}};
  std::vector<float> all;
  for (int i = 0; i < 5; ++i) {
    records.push_back({1, kG + i * 10 * kMs, ramp(100 * i, kChunk)});
    append(&all, played(records.back().samples));
  }
  Fed fed(scratch.file("restart.vrst"), {geometry_track(), audio_track(1)},
          records, kG);
  ASSERT_TRUE(fed.ok());
  fed.ended();
  std::vector<float> some(30);
  fed.out().render(some.data(), 30);
  EXPECT_EQ(fed.out().position(), kG + audio_ns(30, kRate));
  fed.feeder().restart();
  ASSERT_TRUE(eventually([&] {
    const AudioFeedStats stats = fed.feeder().stats();
    return stats.passes == 2 && stats.ended;
  }));
  EXPECT_EQ(fed.out().rest(), all) << "the whole track again";
  EXPECT_EQ(fed.out().stats().flushes, 2u);
}

// Below its low lead it reads up to its high one, a record past it at most.
TEST(AudioFeeder, ItKeepsBetweenItsLowAndHighLead) {
  Scratch scratch;
  std::vector<Rec> records = {{0, 0, {}}};
  for (int i = 0; i < 200; ++i) {  // 2 s
    records.push_back({1, i * 10 * kMs, ramp(i, kChunk)});
  }
  Fed fed(scratch.file("lead.vrst"), {geometry_track(), audio_track(1)},
          records, 0);
  ASSERT_TRUE(fed.ok());
  const std::uint64_t low = audio_frames(200 * kMs, kRate);
  const std::uint64_t high = audio_frames(500 * kMs, kRate);
  ASSERT_TRUE(eventually([&] { return fed.out().queued() >= high; }));
  std::this_thread::sleep_for(50ms);  // it has stopped there
  EXPECT_LT(fed.out().queued(), high + kChunk);
  std::vector<float> drained(static_cast<std::size_t>(high - low + kChunk));
  fed.out().render(drained.data(), drained.size());
  ASSERT_LT(fed.out().queued(), low);
  EXPECT_TRUE(eventually([&] { return fed.out().queued() >= high; }))
      << "below its low lead, it reads on";
  EXPECT_LT(fed.out().queued(), high + kChunk);
  EXPECT_FALSE(fed.feeder().stats().ended);
}

// The null output's own thread drains what the feeder's writes, on
// steady_clock: under ThreadSanitizer, the ring's two sides and the flush.
TEST(AudioFeeder, OnThreadsTheNullOutputPlaysWhatItWrites) {
  Scratch scratch;
  std::vector<Rec> records = {{0, 0, {}}};
  for (int i = 0; i < 100; ++i) {  // 1 s
    records.push_back({1, i * 10 * kMs, ramp(i, kChunk, 2)});
  }
  const std::string path = scratch.file("threads.vrst");
  ASSERT_TRUE(
      write_file(path, {geometry_track(), audio_track(2)}, records).ok());
  core::Result<std::unique_ptr<stream::StreamReader>> reader =
      stream::StreamReader::open(path);
  ASSERT_TRUE(reader.ok()) << reader.status().message();
  const std::optional<AudioTrackChoice> choice =
      find_audio_track((*reader)->session(), (*reader)->stats(), 0);
  ASSERT_TRUE(choice.has_value());
  core::Result<std::unique_ptr<NullAudioOut>> out =
      NullAudioOut::create(played_format(choice->params));
  ASSERT_TRUE(out.ok()) << out.status().message();
  (*out)->play();
  core::Result<std::unique_ptr<AudioFeeder>> feeder =
      AudioFeeder::start(**reader, *choice, 0, **out);
  ASSERT_TRUE(feeder.ok()) << feeder.status().message();
  EXPECT_TRUE(eventually([&] {
    const std::optional<std::uint64_t> at = (*out)->position();
    return at && *at >= 200 * kMs;
  })) << "the output never played on";
  (*feeder)->restart();
  EXPECT_TRUE(eventually([&] { return (*out)->stats().flushes == 2; }));
  (*feeder)->stop();
  EXPECT_EQ((*feeder)->stats().passes, 2u);
  EXPECT_EQ((*feeder)->stats().skipped, 0u);
}

}  // namespace
