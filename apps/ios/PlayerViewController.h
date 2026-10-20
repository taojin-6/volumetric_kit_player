// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#import <UIKit/UIKit.h>

/// The iOS shell over PlayerView: it plays the first .vrst in the app's
/// Documents folder on a CAMetalLayer (PlayerViewController.mm). Main thread
/// only.
@interface PlayerViewController : UIViewController
/// The scene stops being active: stop drawing, and park the worker.
- (void)sceneWillResignActive;
/// The scene is active again: unpark, or build the view anew.
- (void)sceneDidBecomeActive;
@end
