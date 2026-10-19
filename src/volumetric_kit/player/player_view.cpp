// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/player_view.hpp"

#include <cstddef>
#include <utility>

#include "imgui.h"
#include "volumetric_kit/gfx/app/shared_device_config.hpp"
#include "volumetric_kit/gfx/assets/mesh.hpp"
#include "volumetric_kit/gfx/core/render_target.hpp"
#include "volumetric_kit/gfx/pipelines/live_mesh.hpp"
#include "volumetric_kit/gfx/windowing/frame_loop.hpp"
#include "volumetric_kit/gfx/windowing/swapchain.hpp"
#include "volumetric_kit/player/controls.hpp"
#include "volumetric_kit/player/mesh_wiring.hpp"
#include "volumetric_kit/recon/core/device_requirements.hpp"
#include "volumetric_kit/recon/mesh/mesh.hpp"

namespace volumetric_kit::player {

namespace app = gfx::app;
namespace pipelines = gfx::pipelines;

namespace {

// recon's buffers, drawn in place: gfx reads them through its own Vertex's
// size and offsets, so the two layouts must agree, or every vertex misreads
// with no error. gfx binds no tangent.
// TODO: a gfx live-mesh class owns this conversion (plan: once a 4th app
// draws a live mesh).
pipelines::LiveMesh live_mesh(const recon::mesh::DeviceMesh& mesh) {
  using Recon = recon::mesh::Vertex;
  using Gfx = gfx::assets::Vertex;
  static_assert(sizeof(Recon) == sizeof(Gfx),
                "recon and gfx vertex layouts have diverged in size");
  static_assert(offsetof(Recon, position) == offsetof(Gfx, position),
                "recon and gfx vertex layouts have diverged: position");
  static_assert(offsetof(Recon, normal) == offsetof(Gfx, normal),
                "recon and gfx vertex layouts have diverged: normal");
  static_assert(offsetof(Recon, uv0) == offsetof(Gfx, uv0),
                "recon and gfx vertex layouts have diverged: uv0");
  static_assert(offsetof(Recon, color) == offsetof(Gfx, color),
                "recon and gfx vertex layouts have diverged: color");
  pipelines::LiveMesh live;
  live.vertices = mesh.vertices;
  live.indices = mesh.indices;
  live.indirect = mesh.indirect;
  return live;
}

}  // namespace

core::Result<std::unique_ptr<PlayerView>> PlayerView::create(
    const gfx::windowing::SurfaceSource& surface, VkExtent2D extent,
    std::unique_ptr<StreamPlayer> player, const PlayerViewConfig& config) {
  if (player == nullptr) {
    return core::Status::invalid_argument("PlayerView::create needs a player");
  }
  if (extent.width == 0 || extent.height == 0) {
    return core::Status::invalid_argument(
        "PlayerView::create needs a drawable of at least 1 x 1");
  }
  app::WindowedAppConfig app_config;
  app_config.swapchain.extent = extent;
  app_config.swapchain.preferred_present_mode = VK_PRESENT_MODE_FIFO_KHR;
  app_config.swapchain.depth_format = VK_FORMAT_D32_SFLOAT;
  app_config.frames_in_flight = kFramesInFlight;
  app::WindowDeviceConfig device_config;
  device_config.instance.app_name = "volumetric_kit_player";
  device_config.instance.enable_validation = config.validation;
  device_config.graphics = app_config.device;
  VKC_ASSIGN(std::unique_ptr<core::SharedDevice> shared,
             app::make_shared_device(surface, recon::device_requirements(),
                                     device_config));
  VKC_ASSIGN(app::WindowedApp windowed,
             app::WindowedApp::adopt(*shared, app_config));
  VKC_ASSIGN(core::Device recon_device,
             core::Device::adopt(shared->compute_payload(),
                                 recon::device_requirements()));
  VKC_ASSIGN(
      core::Allocator recon_allocator,
      core::Allocator::create(shared->instance().handle(), recon_device));
  VKC_ASSIGN(pipelines::HybridMeshPipeline mesh_pipeline,
             pipelines::HybridMeshPipeline::create(
                 windowed.device(), windowed.allocator(),
                 windowed.swapchain().layout()));
  gfx::ui::ImGuiOverlayConfig overlay_config =
      gfx::ui::overlay_config(windowed.swapchain());
  overlay_config.scale = config.ui_scale;
  VKC_ASSIGN(
      gfx::ui::ImGuiOverlay overlay,
      gfx::ui::ImGuiOverlay::create(
          windowed.device(), windowed.instance_handle(), overlay_config));
  const MeshWiring wiring = mesh_wiring(
      shared->graphics_family(), shared->compute_family(), kFramesInFlight);
  std::unique_ptr<PlayerView> view(new PlayerView(
      std::move(shared), std::move(windowed), std::move(recon_device),
      std::move(recon_allocator), std::move(mesh_pipeline), std::move(overlay),
      wiring.exchange, std::move(player)));
  view->camera_.set_target({0.0f, 0.85f, 0.0f});
  view->camera_.set_distance(3.0f);
  VKC_TRY(view->player_->start(view->recon_device_, view->recon_allocator_,
                               wiring.mc, view->exchange_));
  return view;
}

PlayerView::PlayerView(std::unique_ptr<core::SharedDevice> shared,
                       app::WindowedApp app, core::Device recon_device,
                       core::Allocator recon_allocator,
                       pipelines::HybridMeshPipeline mesh_pipeline,
                       gfx::ui::ImGuiOverlay overlay,
                       const recon::mesh::MeshExchangeConfig& exchange,
                       std::unique_ptr<StreamPlayer> player)
    : shared_(std::move(shared)),
      app_(std::move(app)),
      recon_device_(std::move(recon_device)),
      recon_allocator_(std::move(recon_allocator)),
      mesh_pipeline_(std::move(mesh_pipeline)),
      overlay_(std::move(overlay)),
      exchange_(exchange),
      player_(std::move(player)) {}

PlayerView::~PlayerView() {
  player_->stop();
  shared_->wait_idle();
}

core::Result<bool> PlayerView::frame(VkExtent2D extent) {
  VKC_TRY(player_->status());
  // 1. A skipped tick opens no ImGui frame.
  VKC_ASSIGN(std::optional<gfx::windowing::Frame> begun,
             app_.begin_frame(extent));
  if (!begun) {
    return false;
  }
  const gfx::windowing::Frame& frame = *begun;
  // 2. The controls, as of the last frame drawn.
  overlay_.new_frame();
  draw_controls(*player_, shown_);
  // 3. The newest mesh; the slot's last frame has completed.
  exchange_.begin_frame(
      frame.slot, [this](const recon::mesh::DeviceMesh&, FrameStamp& stamp) {
        shown_ = stamp;
        return true;
      });
  const core::Status drawn = drawable(exchange_);
  // 4. The mesh, then the controls over it. recon's extract waited on its
  // fence before it published, and this frame's submit makes its writes
  // visible to the draw.
  gfx::RenderTargetBeginInfo clear;
  clear.clear_color.float32[0] = 0.05f;
  clear.clear_color.float32[1] = 0.05f;
  clear.clear_color.float32[2] = 0.07f;
  clear.clear_color.float32[3] = 1.0f;
  frame.target->begin(frame.cmd, clear);
  const recon::mesh::DeviceMesh& mesh = exchange_.live();
  if (mesh.valid() && !mesh.empty()) {
    const pipelines::HybridMeshDraw draw{live_mesh(mesh)};
    const VkExtent2D size = app_.swapchain().extent();
    pipelines::HybridMeshFrame draws;
    draws.extent = size;
    draws.view_proj = camera_
                          .to_camera(kFovY,
                                     static_cast<float>(size.width) /
                                         static_cast<float>(size.height),
                                     kNear, kFar)
                          .view_proj();
    draws.draws = &draw;
    draws.draw_count = 1;
    mesh_pipeline_.submit(frame.cmd, draws);
  }
  overlay_.render(frame.cmd);
  frame.target->end(frame.cmd);
  // 5. Every path from begin_frame ends the frame.
  const core::Status ended = app_.end_frame(frame);
  VKC_TRY(drawn);
  if (!ended.ok()) {
    if (gfx::windowing::swapchain_stale(ended)) {
      return false;
    }
    return ended;
  }
  return true;
}

bool PlayerView::ui_wants_pointer() const noexcept {
  ImGuiContext* const current = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(overlay_.context());
  const bool wants = ImGui::GetIO().WantCaptureMouse;
  ImGui::SetCurrentContext(current);
  return wants;
}

}  // namespace volumetric_kit::player
