// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The Apple output (open_apple_audio, device_audio_out.hpp), on macOS and
// iOS, under ARC: the AudioDevice a DeviceAudioOut drives, an AVAudioEngine
// built anew at each start, its AVAudioSourceNode's render block pulling from
// the output's ring, and the system's notices: the playing engine's
// configuration change starts it again (on macOS, a device that went away
// asks the player to pause first); on iOS the session is playback for movies,
// with no background audio, an interruption stops it and asks the player to
// pause, a route whose device went away asks it to pause, and a new route
// starts it again. The render block calls no Objective-C, and neither locks
// nor allocates. With VP_NO_AUDIO_DEVICE set, as for every test, it opens
// nothing.

#include "volumetric_kit/player/device_audio_out.hpp"

#import <AVFoundation/AVFoundation.h>
#include <TargetConditionals.h>
#include <mach/mach_time.h>
#if TARGET_OS_OSX
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace volumetric_kit::player {
namespace {

// The most frames a render block call renders at once; a larger call goes
// in pieces.
constexpr std::size_t kPieceFrames = 4096;

std::string Utf8(NSString* text) { return text != nil ? text.UTF8String : ""; }

core::Status Failed(const std::string& what, NSError* error) {
  return core::Status::io_error(
      what + ": " +
      (error != nil ? Utf8(error.localizedDescription) : "no reason given"));
}

#if TARGET_OS_OSX
AudioObjectID CurrentDevice(AVAudioEngine* engine) {
  AudioObjectID device = kAudioObjectUnknown;
  UInt32 size = sizeof device;
  AudioUnit unit = engine.outputNode.audioUnit;
  if (unit == nullptr ||
      AudioUnitGetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                           kAudioUnitScope_Global, 0, &device,
                           &size) != noErr) {
    return kAudioObjectUnknown;
  }
  return device;
}

std::string DeviceName(AudioObjectID device) {
  const AudioObjectPropertyAddress address{kAudioObjectPropertyName,
                                           kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain};
  CFStringRef name = nullptr;
  UInt32 size = sizeof name;
  if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &name) !=
          noErr ||
      name == nullptr) {
    return {};
  }
  return Utf8(CFBridgingRelease(name));
}

bool DeviceAlive(AudioObjectID device) {
  const AudioObjectPropertyAddress address{kAudioDevicePropertyDeviceIsAlive,
                                           kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain};
  UInt32 alive = 0;
  UInt32 size = sizeof alive;
  return AudioObjectGetPropertyData(device, &address, 0, nullptr, &size,
                                    &alive) == noErr &&
         alive != 0;
}
#endif

class AppleDevice;

// What a notice reaches, on the device's queue alone: closed once the
// device goes, so a notice in flight then does nothing.
struct Gate {
  bool open = true;
  AppleDevice* device = nullptr;
  DeviceAudioOut* out = nullptr;
};

class AppleDevice final : public AudioDevice {
 public:
  AppleDevice(DeviceAudioOut& out, const AudioFormat& format)
      : out_(out), format_(format), scratch_(kPieceFrames * format.channels) {
    mach_timebase_info_data_t timebase{};
    mach_timebase_info(&timebase);
    numer_ = timebase.numer;
    denom_ = timebase.denom;
    queue_ = dispatch_queue_create("volumetric_kit.player.audio",
                                   DISPATCH_QUEUE_SERIAL);
    gate_ = std::make_shared<Gate>();
    gate_->device = this;
    gate_->out = &out;
    observers_ = [NSMutableArray array];
    observe();
  }

  ~AppleDevice() override {
    for (id observer in observers_) {
      [NSNotificationCenter.defaultCenter removeObserver:observer];
    }
    std::shared_ptr<Gate> gate = gate_;
    dispatch_sync(queue_, ^{
      gate->open = false;
    });
    if (engine_ != nil) {  // the output stops it first
      stop(true);
    }
  }

  AppleDevice(const AppleDevice&) = delete;
  AppleDevice& operator=(const AppleDevice&) = delete;

