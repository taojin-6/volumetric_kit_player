// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The iOS shell over PlayerView (DECISIONS.md), all on main: a MetalView hosts
// the CAMetalLayer, a display link ticks the view, every touch phase reaches
// gfx's ImGuiTouch for the controls panel, three recognizers drive the camera
// unless the panel holds the finger, and the scene's activation parks and
// unparks the worker. The Source button, at the top right, opens iOS's
// dialog, which chooses a sender by host and port, or the first .vrst in the
// app's Documents folder, which the Files app and Finder show (Info.plist.in);
// the choice is kept for the next launch.

#import "PlayerViewController.h"

#import <QuartzCore/QuartzCore.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/vec2.hpp>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/config/config.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"
#include "volumetric_kit/gfx/ui/imgui_touch.hpp"
#include "volumetric_kit/gfx/windowing/surface_source.hpp"
#include "volumetric_kit/player/player_view.hpp"
#include "volumetric_kit/player/scenario.hpp"
#include "volumetric_kit/player/stream_player.hpp"

namespace vkc = volumetric_kit::core;
namespace cfg = volumetric_kit::core::config;
namespace vg = volumetric_kit::gfx;
namespace vkp = volumetric_kit::player;

namespace {

// How long a resign-active waits for the worker's step under way.
constexpr std::chrono::seconds kParkWait{1};

// The last choice, in the standard user defaults (DECISIONS.md): whether it
// was a sender, and the sender's host and port as parse_live_source took
// them, the port empty for the sender's default. The host and port stay for
// the dialog when a file is chosen.
NSString* const kLiveKey = @"live";
NSString* const kHostKey = @"live.host";
NSString* const kPortKey = @"live.port";

// `text`, empty for nil.
std::string Utf8(NSString* text) { return text != nil ? text.UTF8String : ""; }

// The first .vrst in Documents, by name in Finder's order
// (localizedStandardCompare:). With none, a note is left there, as the Files
// app may hide an empty app folder (DECISIONS.md).
std::optional<cfg::Source> DocumentsStream() {
  NSFileManager* files = NSFileManager.defaultManager;
  NSString* documents = NSSearchPathForDirectoriesInDomains(
                            NSDocumentDirectory, NSUserDomainMask, YES)
                            .firstObject;
  NSArray<NSString*>* listed = [files contentsOfDirectoryAtPath:documents
                                                          error:nil];
  std::vector<std::string> names;
  for (NSString* name in
       [listed sortedArrayUsingSelector:@selector(localizedStandardCompare:)]) {
    names.emplace_back(name.UTF8String);
  }
  std::optional<cfg::Source> found =
      vkp::folder_source(documents.UTF8String, names);
  if (!found) {
    NSString* note = [documents
        stringByAppendingPathComponent:@"Copy .vrst streams here.txt"];
    NSData* text = [@"The player plays the first .vrst here, by name.\n"
        dataUsingEncoding:NSUTF8StringEncoding];
    if (![files fileExistsAtPath:note]) {
      [files createFileAtPath:note contents:text attributes:nil];
    }
  }
  return found;
}

// What to play: the sender of the last choice, if it was one; else the first
// .vrst in Documents; else nothing, and the dialog asks.
vkc::Result<std::optional<cfg::Source>> ChosenSource() {
  NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
  if ([defaults boolForKey:kLiveKey]) {
    VKC_ASSIGN(cfg::Source live,
               vkp::parse_live_source(Utf8([defaults stringForKey:kHostKey]),
                                      Utf8([defaults stringForKey:kPortKey])));
    return std::optional<cfg::Source>(std::move(live));
  }
  return DocumentsStream();
}

// The view over `source`, on `layer`, with fingertip-sized controls and the
// Source button. No validation: iOS has no layer to load (DECISIONS.md).
vkc::Result<std::unique_ptr<vkp::PlayerView>> MakeView(
    CAMetalLayer* layer, VkExtent2D pixels, const cfg::Source& source) {
  VKC_ASSIGN(std::unique_ptr<vkp::StreamPlayer> player,
             vkp::open_source(source));
  VKC_ASSIGN(const vg::windowing::SurfaceSource surface,
             vg::windowing::metal_surface((__bridge const void*)layer));
  vkp::PlayerViewConfig config;
  config.ui_scale = 1.3f;              // 17-pt text, near iOS body text
  config.ui_min_frame_height = 44.0f;  // Apple's least touch target
  config.source_button = true;
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
  UILabel* _failure;  // why nothing plays; a tap on it asks what to
  CADisplayLink* _link;
  UIPanGestureRecognizer* _orbit;  // one finger
  UIPanGestureRecognizer* _pan;    // two fingers, with the pinch
  UIPinchGestureRecognizer* _pinch;
  std::unique_ptr<vkp::PlayerView> _view;
  std::unique_ptr<vg::ui::ImGuiTouch> _touch;  // after _view: goes first
  BOOL _failed;    // the label is up until the next activation or choice
  BOOL _retried;   // a frame() error rebuilt the view in this activation
  BOOL _rebuild;   // a park timed out, so the device may be lost
  BOOL _appeared;  // on screen, so the dialog can be presented
  BOOL _askDue;    // nothing to play before then: ask once it is
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
  _failure.userInteractionEnabled = YES;  // with no view, no Source button
  [_failure addGestureRecognizer:[[UITapGestureRecognizer alloc]
                                     initWithTarget:self
                                             action:@selector(ask)]];
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

- (void)viewDidAppear:(BOOL)animated {
  [super viewDidAppear:animated];
  _appeared = YES;
  if (_askDue) {
    _askDue = NO;
    [self ask];
  }
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
  const vkc::Result<std::optional<cfg::Source>> chosen = ChosenSource();
  if (!chosen.ok()) {
    [self fail:chosen.status()];
    return;
  }
  if (!chosen.value()) {
    [self fail:vkc::Status::not_found(
                   "Nothing to play. Connect to a sender, or copy a .vrst "
                   "stream into this app's folder with the Files app or "
                   "Finder, then come back to the app.")];
    [self askOnScreen];
    return;
  }
  vkc::Result<std::unique_ptr<vkp::PlayerView>> made =
      MakeView((CAMetalLayer*)_metal.layer, pixels, **chosen);
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
  } else if (_view->take_source_request()) {
    [self ask];  // after the UI's frame: no UIKit runs inside it
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
  _failure.text =
      [message stringByAppendingString:@"\n\nTap here to choose what to play."];
  _failure.hidden = NO;
  NSLog(@"volumetric_kit_player: %@", message);
}

// The dialog over what to play, with the last sender's host and port.
- (void)ask {
  NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
  [self askWithHost:[defaults stringForKey:kHostKey]
               port:[defaults stringForKey:kPortKey]
             reason:nil];
}

// The dialog, once the view is on screen: presented before, it would not
// show.
- (void)askOnScreen {
  _askDue = !_appeared;
  if (_appeared) {
    [self ask];
  }
}

// iOS's own dialog (DECISIONS.md): Host and Port on the system keyboard, then
// Connect, Play file while Documents holds a .vrst, or Cancel. `reason`, a
// refusal of the values it is given, is its message. An alert needs no
// popover anchor on an iPad. One at a time.
- (void)askWithHost:(NSString*)host
               port:(NSString*)port
             reason:(NSString*)reason {
  if (self.presentedViewController != nil) {
    return;
  }
  UIAlertController* alert =
      [UIAlertController alertControllerWithTitle:@"Play"
                                          message:reason
                                   preferredStyle:UIAlertControllerStyleAlert];
  [alert addTextFieldWithConfigurationHandler:^(UITextField* field) {
    field.placeholder = @"Host";
    field.text = host;
    field.keyboardType = UIKeyboardTypeURL;
    field.autocorrectionType = UITextAutocorrectionTypeNo;
    field.autocapitalizationType = UITextAutocapitalizationTypeNone;
    field.spellCheckingType = UITextSpellCheckingTypeNo;
    field.clearButtonMode = UITextFieldViewModeWhileEditing;
  }];
  [alert addTextFieldWithConfigurationHandler:^(UITextField* field) {
    field.placeholder = @(vkp::LiveAddress{}.port).stringValue;
    field.text = port;
    field.keyboardType = UIKeyboardTypeNumberPad;
  }];
  // Weak: the alert holds the actions, and they would hold it.
  __weak PlayerViewController* weakSelf = self;
  __weak UIAlertController* weakAlert = alert;
  UIAlertAction* connect = [UIAlertAction
      actionWithTitle:@"Connect"
                style:UIAlertActionStyleDefault
              handler:^(UIAlertAction*) {
                [weakSelf connectHost:weakAlert.textFields[0].text
                                 port:weakAlert.textFields[1].text];
              }];
  [alert addAction:connect];
  if (DocumentsStream()) {
    [alert addAction:[UIAlertAction actionWithTitle:@"Play file"
                                              style:UIAlertActionStyleDefault
                                            handler:^(UIAlertAction*) {
                                              [weakSelf playFile];
                                            }]];
  }
  [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                            style:UIAlertActionStyleCancel
                                          handler:nil]];
  alert.preferredAction = connect;
  [self presentViewController:alert animated:YES completion:nil];
}

// Connect: values parse_live_source takes are kept and played; a refusal
// asks again, with why and with what was typed.
- (void)connectHost:(NSString*)host port:(NSString*)port {
  const vkc::Result<cfg::Source> live =
      vkp::parse_live_source(Utf8(host), Utf8(port));
  if (!live.ok()) {
    [self askWithHost:host port:port reason:@(live.status().message().c_str())];
    return;
  }
  NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
  [defaults setBool:YES forKey:kLiveKey];
  [defaults setObject:@(live->host.c_str()) forKey:kHostKey];
  [defaults setObject:live->port != 0 ? @(live->port).stringValue : @""
               forKey:kPortKey];
  [self rebuild];
}

- (void)playFile {
  [NSUserDefaults.standardUserDefaults setBool:NO forKey:kLiveKey];
  [self rebuild];
}

// The view anew on the choice just kept, as after a timed-out park. connect
// does no I/O, so main never waits on the network.
- (void)rebuild {
  _failed = NO;
  _retried = NO;
  [self dropView];
  [self bringUpIfReady];
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
