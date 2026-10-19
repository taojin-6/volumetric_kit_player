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
- `player::StreamPlayer`: plays a `.vrst` stream's geometry on a worker
  thread, paced by its pts. The worker builds its decoder and extractor, so
  `start` returns at once and a failed build reaches `status()`.
- `player::PlayerView`: the window side of playback on any window system,
  over gfx: the shared device, a FIFO swapchain, and the newest mesh under an
  orbit camera and a controls panel, one `frame(extent)` per tick.
