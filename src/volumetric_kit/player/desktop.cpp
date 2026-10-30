// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// The desktop app (desktop.hpp): plays a scenario's stream, from a file or live
// from a sender, in a GLFW window. PlayerView does the work; this shell makes
// the window, hands it the mouse and keys, and pumps frames (DECISIONS.md).

#include "volumetric_kit/player/desktop.hpp"

#include <GLFW/glfw3.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <glm/vec2.hpp>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"
#include "volumetric_kit/gfx/camera/orbit_camera.hpp"
#include "volumetric_kit/gfx/glfw/imgui_glfw.hpp"
#include "volumetric_kit/gfx/glfw/window.hpp"
#include "volumetric_kit/gfx/windowing/surface_source.hpp"
#include "volumetric_kit/player/frame_hooks.hpp"
#include "volumetric_kit/player/pointer.hpp"
#include "volumetric_kit/player/scenario.hpp"
#include "volumetric_kit/player/stream_player.hpp"

namespace volumetric_kit::player {

namespace {

namespace cfg = volumetric_kit::core::config;
namespace vg = volumetric_kit::gfx;

// What GLFW's callbacks hand the loop, through the window's user pointer.
// TODO: the GLFW input glue (keys, the wheel, drags) gets one home in gfx_glfw
// with calib's (its calib_rig.cpp TODO); this wheel accumulator is a knowing
// copy until then (DECISIONS.md).
struct Input {
  PlayerView* view = nullptr;  // once it is made
  double wheel = 0.0;          // notches since the last tick
};

Input& input_of(GLFWwindow* window) {
  return *static_cast<Input*>(glfwGetWindowUserPointer(window));
}

// The panel's controls as keys: Space plays or pauses, Right steps, L loops,
// which a live player ignores, N flips Normals, D the deblock, U Unlit, T
// Texture, M the mute, and a program's key its panel (PlayerView::panel_key).
// Esc closes the window.
void on_key(GLFWwindow* window, int key, int /*scancode*/, int action,
            int /*mods*/) {
  if (action != GLFW_PRESS) {
    return;
  }
  if (key == GLFW_KEY_ESCAPE) {
    glfwSetWindowShouldClose(window, GLFW_TRUE);
  }
  PlayerView* const view = input_of(window).view;
  if (view == nullptr) {
    return;
  }
  StreamPlayer& player = view->player();
  if (key == GLFW_KEY_SPACE) {
    player.playing() ? player.pause() : player.play();
  } else if (key == GLFW_KEY_RIGHT) {
    player.step();
  } else if (key == GLFW_KEY_L) {
    player.set_loop(!player.loop());
  } else if (key == GLFW_KEY_N) {
    view->set_normals(!view->normals());
  } else if (key == GLFW_KEY_D) {
    player.set_deblock(!player.deblock());
  } else if (key == GLFW_KEY_U) {
    view->set_unlit(!view->unlit());
  } else if (key == GLFW_KEY_T) {
    view->set_textured(!view->textured());
  } else if (key == GLFW_KEY_M) {
    player.set_muted(!player.muted());
  } else if (view->panel_key() != 0 && key == view->panel_key()) {
    view->set_panel_open(!view->panel_open());
  }
}

void on_scroll(GLFWwindow* window, double /*dx*/, double dy) {
  input_of(window).wheel += dy;
}

core::Status run(const cfg::Config& settings, const AppConfig& config) {
  // Refusals before GLFW; a live player does no I/O. A file's audio plays
  // through the platform's output, made at start.
  VKC_ASSIGN(std::unique_ptr<StreamPlayer> player,
             open_source(settings.source, AudioOut::open_default));
  const std::string title =
      player->live() ? player->live_status().address : settings.source.path;
  if (std::getenv("VP_NO_WINDOW") != nullptr) {  // cli_test's backstop
    return core::Status::unsupported("VP_NO_WINDOW is set, so no window for " +
                                     title);
  }
  VKC_ASSIGN(const std::unique_ptr<vg::glfw::Library> library,
             vg::glfw::Library::init());
  Input input;  // outlives the window
  VKC_ASSIGN(const vg::glfw::Window window,
             vg::glfw::create_window(settings.window.width,
                                     settings.window.height, title.c_str()));
  GLFWwindow* const w = window.get();
  glfwSetWindowUserPointer(w, &input);
  glfwSetKeyCallback(w, on_key);  // before ImGuiGlfw, which chains them
  glfwSetScrollCallback(w, on_scroll);
  VKC_ASSIGN(const vg::windowing::SurfaceSource surface,
             vg::glfw::surface_source(w));
  VKC_ASSIGN(const std::unique_ptr<PlayerView> view,
             PlayerView::create(surface, vg::glfw::framebuffer_extent(w),
                                std::move(player),
                                desktop_view_config(config, settings)));
  VKC_ASSIGN(const std::unique_ptr<vg::glfw::ImGuiGlfw> ui,  // goes first
             vg::glfw::ImGuiGlfw::create(w, view->ui_context()));
  input.view = view.get();

  glm::vec2 last = vg::glfw::cursor_in_framebuffer(w);
  PointerTarget target;  // the scene or the panel, kept through a press
  DoubleClick clicks;
  bool was_left = false;
  // The presents of a stream frame to exit after, for CI's smoke run; 0 plays
  // until the window closes.
  const std::uint64_t frames =
      settings.run.frames > 0 ? static_cast<std::uint64_t>(settings.run.frames)
                              : 0;
  std::uint64_t presented = 0;
  while (!glfwWindowShouldClose(w)) {
    glfwPollEvents();
    ui->new_frame();
    const VkExtent2D size = vg::glfw::framebuffer_size(w);
    const glm::vec2 cursor = vg::glfw::cursor_in_framebuffer(w);
    const bool left =
        glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    const bool right =
        glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    // The program's panel or the scene, as a press began, unless the panels
    // hold the pointer: on the panel, its pointer input; on the scene, the
    // camera, its drags in the scene's heights.
    const bool to_panel =
        target.on_panel(left || right, view->over_panel(cursor));
    double points_x = 0.0;
    double points_y = 0.0;
    glfwGetCursorPos(w, &points_x, &points_y);  // window points, as ImGui's
    const bool double_click = left && !was_left &&
                              clicks.press({static_cast<float>(points_x),
                                            static_cast<float>(points_y)},
                                           glfwGetTime());
    const float height = static_cast<float>(
        view->scene_rect().extent.height > 0 ? view->scene_rect().extent.height
                                             : size.height);
    if (!view->ui_wants_pointer() && to_panel) {
      PanelPointer pointer;
      pointer.at = cursor;
      if (left || right) {
        pointer.drag = cursor - last;
      }
      pointer.zoom = std::pow(kWheelZoom, static_cast<float>(input.wheel));
      pointer.double_tap = double_click;
      if (pointer.drag != glm::vec2(0.0f) || input.wheel != 0.0 ||
          double_click) {
        view->panel_pointer(pointer);
      }
    } else if (height > 0.0f && !view->ui_wants_pointer()) {
      vg::camera::OrbitCamera& camera = view->camera();
      const glm::vec2 drag = (cursor - last) / height;
      if (left) {
        camera.drag_orbit(drag);
      }
      if (right) {
        camera.drag_pan(drag, PlayerView::kFovY);
      }
      camera.zoom(std::pow(vg::camera::OrbitCamera::kZoomPerNotch,
                           static_cast<float>(input.wheel)));
    }
    last = cursor;
    was_left = left;
    input.wheel = 0.0;
    VKC_ASSIGN(const bool drawn, view->frame(size));
    if (!drawn) {
      glfwWaitEventsTimeout(0.05);  // minimized or settling: no busy loop
    } else if (frames != 0 && view->shown() && ++presented == frames) {
      break;
    }
  }
  if (frames != 0) {
    std::printf(
        "volumetric_kit_player: presented %llu frame(s), drew stream frame "
        "%lld\n",
        static_cast<unsigned long long>(presented),
        view->shown() ? static_cast<long long>(view->shown()->index) : -1LL);
  }
  return {};
}

// A refusal on one line, with its domain; `exit_code`.
int report(const core::Status& refused, int exit_code) {
  const std::string_view domain = core::to_string(refused.domain());
  std::fprintf(stderr, "volumetric_kit_player: %.*s: %s\n",
               static_cast<int>(domain.size()), domain.data(),
               refused.message().c_str());
  return exit_code;
}

}  // namespace

PlayerViewConfig desktop_view_config(const AppConfig& config,
                                     const core::config::Config& scenario) {
  PlayerViewConfig view = config.view;
  view.validation = scenario.window.validation;
#ifndef NDEBUG
  view.validation = true;
#endif
  return view;
}

int run_desktop(int argc, char** argv, const AppConfig& config) {
  const core::Result<cfg::Arguments> args = cfg::parse_args(argc, argv);
  if (!args.ok()) {
    std::fprintf(stderr, "volumetric_kit_player: %s\n%s",
                 args.status().message().c_str(), cfg::usage());
    return 2;
  }
  if (args.value().help) {
    std::fputs(cfg::usage(), stdout);
    return 0;
  }
  const core::Result<cfg::Config> settings =
      cfg::load(args.value().config_path, cfg::Application::Player);
  if (!settings.ok()) {
    return report(settings.status(), 2);
  }
  const core::Status status = run(settings.value(), config);
  return status.ok() ? 0 : report(status, 1);
}

}  // namespace volumetric_kit::player
