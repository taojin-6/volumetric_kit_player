// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file test_stream.hpp
/// The tests' streams, which vp_write_stream writes once a ctest run
/// (CMakeLists.txt): kFrames frames of a sphere moving a block a frame, at 25
/// fps with a pause before the fourth, an I-frame every third, each record in
/// the set set_of names, frame kNoSetFrame's in none, each block
/// of each frame with an unobserved voxel, so masked, which grid's deblock
/// keeps at its exact decode; the same with frame kEmptyFrame meshing
/// nothing; coded as a true SDF observed far enough out that no voxel is
/// masked, so the deblock filters every block, its filter "R" (T 1.96 mm);
/// at twice the voxel size; and with frame kGrownFrame a sphere so large that
/// its blocks nearly fill the decode grid the first frame's sizes (grid's
/// make_decode_grid: 8,192 slots), so the table refuses some on the device,
/// and the codec grows the grid and decodes it again at its finish; and with
/// that frame a sphere of more blocks than those slots, so the codec grows
/// the grid before it records the frame. A copy of one of them, with an
/// Audio track added or not, and a stream that opens but never decodes, in a
/// scratch directory of the test's own.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/grid/stream/audio.hpp"
#include "volumetric_kit/grid/stream/session.hpp"
#include "volumetric_kit/grid/stream/stream_reader.hpp"
#include "volumetric_kit/grid/stream/stream_writer.hpp"