  core::Status start() override {
    @try {
      NSError* error = nil;
#if TARGET_OS_IOS
      AVAudioSession* session = AVAudioSession.sharedInstance;
      // Set only when it differs: setting it posts a route change.
      if (![session.category isEqualToString:AVAudioSessionCategoryPlayback] ||
          ![session.mode isEqualToString:AVAudioSessionModeMoviePlayback]) {
        if (![session setCategory:AVAudioSessionCategoryPlayback
                             mode:AVAudioSessionModeMoviePlayback
                          options:0
                            error:&error]) {
          return Failed("the audio session's category", error);
        }
      }
      if (![session setActive:YES error:&error]) {
        return Failed("the audio session", error);
      }
#endif
      AVAudioFormat* format = [[AVAudioFormat alloc]
          initStandardFormatWithSampleRate:format_.sample_rate
                                  channels:format_.channels];
      AVAudioSourceNode* source =
          [[AVAudioSourceNode alloc] initWithFormat:format
                                        renderBlock:render_block()];
      AVAudioEngine* engine = [[AVAudioEngine alloc] init];
      [engine attachNode:source];
      if (@available(macOS 27.0, iOS 27.0, *)) {
        if (![engine connect:source
                          to:engine.mainMixerNode
                      format:format
                       error:&error]) {
          return Failed("AVAudioEngine's connection", error);
        }
      } else {
        // iOS 26, the family's floor, has only the call 27 deprecates.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [engine connect:source to:engine.mainMixerNode format:format];
#pragma clang diagnostic pop
      }
      // Its notices count from here; another engine's, an earlier one's
      // included, are not this one's.
      engine_id_.store((__bridge const void*)engine, std::memory_order_release);
      [engine prepare];
      if (![engine startAndReturnError:&error]) {
        engine_id_.store(nullptr, std::memory_order_release);
        return Failed("AVAudioEngine's start", error);
      }
      engine_ = engine;
      route_ = DeviceRoute{};
      route_.presentation_latency_s = engine.outputNode.presentationLatency;
#if TARGET_OS_IOS
      route_.session_latency_s = session.outputLatency;
      route_.name = Utf8(session.currentRoute.outputs.firstObject.portName);
      // For the owner's check on a phone, which has no vp_audio_check:
      // whether the presentation latency already holds the session's.
      NSLog(@"volumetric_kit_player: audio route %s, presentation %.1f ms, "
            @"session %.1f ms",
            route_.name.c_str(), route_.presentation_latency_s * 1e3,
            route_.session_latency_s * 1e3);
#else
      const AudioObjectID device = CurrentDevice(engine);
      device_.store(device, std::memory_order_relaxed);
      route_.name = DeviceName(device);
#endif
      return {};
    } @catch (NSException* exception) {
      engine_id_.store(nullptr, std::memory_order_release);
      return core::Status::io_error("AVAudioEngine: " + Utf8(exception.reason));
    }
  }

  void stop(bool release) noexcept override {
    engine_id_.store(nullptr, std::memory_order_release);
    @try {
      [engine_ stop];
      engine_ = nil;
#if TARGET_OS_IOS
      if (release) {
        [AVAudioSession.sharedInstance
              setActive:NO
            withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                  error:nil];
      }
#else
      static_cast<void>(release);
#endif
    } @catch (NSException*) {
      engine_ = nil;  // stopped as far as it goes
    }
  }

  DeviceRoute route() const override { return route_; }

  std::uint64_t host_now_ns() const noexcept override {
    return host_ticks_to_ns(mach_absolute_time(), numer_, denom_);
  }

  // The device's queue's: whether `engine`, a notice's, is the one playing.
  // The notice keeps it alive, so no later engine can share its address.
  bool plays_through(id engine) const {
    return engine != nil && (__bridge const void*)engine ==
                                engine_id_.load(std::memory_order_acquire);
  }

#if TARGET_OS_OSX
  // The device's queue's: whether the device the engine played through went
  // away.
  bool device_gone() const {
    const AudioObjectID device = device_.load(std::memory_order_relaxed);
    return device != kAudioObjectUnknown && !DeviceAlive(device);
  }
#endif

 private:
  // The source node's render block: the output's frames, interleaved into
  // the scratch, then into the engine's buffer per channel. It captures no
  // Objective-C object and calls none.
  AVAudioSourceNodeRenderBlock render_block() {
    DeviceAudioOut* const out = &out_;
    float* const scratch = scratch_.data();
    const std::uint16_t channels = format_.channels;
    const std::uint32_t rate = format_.sample_rate;
    const std::uint32_t numer = numer_;
    const std::uint32_t denom = denom_;
    return ^OSStatus(BOOL* silence, const AudioTimeStamp* stamp,
                     AVAudioFrameCount frames, AudioBufferList* data) {
      const std::uint64_t ticks =
          (stamp->mFlags & kAudioTimeStampHostTimeValid) != 0
              ? stamp->mHostTime
              : mach_absolute_time();
      const std::uint64_t host = host_ticks_to_ns(ticks, numer, denom);
      for (AVAudioFrameCount done = 0; done < frames;) {
        const std::size_t piece =
            std::min<std::size_t>(frames - done, kPieceFrames);
        out->render_at(scratch, piece, host + audio_ns(done, rate));
        for (UInt32 b = 0; b < data->mNumberBuffers; ++b) {
          float* const to = static_cast<float*>(data->mBuffers[b].mData) + done;
          const std::size_t channel = std::min<std::size_t>(b, channels - 1);
          for (std::size_t i = 0; i < piece; ++i) {
            to[i] = scratch[i * channels + channel];
          }
        }
        done += static_cast<AVAudioFrameCount>(piece);
      }
      *silence = NO;
      return noErr;
    };
  }

