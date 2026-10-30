// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file device_audio_out.hpp
/// @brief An output that plays through a device: DeviceAudioOut, with the
///        clock that says what is heard when and the lifecycle of parks,
///        interruptions and route changes; the AudioDevice it drives; and on
///        macOS and iOS the Apple device (open_apple_audio).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/player/audio_clock.hpp"
#include "volumetric_kit/player/audio_out.hpp"

namespace volumetric_kit::player {

/// @brief A device's route, as read at its start.
struct DeviceRoute {
  std::string name;  ///< E.g. "MacBook Pro Speakers" or "AirPods Pro".
  /// The output's presentation latency (`AVAudioIONode.presentationLatency`).
  double presentation_latency_s = 0.0;
  /// The route's (`AVAudioSession.outputLatency`); 0 on macOS.
  double session_latency_s = 0.0;
};

class DeviceAudioOut;

/// @brief The device a DeviceAudioOut drives: the seam between its logic,
///        tested with a fake, and an OS's audio engine.
///
/// @code
/// class FakeDevice final : public AudioDevice {
///   core::Status start() override { return {}; }
///   void stop(bool) noexcept override {}
///   DeviceRoute route() const override { return {"fake", 0.01, 0.0}; }
///   std::uint64_t host_now_ns() const noexcept override { return now; }
///   std::uint64_t now = 0;
/// };
/// @endcode
class AudioDevice {
 public:
  virtual ~AudioDevice() = default;
  /// @brief Start playing: until @ref stop, the device's render thread
  ///        calls DeviceAudioOut::render_at. On iOS it activates the audio
  ///        session first.
  /// @return OK, or why it cannot play.
  virtual core::Status start() = 0;
  /// @brief Stop playing: render_at is not called once it returns.
  /// @param release  Give the route back too (on iOS, deactivate the
  ///                 session), as a park does.
  virtual void stop(bool release) noexcept = 0;
  /// @return Its route, as of its last start.
  virtual DeviceRoute route() const = 0;
  /// @return The host clock's time now, in ns: the clock render_at's times
  ///         are on.
  virtual std::uint64_t host_now_ns() const noexcept = 0;
};

/// @brief An AudioOut that plays through an AudioDevice, as the Apple
///        output does through AVAudioEngine.
///
/// **What is heard when.** Each render_at says when its first frame passes
/// the device's output, on the host clock; @ref position maps the frames
/// rendered, less those still queued in the device and their run's latency,
/// onto the host clock's now (heard_pts). Runs of frames follow the
/// re-anchor rule (reanchors); a device start always begins a new run, at
/// the latency of the route read at that start (@ref output_latency_ns), and
/// until it is read the start's renders play silence and take nothing. While
/// the device is stopped the position holds at what was heard as it
/// stopped. The host clock is read at each call, never mapped through a
/// stored offset to steady_clock, so a sleep, which host time skips and
/// steady_clock counts, leaves nothing stale.
///
/// **Lifecycle.** The device plays while the output is neither parked nor
/// interrupted. @ref park stops it and gives the route back; @ref unpark
/// starts it again, ending an interruption too, as one may never end. An
/// interruption stops it and asks the player to pause
/// (@ref take_pause_request); its end starts it, paused as the player is. A
/// configuration change (a new route or device) starts it again, re-reading
/// the route; a device that went away asks the player to pause. A start that
/// fails leaves it stopped, @ref route not audible, until the next.
///
/// **Threads.** The device's render thread calls render_at, which neither
/// locks nor allocates, and publishes the clock to @ref position through a
/// sequence lock it never waits on. The lifecycle calls are safe from any
/// thread, under a mutex of their own, which no render takes and which is
/// held across the device's starts and stops; @ref route reads what the
/// last start or stop left, under another, held for a copy alone.
///
/// @code
/// VKC_ASSIGN(std::unique_ptr<DeviceAudioOut> out,
///            DeviceAudioOut::create(format, [](DeviceAudioOut& out) {
///              return make_device(out);  // its render calls out.render_at
///            }));
/// out->flush(first_pts_ns);
/// out->play();
/// const std::optional<std::uint64_t> heard = out->position();
/// @endcode
class DeviceAudioOut final : public AudioOut {
 public:
  /// @brief Makes the device an output plays through, its render calling
  ///        that output's render_at.
  using DeviceMaker = std::function<core::Result<std::unique_ptr<AudioDevice>>(
      DeviceAudioOut&)>;

  /// @brief An output of @p format through the device @p make makes,
  ///        started, paused.
  /// @param format  What it plays.
  /// @param make    Makes its device.
  /// @return The output; `core::Status::Code::InvalidArgument` for a sample
  ///         rate of 0, other than 1 or 2 channels, or a maker that made
  ///         none; `OutOfMemory` when its ring cannot be allocated; or the
  ///         maker's or the device's first start's failure.
  static core::Result<std::unique_ptr<DeviceAudioOut>> create(
      const AudioFormat& format, const DeviceMaker& make);

  /// @brief Stops the device and destroys it, which waits out its notices
  ///        in flight.
  ~DeviceAudioOut() override;