namespace vp_test {

namespace core = volumetric_kit::core;
namespace grid = volumetric_kit::grid;

constexpr std::uint64_t kFrames = 6;
constexpr std::uint32_t kIntraPeriod = 3;
constexpr std::uint64_t kPeriodNs = 40'000'000;  // 25 fps
// The pause before frame kGapAt, so a pts is not its index times the period.
constexpr std::uint64_t kGapAt = 3;
constexpr std::uint64_t kGapNs = 500'000'000;

// The streams' names in vp_write_stream's directory: the sphere; with frame
// kEmptyFrame holding no block, so meshing nothing; as a true SDF, unmasked;
// at twice the voxel size; with frame kGrownFrame, a key after the pause,
// past what the decode grid's table can place; and with that frame past the
// decode grid's slots.
constexpr const char* kStream = "sphere.vrst";
constexpr const char* kEmptyFrameStream = "empty_frame.vrst";
constexpr const char* kTrueSdfStream = "true_sdf.vrst";
constexpr const char* kCoarseStream = "coarse.vrst";
constexpr const char* kGrowStream = "grow.vrst";
constexpr const char* kOutgrowStream = "outgrow.vrst";
constexpr std::uint64_t kEmptyFrame = 1;
constexpr std::uint64_t kGrownFrame = kGapAt;
// The decode grid the first frame sizes (grid's make_decode_grid: 1,024
// buckets of 8), and the most free blocks below which its table places no
// block outside its home bucket (an eighth): frame kGrownFrame holds more
// than the slots less that, and no more than the slots.
constexpr std::uint32_t kFirstGridSlots = 8192;
constexpr std::uint32_t kTableFloor = kFirstGridSlots / 8;

// Frame i's pts.
inline std::uint64_t pts_of(std::uint64_t i) {
  return i * kPeriodNs + (i >= kGapAt ? kGapNs : 0);
}

// The frame whose record names no set.
constexpr std::uint64_t kNoSetFrame = 4;

// The set frame i's record names (grid's RecordInfo::set): 500 on from the
// first, apart from its index and seq; grid's kNoSet for kNoSetFrame.
inline std::uint64_t set_of(std::uint64_t i) {
  return i == kNoSetFrame ? grid::stream::kNoSet : 500 + i;
}

// A directory of the test's own, removed with what it holds.
class Scratch {
 public:
  Scratch() {
    const ::testing::TestInfo* test =
        ::testing::UnitTest::GetInstance()->current_test_info();
    path_ =
        std::filesystem::temp_directory_path() /
        ("vp_" + std::string(test->name()) + "_" + std::to_string(::getpid()));
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

  // The tests' stream `name` (kStream, ...), copied from `dir`, where ctest's
  // vp_test_streams wrote it (VP_TEST_STREAMS), so the test may change its
  // copy; its path, or empty with a failure recorded.
  std::string copy(const char* dir, const char* name) const {
    const std::filesystem::path from = std::filesystem::path(dir) / name;
    std::error_code ec;
    std::filesystem::copy_file(from, path_ / name, ec);
    if (ec) {
      ADD_FAILURE() << "no " << from.string() << " (" << ec.message()
                    << "): ctest's vp_test_streams writes it";
      return {};
    }
    return file(name);
  }

 private:
  std::filesystem::path path_;
};

// A stream that opens, with no device, but never decodes: a 25 fps geometry
// track of `records` one-byte key records; with `audio`, an Audio track too,
// 8 kHz mono PCM, a frame of silence at each geometry record's pts.
inline core::Status write_undecodable_stream(const std::string& path,
                                             std::uint64_t records,
                                             bool audio = false) {
  namespace stream = grid::stream;
  stream::Session session;
  session.writer = "volumetric_kit_player tests";
  stream::Track track;
  track.kind = stream::TrackKind::Geometry;
  track.rate = {25, 1};
  session.tracks.push_back(track);
  if (audio) {
    stream::Track sound;
    sound.kind = stream::TrackKind::Audio;
    sound.rate = {8000, 1};
    stream::AudioParams params;
    params.sample_rate = 8000;
    sound.params = stream::encode_audio_params(params);
    session.tracks.push_back(sound);
  }
  VKC_ASSIGN(std::unique_ptr<stream::StreamWriter> writer,
             stream::StreamWriter::create(path, session));
  const std::uint8_t bytes[2] = {0, 0};
  for (std::uint64_t i = 0; i < records; ++i) {
    stream::RecordInfo record;
    record.key = true;
    record.pts_ns = pts_of(i);
    record.seq = i;
    VKC_TRY(writer->append(record, bytes, 1));
    if (audio) {
      record.track = 1;
      VKC_TRY(writer->append(record, bytes, 2));
    }
  }
  return writer->finish();
}

// The stream at `from` copied to `to` with an Audio track added, on the
// first track's clock: 8 kHz mono PCM, a frame of silence at each record's
// pts.
inline core::Status copy_with_audio(const std::string& from,
                                    const std::string& to) {
  namespace stream = grid::stream;
  VKC_ASSIGN(std::unique_ptr<stream::StreamReader> reader,
             stream::StreamReader::open(from));
  stream::Session session = reader->session();
  stream::Track sound;
  sound.kind = stream::TrackKind::Audio;
  sound.clock = session.tracks.at(0).clock;
  sound.rate = {8000, 1};
  stream::AudioParams params;
  params.sample_rate = 8000;
  sound.params = stream::encode_audio_params(params);
  session.tracks.push_back(sound);
  const auto audio_track =
      static_cast<std::uint16_t>(session.tracks.size() - 1);
  VKC_ASSIGN(std::unique_ptr<stream::StreamWriter> writer,
             stream::StreamWriter::create(to, session));
  stream::Position pos = reader->begin();
  stream::Record record;
  std::vector<std::uint8_t> payload;
  const std::uint8_t silence[2] = {0, 0};
  for (std::uint64_t seq = 0;; ++seq) {
    VKC_ASSIGN(const bool more, reader->next(&pos, &record));
    if (!more) {
      break;
    }
    VKC_TRY(reader->read(record, &payload));
    VKC_TRY(writer->append(record.info, payload.data(), payload.size()));
    stream::RecordInfo info;
    info.track = audio_track;
    info.key = true;
    info.pts_ns = record.info.pts_ns;
    info.seq = seq;
    VKC_TRY(writer->append(info, silence, 2));
  }
  return writer->finish();
}

}  // namespace vp_test
