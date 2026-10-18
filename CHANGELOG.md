# Changelog

All notable changes to `volumetric_kit_player` are documented here. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- The repository's skeleton: CMake, the agents' guide, the formatting hooks,
  and CI that builds and tests on the family's self-hosted Mac and Linux GPU
  runners, with lint and the `required` gate hosted.
- `player::Playback`, the clock that paces a stream's frames by their
  timestamps, with play, pause and step.
- `player::StreamPlayer`, which plays a `.vrst` stream's geometry on a worker
  thread: each frame decoded and meshed on the GPU when it is due, resynced
  at the next key after a damaged record, looped or held at the end, and
  handed to the drawing thread through recon's `MeshExchange`. Core and recon
  are fetched pinned.