  /// @brief The device's render thread's: the next @p frames frames into
  ///        @p out (AudioOut::render), and when they go out. It neither
  ///        locks nor allocates.
  /// @param out      Room for @p frames interleaved frames.
  /// @param frames   How many.
  /// @param host_ns  When the first passes the device's output, on the host
  ///                 clock.
  void render_at(float* out, std::size_t frames,
                 std::uint64_t host_ns) noexcept;

  /// @return The pts heard now: heard_pts of the renders' clock at the host
  ///         clock's now, each run less its latency; none before a flush.
  std::optional<std::uint64_t> position() const noexcept override;
  /// @return Audible while the device plays; the latency and name of the
  ///         route as of its last start. It never waits for a start or stop
  ///         under way.
  AudioRoute route() const override;
  /// @brief Stop the device and give the route back, until @ref unpark.
  void park() override;
  /// @brief Start the device again, and end an interruption whose end never
  ///        came (Apple's guidance: try again when the app is active).
  void unpark() override;
  /// @return Whether an interruption or a device gone asked the player to
  ///         pause since the last call.
  bool take_pause_request() noexcept override;

  /// @brief The system interrupted the device (a call, Siri): it is
  ///        stopped, and the player asked to pause.
  void interruption_began();
  /// @brief The interruption ended: start the device, unless parked.
  void interruption_ended();
  /// @brief The device's configuration changed (a route or device change,
  ///        e.g. AirPods): start it again, unless parked or interrupted,
  ///        re-reading the route; the next render begins a new run.
  void configuration_changed();
  /// @brief The device played through went away: ask the player to pause.
  void device_went_away() noexcept;

  /// @return The device's route as of its last start, the latency's parts
  ///         apart, for the check tool.
  DeviceRoute device_route() const;
  /// @return The runs of frames its clock has begun (reanchors), for the
  ///         check tool: one a start, a flush or an underrun, more if the
  ///         device's render times jump.
  std::uint64_t runs() const noexcept {
    return runs_.load(std::memory_order_relaxed);
  }

 private:
  explicit DeviceAudioOut(const AudioFormat& format);

  // Under control_mutex_: start the device if it should play and does not;
  // and stop it if it plays.
  core::Status run_device_locked();
  void stop_device_locked(bool release);

  // The clock as the render thread last published it.
  HeardClock published() const noexcept;
  void publish(const HeardClock& clock) noexcept;

  // The render thread's.
  HeardClock clock_;
  std::uint64_t seen_starts_ = 0;

  // The clock, published under a sequence lock: seq_ is odd while written.
  std::atomic<std::uint64_t> seq_{0};
  std::atomic<bool> based_{false};
  std::atomic<std::uint64_t> floor_{0};
  std::atomic<bool> has_current_{false};
  std::atomic<std::uint64_t> current_first_{0};
  std::atomic<std::uint64_t> current_end_{0};
  std::atomic<std::uint64_t> current_end_host_{0};
  std::atomic<std::uint64_t> current_latency_{0};
  std::atomic<bool> has_previous_{false};
  std::atomic<std::uint64_t> previous_first_{0};
  std::atomic<std::uint64_t> previous_end_{0};
  std::atomic<std::uint64_t> previous_end_host_{0};
  std::atomic<std::uint64_t> previous_latency_{0};

  // Any thread's: device starts begun; the last whose route was read, and
  // that route's latency, which its renders wait for; when the device
  // stopped on the host clock (0 while it plays); a pause asked for; and the
  // runs begun.
  std::atomic<std::uint64_t> starts_{0};
  std::atomic<std::uint64_t> ready_{0};
  std::atomic<std::uint64_t> latency_ns_{0};
  std::atomic<std::uint64_t> stopped_at_{0};
  std::atomic<bool> pause_requested_{false};
  std::atomic<std::uint64_t> runs_{0};

  // The lifecycle, under control_mutex_, held across the device's calls.
  mutable std::mutex control_mutex_;
  bool parked_ = false;
  bool interrupted_ = false;
  bool running_ = false;
  std::unique_ptr<AudioDevice> device_;

  // What route() reads, as the last start or stop left it, under
  // route_mutex_, held for a copy alone.
  mutable std::mutex route_mutex_;
  bool audible_ = false;
  DeviceRoute route_;
};

#if defined(__APPLE__)
/// @brief The Apple output, on macOS and iOS: AVAudioEngine with an
///        AVAudioSourceNode whose render block pulls from the ring
///        (apple_audio_out.mm). On iOS it sets the audio session's category
///        to playback, mode movie playback, with no background audio.
///
/// @code
/// VKC_ASSIGN(std::unique_ptr<DeviceAudioOut> out, open_apple_audio(format));
/// @endcode
/// @param format  What it plays.
/// @return The output, started, paused; `core::Status::Code::Unsupported`,
///         touching no device, while `VP_NO_AUDIO_DEVICE` is set (every
///         test's, AGENTS.md); DeviceAudioOut::create's refusals; or the
///         engine's or the session's failure.
core::Result<std::unique_ptr<DeviceAudioOut>> open_apple_audio(
    const AudioFormat& format);
#endif

}  // namespace volumetric_kit::player
