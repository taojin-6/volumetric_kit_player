// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The iOS shell over PlayerView (DECISIONS.md), all on main: a MetalView hosts
// the CAMetalLayer, a display link ticks the view, every touch phase reaches
// gfx's ImGuiTouch for the controls panel, three recognizers drive the camera
// unless the panel holds the finger, and the scene's activation parks and
// unparks the worker. It plays the first .vrst, by name, in the app's
// Documents folder, which the Files app and Finder show (Info.plist.in).

#import "PlayerViewController.h"

#import <QuartzCore/QuartzCore.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

#include <glm/vec2.hpp>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"
#include "volumetric_kit/gfx/ui/imgui_touch.hpp"
#include "volumetric_kit/gfx/windowing/surface_source.hpp"
#include "volumetric_kit/player/player_view.hpp"
#include "volumetric_kit/player/stream_player.hpp"

namespace vkc = volumetric_kit::core;
namespace vg = volumetric_kit::gfx;
namespace vkp = volumetric_kit::player;

namespace {

// How long a resign-active waits for the worker's step under way.
constexpr std::chrono::seconds kParkWait{1};

// The first .vrst, by name, in Documents; nil when there is none, and then
// a note is left there, as the Files app may hide an empty app folder
// (DECISIONS.md).
NSString* FirstStream() {
  NSFileManager* files = NSFileManager.defaultManager;
  NSString* documents = NSSearchPathForDirectoriesInDomains(
                            NSDocumentDirectory, NSUserDomainMask, YES)
                            .firstObject;
  NSArray<NSString*>* names = [files contentsOfDirectoryAtPath:documents
                                                         error:nil];
  names = [names sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
  for (NSString* name in names) {
    if ([name.pathExtension caseInsensitiveCompare:@"vrst"] == NSOrderedSame) {
      return [documents stringByAppendingPathComponent:name];
    }
  }
  NSString* note =
      [documents stringByAppendingPathComponent:@"Copy .vrst streams here.txt"];
  NSData* text = [@"The player plays the first .vrst here, by name.\n"
      dataUsingEncoding:NSUTF8StringEncoding];
  if (![files fileExistsAtPath:note]) {
    [files createFileAtPath:note contents:text attributes:nil];
  }
  return nil;
}

// The view over the first stream, on `layer`, with fingertip-sized controls.
// No validation: iOS has no layer to load (DECISIONS.md).
vkc::Result<std::unique_ptr<vkp::PlayerView>> MakeView(CAMetalLayer* layer,
                                                       VkExtent2D pixels) {
  NSString* path = FirstStream();
  if (path == nil) {
    return vkc::Status::not_found(
        "No .vrst stream to play. Copy one into this app's folder with the "
        "Files app or Finder, then come back to the app.");
  }
  VKC_ASSIGN(std::unique_ptr<vkp::StreamPlayer> player,
             vkp::StreamPlayer::open(path.UTF8String));
  VKC_ASSIGN(const vg::windowing::SurfaceSource surface,
             vg::windowing::metal_surface((__bridge const void*)layer));
  vkp::PlayerViewConfig config;
  config.ui_scale = 1.3f;              // 17-pt text, near iOS body text
  config.ui_min_frame_height = 44.0f;  // Apple's least touch target
  return vkp::PlayerView::create(surface, pixels, std::move(player), config);
}

std::uintptr_t Finger(UITouch* touch) {
  return reinterpret_cast<std::uintptr_t>((__bridge void*)touch);
}

// In points, from the view's top-left.
glm::vec2 At(UITouch* touch, UIView* view) {
  const CGPoint at = [touch locationInView:view];
  return {static_cast<float>(at.x), static_cast<float>(at.y)};
}

}  // namespace

// The view whose layer is the CAMetalLayer the surface wraps.
@interface MetalView : UIView
@property(nonatomic, readonly) VkExtent2D pixels;  ///< The bounds, in pixels.
@end

@implementation MetalView

+ (Class)layerClass {
  return [CAMetalLayer class];
}

// Pixels at the screen's native scale. MoltenVK sizes the drawable from the
// bounds and this scale, so nothing here writes drawableSize.
- (void)didMoveToWindow {
  [super didMoveToWindow];
  UIScreen* screen = self.window.windowScene.screen;
  if (screen != nil) {
    self.contentScaleFactor = screen.nativeScale;
    self.layer.contentsScale = screen.nativeScale;
  }
}

- (VkExtent2D)pixels {
  const CGFloat scale = self.contentScaleFactor;
  return {static_cast<std::uint32_t>(self.bounds.size.width * scale),
          static_cast<std::uint32_t>(self.bounds.size.height * scale)};
}

@end

@interface PlayerViewController () <UIGestureRecognizerDelegate>
- (void)tick:(CADisplayLink*)link;
@end

// The display link's target: a link retains its target until invalidated, so
// this holds the controller weakly.
@interface LinkProxy : NSObject
@property(nonatomic, weak) PlayerViewController* owner;
@end

@implementation LinkProxy
- (void)tick:(CADisplayLink*)link {
  [self.owner tick:link];
}
@end

@implementation PlayerViewController {
  MetalView* _metal;
  UILabel* _failure;  // the one native view: why nothing plays
  CADisplayLink* _link;
  UIPanGestureRecognizer* _orbit;  // one finger
  UIPanGestureRecognizer* _pan;    // two fingers, with the pinch
  UIPinchGestureRecognizer* _pinch;
  std::unique_ptr<vkp::PlayerView> _view;
  std::unique_ptr<vg::ui::ImGuiTouch> _touch;  // after _view: goes first
  BOOL _failed;   // the label is up until the next activation
  BOOL _retried;  // a frame() error rebuilt the view in this activation
  BOOL _rebuild;  // a park timed out, so the device may be lost
}

- (void)loadView {
  _metal = [[MetalView alloc] init];
  _metal.backgroundColor = UIColor.blackColor;
  _metal.multipleTouchEnabled = YES;  // for the recognizers' second finger
  self.view = _metal;

  _failure = [[UILabel alloc] init];
  _failure.translatesAutoresizingMaskIntoConstraints = NO;
  _failure.numberOfLines = 0;
  _failure.textAlignment = NSTextAlignmentCenter;
  _failure.textColor = UIColor.whiteColor;
  _failure.hidden = YES;
  [_metal addSubview:_failure];
  UILayoutGuide* safe = _metal.safeAreaLayoutGuide;
  [NSLayoutConstraint activateConstraints:@[
    [_failure.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
    [_failure.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor
                                           constant:24],
    [_failure.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor
                                            constant:-24],
  ]];

  _orbit = [[UIPanGestureRecognizer alloc] initWithTarget:self
                                                   action:@selector(onOrbit:)];
  _orbit.maximumNumberOfTouches = 1;
  _pan = [[UIPanGestureRecognizer alloc] initWithTarget:self
                                                 action:@selector(onPan:)];
  _pan.minimumNumberOfTouches = 2;
  _pan.maximumNumberOfTouches = 2;
  _pinch =
      [[UIPinchGestureRecognizer alloc] initWithTarget:self
                                                action:@selector(onPinch:)];
  for (UIGestureRecognizer* recognizer in @[ _orbit, _pan, _pinch ]) {
    recognizer.delegate = self;
    [_metal addGestureRecognizer:recognizer];
  }
}

- (void)dealloc {
  [self stopLink];
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  [self bringUpIfReady];
}

// Bring-up submits GPU work, so only in the foreground and active, with a
// drawable. Lifecycle callbacks also come on main, so none interleaves.
- (void)bringUpIfReady {
  if (_view || _failed) {
    return;
  }
  // With no window, or a window with no scene, the state would read as
  // ForegroundActive, which is 0.
  UIWindowScene* scene = self.view.window.windowScene;
  if (scene == nil ||
      scene.activationState != UISceneActivationStateForegroundActive) {
    return;
  }
  const VkExtent2D pixels = _metal.pixels;
  if (pixels.width == 0 || pixels.height == 0) {
    return;
  }
  const CFTimeInterval start = CACurrentMediaTime();
  vkc::Result<std::unique_ptr<vkp::PlayerView>> made =
      MakeView((CAMetalLayer*)_metal.layer, pixels);
  if (!made.ok()) {
    [self fail:made.status()];
    return;
  }
  vkc::Result<std::unique_ptr<vg::ui::ImGuiTouch>> touch =
      vg::ui::ImGuiTouch::create(made.value()->ui_context());
  if (!touch.ok()) {
    [self fail:touch.status()];
    return;
  }
  _view = std::move(made).value();
  _touch = std::move(touch).value();
  _failure.hidden = YES;
  NSLog(@"volumetric_kit_player: view up in %.0f ms",
        (CACurrentMediaTime() - start) * 1e3);
  [self startLink];
}

- (void)tick:(CADisplayLink*)link {
  const VkExtent2D pixels = _metal.pixels;
  if (!_view || pixels.width == 0 || pixels.height == 0) {
    return;  // ImGuiTouch::new_frame needs a drawable
  }
  const UIEdgeInsets safe = _metal.safeAreaInsets;
  vg::ui::DisplayFrame display;
  display.pixels = pixels;
  display.pixels_per_point = static_cast<float>(_metal.contentScaleFactor);
  display.safe_area = {
      static_cast<float>(safe.top), static_cast<float>(safe.left),
      static_cast<float>(safe.bottom), static_cast<float>(safe.right)};
  display.seconds = link.timestamp;
  _touch->new_frame(display);
  const vkc::Result<bool> presented = _view->frame(pixels);
  if (!presented.ok()) {
    [self drop:presented.status()];
  }
}

// A frame() error, a lost device included: build anew once per activation,
// then show the label.
- (void)drop:(const vkc::Status&)status {
  [self dropView];
  if (_retried) {
    [self fail:status];
    return;
  }
  _retried = YES;
  [self bringUpIfReady];
}

- (void)fail:(const vkc::Status&)status {
  [self dropView];
  _failed = YES;
  NSString* message = @(status.message().c_str());  // UTF-8, unlike %s
  _failure.text = message;
  _failure.hidden = NO;
  NSLog(@"volumetric_kit_player: %@", message);
}

- (void)dropView {
  [self stopLink];
  _touch.reset();  // the platform half first; the view waits for the GPU
  _view.reset();
}

- (void)startLink {
  if (_link != nil || !_view) {
    return;
  }
  LinkProxy* proxy = [[LinkProxy alloc] init];
  proxy.owner = self;
  _link = [CADisplayLink displayLinkWithTarget:proxy selector:@selector(tick:)];
  [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}

- (void)stopLink {
  [_link invalidate];
  _link = nil;
}

// The controls: every touch phase, in points.
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  for (UITouch* touch in touches) {
    if (_touch) {
      _touch->touch_began(Finger(touch), At(touch, _metal));
    }
  }
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  for (UITouch* touch in touches) {
    if (_touch) {
      _touch->touch_moved(Finger(touch), At(touch, _metal));
    }
  }
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  for (UITouch* touch in touches) {
    if (_touch) {
      _touch->touch_ended(Finger(touch));
    }
  }
}

// A recognizer that took the finger, or the system: no widget is pressed.
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  for (UITouch* touch in touches) {
    if (_touch) {
      _touch->touch_cancelled(Finger(touch));
    }
  }
}

// The camera: none until the view exists, and none from the panel.
- (BOOL)gestureRecognizerShouldBegin:(UIGestureRecognizer*)recognizer {
  return _view && !_view->ui_wants_pointer();
}

// A two-finger pan and a pinch compose.
- (BOOL)gestureRecognizer:(UIGestureRecognizer*)recognizer
    shouldRecognizeSimultaneouslyWithGestureRecognizer:
        (UIGestureRecognizer*)other {
  return (recognizer == _pan && other == _pinch) ||
         (recognizer == _pinch && other == _pan);
}

// The pan's move since the last call, in view heights (OrbitCamera's drag;
// it ignores the non-finite move a zero height gives).
- (glm::vec2)dragOf:(UIPanGestureRecognizer*)recognizer {
  const CGFloat height = _metal.bounds.size.height;
  const CGPoint moved = [recognizer translationInView:_metal];
  [recognizer setTranslation:CGPointZero inView:_metal];
  return {static_cast<float>(moved.x / height),
          static_cast<float>(moved.y / height)};
}

- (void)onOrbit:(UIPanGestureRecognizer*)recognizer {
  const glm::vec2 drag = [self dragOf:recognizer];
  if (_view) {
    _view->camera().drag_orbit(drag);
  }
}

- (void)onPan:(UIPanGestureRecognizer*)recognizer {
  const glm::vec2 drag = [self dragOf:recognizer];
  if (_view) {
    _view->camera().drag_pan(drag, vkp::PlayerView::kFovY);
  }
}

- (void)onPinch:(UIPinchGestureRecognizer*)recognizer {
  if (_view) {
    _view->camera().zoom(static_cast<float>(1.0 / recognizer.scale));
  }
  recognizer.scale = 1.0;
}

- (void)sceneWillResignActive {
  [self stopLink];
  if (!_view) {
    return;
  }
  // Waits on main for the step under way. On a timeout the hold stays, so
  // nothing new starts, and the next activation builds a new view.
  const CFTimeInterval start = CACurrentMediaTime();
  const bool idle = _view->player().park(kParkWait);
  NSLog(@"volumetric_kit_player: parked %s in %.1f ms",
        idle ? "idle" : "with a step under way",
        (CACurrentMediaTime() - start) * 1e3);
  if (!idle) {
    _rebuild = YES;
  }
}

- (void)sceneDidBecomeActive {
  _failed = NO;  // one try per activation, never a loop
  _retried = NO;
  if (_rebuild) {
    _rebuild = NO;
    [self dropView];
  }
  if (!_view) {
    [self bringUpIfReady];
    return;
  }
  _view->player().unpark();
  [self startLink];
}

@end
