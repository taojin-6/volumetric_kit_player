// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

// volumetric_kit_player [--frames N] <stream.vrst>: plays a stream in a GLFW
// window. PlayerView does the work; this shell makes the window, hands it the
// mouse and keys, and pumps frames (DECISIONS.md). A stream it cannot play is
// refused before GLFW starts, and with VP_NO_WINDOW set it stops there.

#include <GLFW/glfw3.h>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <glm/vec2.hpp>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"
#include "volumetric_kit/gfx/camera/orbit_camera.hpp"
#include "volumetric_kit/gfx/glfw/imgui_glfw.hpp"
#include "volumetric_kit/gfx/glfw/window.hpp"
#include "volumetric_kit/gfx/windowing/surface_source.hpp"
#include "volumetric_kit/player/player_view.hpp"
#include "volumetric_kit/player/stream_player.hpp"

namespace {

namespace vkc = volumetric_kit::core;
namespace vg = volumetric_kit::gfx;
namespace vkp = volumetric_kit::player;

constexpr const char* kUsage =
    "usage: volumetric_kit_player [--frames N] <stream.vrst>\n"
    "  --frames N  exit after N presents of a stream frame (N >= 1)\n";

struct Args {
  std::string path;
  std::optional<std::uint64_t> frames;  // --frames, the CI smoke run's
};

// The command line, or none for the usage.
std::optional<Args> parse(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--frames" && i + 1 < argc && !args.frames) {
      const std::string_view n = argv[++i];
      std::uint64_t frames = 0;
      const auto [end, error] =
          std::from_chars(n.data(), n.data() + n.size(), frames);
      if (error != std::errc() || end != n.data() + n.size() || frames == 0) {
        return std::nullopt;
      }
      args.frames = frames;
    } else if (!arg.empty() && arg[0] != '-' && args.path.empty()) {
      args.path = arg;
    } else {
      return std::nullopt;
    }
  }
  if (args.path.empty()) {
    return std::nullopt;
  }
  return args;
}

// What GLFW's callbacks hand the loop, through the window's user pointer.
// TODO: the GLFW input glue (keys, the wheel, drags) gets one home in gfx_glfw
// with calib's (its calib_rig.cpp TODO); this wheel accumulator is a knowing
// copy until then (DECISIONS.md).
struct Input {
  vkp::StreamPlayer* player = nullptr;  // once the view holds it
  double wheel = 0.0;                   // notches since the last tick
};

Input& input_of(GLFWwindow* window) {
  return *static_cast<Input*>(glfwGetWindowUserPointer(window));
}

// The panel's controls as keys: Space plays or pauses, Right steps, L loops.
// Esc closes the window.
void on_key(GLFWwindow* window, int key, int /*scancode*/, int action,
            int /*mods*/) {
  if (action != GLFW_PRESS) {
    return;
  }
  if (key == GLFW_KEY_ESCAPE) {
    glfwSetWindowShouldClose(window, GLFW_TRUE);
  }
  vkp::StreamPlayer* const player = input_of(window).player;
  if (player == nullptr) {
    return;
  }
  if (key == GLFW_KEY_SPACE) {
    player->playing() ? player->pause() : player->play();
  } else if (key == GLFW_KEY_RIGHT) {
    player->step();
  } else if (key == GLFW_KEY_L) {
    player->set_loop(!player->loop());
  }
}

void on_scroll(GLFWwindow* window, double /*dx*/, double dy) {
  input_of(window).wheel += dy;
}

vkc::Status run(const Args& args) {
  VKC_ASSIGN(std::unique_ptr<vkp::StreamPlayer> player,
             vkp::StreamPlayer::open(args.path));  // refusals before GLFW
  if (std::getenv("VP_NO_WINDOW") != nullptr) {    // cli_test's backstop
    return vkc::Status::unsupported("VP_NO_WINDOW is set, so no window");
  }
  VKC_ASSIGN(const std::unique_ptr<vg::glfw::Library> library,
             vg::glfw::Library::init());
  Input input;  // outlives the window
  VKC_ASSIGN(const vg::glfw::Window window,
             vg::glfw::create_window(1280, 720, args.path.c_str()));
  GLFWwindow* const w = window.get();
  glfwSetWindowUserPointer(w, &input);
  glfwSetKeyCallback(w, on_key);  // before ImGuiGlfw, which chains them
  glfwSetScrollCallback(w, on_scroll);
  vkp::PlayerViewConfig config;
#ifndef NDEBUG
  config.validation = true;
#endif
  VKC_ASSIGN(const vg::windowing::SurfaceSource surface,
             vg::glfw::surface_source(w));
  VKC_ASSIGN(const std::unique_ptr<vkp::PlayerView> view,
             vkp::PlayerView::create(surface, vg::glfw::framebuffer_extent(w),
                                     std::move(player), config));
  VKC_ASSIGN(const std::unique_ptr<vg::glfw::ImGuiGlfw> ui,  // goes first
             vg::glfw::ImGuiGlfw::create(w, view->ui_context()));
  input.player = &view->player();

  glm::vec2 last = vg::glfw::cursor_in_framebuffer(w);
  std::uint64_t presented = 0;
  while (!glfwWindowShouldClose(w)) {
    glfwPollEvents();
    ui->new_frame();
    const VkExtent2D size = vg::glfw::framebuffer_size(w);
    const glm::vec2 cursor = vg::glfw::cursor_in_framebuffer(w);
    // The camera, in viewport heights, unless the panel holds the pointer.
    if (size.height > 0 && !view->ui_wants_pointer()) {
      vg::camera::OrbitCamera& camera = view->camera();
      const glm::vec2 drag = (cursor - last) / static_cast<float>(size.height);
      if (glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
        camera.drag_orbit(drag);
      }
      if (glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
        camera.drag_pan(drag, vkp::PlayerView::kFovY);
      }
      camera.zoom(std::pow(vg::camera::OrbitCamera::kZoomPerNotch,
                           static_cast<float>(input.wheel)));
    }
    last = cursor;
    input.wheel = 0.0;
    VKC_ASSIGN(const bool drawn, view->frame(size));
    if (!drawn) {
      glfwWaitEventsTimeout(0.05);  // minimized or settling: no busy loop
    } else if (args.frames && view->shown() && ++presented == *args.frames) {
      break;
    }
  }
  if (args.frames) {
    std::printf(
        "volumetric_kit_player: presented %llu frame(s), drew stream frame "
        "%lld\n",
        static_cast<unsigned long long>(presented),
        view->shown() ? static_cast<long long>(view->shown()->index) : -1LL);
  }
  return {};
}

}  // namespace

int main(int argc, char** argv) {
  const std::optional<Args> args = parse(argc, argv);
  if (!args) {
    std::fputs(kUsage, stderr);
    return 2;
  }
  const vkc::Status status = run(*args);
  if (!status.ok()) {
    const std::string_view domain = vkc::to_string(status.domain());
    std::fprintf(stderr, "volumetric_kit_player: %.*s: %s\n",
                 static_cast<int>(domain.size()), domain.data(),
                 status.message().c_str());
    return 1;
  }
  return 0;
}
