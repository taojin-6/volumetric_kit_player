// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The app and its one scene. The scene delegate makes the window over a
// PlayerViewController and hands it the scene's activation, as Info.plist's
// scene manifest asks (an app built with the iOS 27 SDK must adopt scenes).

#import <UIKit/UIKit.h>

#import "PlayerViewController.h"

@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@property(nonatomic, strong) PlayerViewController* player;
@end

@implementation SceneDelegate

- (void)scene:(UIScene*)scene
    willConnectToSession:(UISceneSession*)session
                 options:(UISceneConnectionOptions*)options {
  if (![scene isKindOfClass:[UIWindowScene class]]) {
    return;
  }
  self.player = [[PlayerViewController alloc] init];
  self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
  self.window.rootViewController = self.player;
  [self.window makeKeyAndVisible];
}

- (void)sceneWillResignActive:(UIScene*)scene {
  [self.player sceneWillResignActive];
}

- (void)sceneDidBecomeActive:(UIScene*)scene {
  [self.player sceneDidBecomeActive];
}

@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation AppDelegate
@end

int main(int argc, char* argv[]) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil,
                             NSStringFromClass([AppDelegate class]));
  }
}