  // `handle` each notice `name` posts on the queue while the gate is open.
  void on(NSNotificationName name, void (^handle)(Gate&, NSNotification*)) {
    std::shared_ptr<Gate> gate = gate_;
    dispatch_queue_t queue = queue_;
    id observer = [NSNotificationCenter.defaultCenter
        addObserverForName:name
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification* note) {
                  dispatch_async(queue, ^{
                    if (gate->open) {
                      handle(*gate, note);
                    }
                  });
                }];
    [observers_ addObject:observer];
  }

  // The system's notices.
  void observe() {
    // Every engine's, as the device makes one at each start: the playing
    // one's alone count.
    on(AVAudioEngineConfigurationChangeNotification,
       ^(Gate& gate, NSNotification* note) {
         if (!gate.device->plays_through(note.object)) {
           return;
         }
#if TARGET_OS_OSX
         if (gate.device->device_gone()) {
           gate.out->device_went_away();
         }
#endif
         gate.out->configuration_changed();
       });
#if TARGET_OS_IOS
    on(AVAudioSessionInterruptionNotification, ^(Gate& gate,
                                                 NSNotification* note) {
      const NSUInteger type = [note.userInfo[AVAudioSessionInterruptionTypeKey]
          unsignedIntegerValue];
      if (type == AVAudioSessionInterruptionTypeBegan) {
        gate.out->interruption_began();
      } else {
        gate.out->interruption_ended();
      }
    });
    on(AVAudioSessionRouteChangeNotification, ^(Gate& gate,
                                                NSNotification* note) {
      const NSUInteger reason =
          [note.userInfo[AVAudioSessionRouteChangeReasonKey]
              unsignedIntegerValue];
      const bool gone =
          reason == AVAudioSessionRouteChangeReasonOldDeviceUnavailable;
      if (gone) {
        gate.out->device_went_away();
      }
      // A new route has a latency of its own. The engine's own notice, if
      // it came first, has restarted it already: this restarts it again.
      if (gone || reason == AVAudioSessionRouteChangeReasonNewDeviceAvailable ||
          reason == AVAudioSessionRouteChangeReasonOverride) {
        gate.out->configuration_changed();
      }
    });
    on(AVAudioSessionMediaServicesWereResetNotification,
       ^(Gate& gate, NSNotification*) {
         gate.out->configuration_changed();
       });
#endif
  }

  DeviceAudioOut& out_;
  const AudioFormat format_;
  std::vector<float> scratch_;  // the render block's
  std::uint32_t numer_ = 1;     // the host clock's timebase
  std::uint32_t denom_ = 1;
  dispatch_queue_t queue_;
  std::shared_ptr<Gate> gate_;
  NSMutableArray* observers_;
  // Under the output's control mutex: the engine playing, and its route.
  AVAudioEngine* engine_ = nil;
  DeviceRoute route_;
  // The engine whose notices count, from before its start until its stop;
  // the queue reads it.
  std::atomic<const void*> engine_id_{nullptr};
#if TARGET_OS_OSX
  std::atomic<AudioObjectID> device_{kAudioObjectUnknown};  // the queue reads
#endif
};

}  // namespace

core::Result<std::unique_ptr<DeviceAudioOut>> open_apple_audio(
    const AudioFormat& format) {
  if (std::getenv("VP_NO_AUDIO_DEVICE") != nullptr) {  // the tests' backstop
    return core::Status::unsupported(
        "VP_NO_AUDIO_DEVICE is set, so no audio device");
  }
  return DeviceAudioOut::create(
      format,
      [format](
          DeviceAudioOut& out) -> core::Result<std::unique_ptr<AudioDevice>> {
        try {
          return std::unique_ptr<AudioDevice>(new AppleDevice(out, format));
        } catch (const std::bad_alloc&) {
          return core::Status::out_of_memory("the Apple output: no room");
        }
      });
}

}  // namespace volumetric_kit::player
