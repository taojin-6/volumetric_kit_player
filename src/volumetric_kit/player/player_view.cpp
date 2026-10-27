// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/player/player_view.hpp"

#include <utility>

#include "imgui.h"
#include "volumetric_kit/core/base/exception_status.hpp"
#include "volumetric_kit/gfx/app/shared_device_config.hpp"
#include "volumetric_kit/gfx/core/render_target.hpp"
#include "volumetric_kit/gfx/pipelines/live_mesh.hpp"
#include "volumetric_kit/gfx/windowing/frame_loop.hpp"
#include "volumetric_kit/gfx/windowing/swapchain.hpp"
#include "volumetric_kit/player/controls.hpp"
#include "volumetric_kit/player/mesh_wiring.hpp"
#include "volumetric_kit/recon/core/device_requirements.hpp"

namespace volumetric_kit::player {

namespace app = gfx::app;
namespace pipelines = gfx::pipelines;

namespace {

// recon's buffers, drawn in place: gfx reads them at its own Vertex's size
// and offsets, which recon's mesh.hpp and gfx's hybrid_mesh_pipeline.cpp each
// pin to the same numbers.
// TODO: a gfx live-mesh class owns this conversion (plan: once a 4th app
// draws a live mesh).
pipelines::LiveMesh live_mesh(const recon::mesh::DeviceMesh& mesh) {
  pipelines::LiveMesh live;
  live.vertices = mesh.vertices;
  live.indices = mesh.indices;
  live.indirect = mesh.indirect;
  return live;
}

}  // namespace

gfx::windowing::SwapchainConfig view_swapchain(VkExtent2D extent, bool live) {
  gfx::windowing::SwapchainConfig config;
  config.extent = extent;
  config.depth_format = VK_FORMAT_D32_SFLOAT;
  if (live) {
    config.min_image_count = 2;
  }
  return config;
}

core::Result<std::unique_ptr<PlayerView>> PlayerView::create(
    const gfx::windowing::SurfaceSource& surface, VkExtent2D extent,
    std::unique_ptr<StreamPlayer> player, const PlayerViewConfig& config) try {
  if (player == nullptr) {
    return core::Status::invalid_argument("PlayerView::create needs a player");
  }
  if (extent.width == 0 || extent.height == 0) {
    return core::Status::invalid_argument(
        "PlayerView::create needs a drawable of at least 1 x 1");
  }
  app::WindowedAppConfig app_config;
  app_config.swapchain = view_swapchain(extent, player->live());
  app_config.frames_in_flight = kFramesInFlight;
  app::WindowDeviceConfig device_config;
  device_config.instance.app_name = "volumetric_kit_player";
  device_config.instance.enable_validation = config.validation;
  device_config.instance.request_debug_utils = config.debug_labels;
  device_config.graphics = app_config.device;
  VKC_ASSIGN(std::unique_ptr<core::SharedDevice> shared,
             app::make_shared_device(surface, recon::device_requirements(),
                                     device_config));
  VKC_ASSIGN(app::WindowedApp windowed,
             app::WindowedApp::adopt(*shared, app_config));
  gfx::ProfilerConfig profiler_config;
  profiler_config.frames_in_flight = kFramesInFlight;
  VKC_ASSIGN(gfx::Profiler profiler,
             gfx::Profiler::create(windowed.device(), profiler_config));
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
  overlay_config.min_frame_height = config.ui_min_frame_height;
  VKC_ASSIGN(
      gfx::ui::ImGuiOverlay overlay,
      gfx::ui::ImGuiOverlay::create(
          windowed.device(), windowed.instance_handle(), overlay_config));
  // A commit hook may skip a mesh, which keeps its slot until the next.
  const MeshWiring wiring = mesh_wiring(
      shared->graphics_family(), shared->compute_family(), kFramesInFlight,
      config.share_vertices, static_cast<bool>(config.make_hooks));
  const std::uint32_t compute_family = shared->compute_family();
  const std::uint32_t graphics_family = shared->graphics_family();
  std::unique_ptr<PlayerView> view(
      new PlayerView(std::move(shared), std::move(windowed),
                     std::move(profiler), std::move(recon_device),
                     std::move(recon_allocator), std::move(mesh_pipeline),
                     std::move(overlay), wiring.exchange, std::move(player)));
  view->camera_.set_target({0.0f, 0.85f, 0.0f});
  view->camera_.set_distance(3.0f);
  view->source_button_ = config.source_button;
  view->unlit_ = config.unlit;
  view->textured_ = config.textured;
  view->stats_panel_ = StatsPanel(config.compact_stats);
  // The panel opens at launch, so its frames are timed from the first.
  view->device_timing_ = config.device_timing;
  view->player_->set_device_timing(
      config.device_timing.value_or(DeviceTiming::kAll));
  view->player_->set_tile_depth(config.tile_depth);
  if (config.make_hooks) {
    const HookContext context{view->recon_device_,   view->recon_allocator_,
                              view->app_.device(),   view->app_.allocator(),
                              &view->mesh_pipeline_, compute_family,
                              graphics_family};
    VKC_ASSIGN(view->hooks_, config.make_hooks(context));
  }
  VKC_TRY(view->player_->start(view->recon_device_, view->recon_allocator_,
                               wiring.mc, view->exchange_, &view->hooks_));
  return view;
} catch (...) {
  return core::exception_status("PlayerView::create",
                                core::Status::Code::InvalidArgument);
}

PlayerView::PlayerView(std::unique_ptr<core::SharedDevice> shared,
                       app::WindowedApp app, gfx::Profiler profiler,
                       core::Device recon_device,
                       core::Allocator recon_allocator,
                       pipelines::HybridMeshPipeline mesh_pipeline,
                       gfx::ui::ImGuiOverlay overlay,
                       const recon::mesh::MeshExchangeConfig& exchange,
                       std::unique_ptr<StreamPlayer> player)
    : shared_(std::move(shared)),
      app_(std::move(app)),
      profiler_(std::move(profiler)),
      recon_device_(std::move(recon_device)),
      recon_allocator_(std::move(recon_allocator)),
      mesh_pipeline_(std::move(mesh_pipeline)),
      overlay_(std::move(overlay)),
      exchange_(exchange),
      player_(std::move(player)),
      taker_(*player_, exchange_, kFramesInFlight, &hooks_),
      stats_(player_->track_stats(), player_->rate()) {}

PlayerView::~PlayerView() {
  player_->stop();
  shared_->wait_idle();
  app_.set_profiler(nullptr);
}

core::Result<bool> PlayerView::frame(VkExtent2D extent) try {
  if (frame_failed_) {
    return core::Status::invalid_argument(
        "PlayerView::frame: recreate the view after an exception");
  }
  VKC_TRY(player_->status());
  VKC_TRY(hook_failure_);
  // 1. The draw is timed as the worker is, while the stats show it as of the
  // last frame: gfx's profiler marks every frame it is attached to, so it is
  // attached only then, in its place (the loop keeps its address; the
  // destructor detaches it). A skipped tick opens no ImGui frame. A frame
  // begun opens the attached profiler's too.
  profiled_ =
      stats_panel_.open() && player_->device_timing() != DeviceTiming::kOff;
  app_.set_profiler(profiled_ ? &profiler_ : nullptr);
  VKC_ASSIGN(std::optional<gfx::windowing::Frame> begun,
             app_.begin_frame(extent));
  if (!begun) {
    return false;
  }
  gfx::windowing::Frame& frame = *begun;  // not const: the mesh's wait goes in
  // 2. The controls, Source if asked for, and the stats, as of the last frame
  // drawn.
  overlay_.new_frame();
  // Texture's box once a before_draw has bound a set.
  draw_controls(*player_, shown_, normals_, unlit_,
                texture_bound_ ? &textured_ : nullptr);
  if (source_button_ && draw_source()) {
    source_request_ = true;
  }
  // Detached, the profiler's last figures are old: the renderer's table is
  // empty.
  stats_panel_.draw(stats_, shown_, shown_triangles_, render_metrics(),
                    PlaybackStats::Clock::now(), player_->live(),
                    hooks_.stats_line);
  // The worker times its frames' device work while the panel shows it,
  // unless a measurement fixed the timing.
  if (!device_timing_) {
    player_->set_device_timing(stats_panel_.open() ? DeviceTiming::kAll
                                                   : DeviceTiming::kOff);
  }
  // 3. The newest mesh, an empty one included, through the commit hook; the
  // slot's last frame has completed. Each frame committed is stamped, and
  // counted once presented.
  core::Result<std::optional<FrameStamp>> taken = taker_.take(frame.slot);
  core::Status failed = taken.status();
  bool took = false;
  if (taken.ok() && taken->has_value()) {
    shown_ = std::move(**taken);
    shown_triangles_ = exchange_.live().triangle_count;
    ++frames_taken_;
    took = true;
  }
  const core::Status drawn = drawable(exchange_);
  // 4. The frame's binding: the view's own (Unlit) and before_draw's, unless
  // Texture is off and that names a set (textured()).
  const recon::mesh::DeviceMesh& live = exchange_.live();
  const bool draws = live.valid() && !live.empty();
  binding_ = DrawBinding{};
  if (unlit_) {
    binding_.mesh_flags_clear = pipelines::kHybridMeshLit;
  }
  if (draws && failed.ok() && hooks_.before_draw && shown_) {
    core::Result<DrawBinding> bound = hooks_.before_draw(frame.cmd, *shown_);
    if (bound.ok()) {
      const bool texture = bound->descriptor_set != VK_NULL_HANDLE;
      texture_bound_ = texture_bound_ || texture;
      if (textured_ || !texture) {
        binding_.descriptor_set = bound->descriptor_set;
        binding_.mesh_flags_clear |= bound->mesh_flags_clear;
      }
    } else {
      failed = bound.status();
    }
  }
  // 5. The mesh, behind a GPU wait on its ready value, then the panels over
  // it. The take committed it once that value was reached, so the wait stalls
  // nothing; it makes the worker's writes visible to the draw on this queue.
  if (draws && live.ready.semaphore != nullptr) {
    frame.waits.push_back(
        {live.ready, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT});
  }
  gfx::RenderTargetBeginInfo clear;
  clear.clear_color.float32[0] = 0.05f;
  clear.clear_color.float32[1] = 0.05f;
  clear.clear_color.float32[2] = 0.07f;
  clear.clear_color.float32[3] = 1.0f;
  {
    // The stats' "draw (mesh+UI)" row: CPU is the recording, GPU the
    // device's time where the graphics queue can time. It spans the whole
    // pass, as Apple GPUs time only at a pass's bounds: inside, it reads
    // about 0. Inert while the profiler is detached.
    const gfx::Profiler::Scope timed = profiler_.gpu_scope(frame.cmd, kDrawRow);
    frame.target->begin(frame.cmd, clear);
    if (failed.ok()) {
      record_mesh(frame.cmd, app_.swapchain().extent());
    }
    overlay_.render(frame.cmd);
    frame.target->end(frame.cmd);
  }
  // 6. Every path from begin_frame ends the frame.
  frame_waits_ = frame.waits;
  const core::Status ended = app_.end_frame(frame);
  if (took) {
    if (ended.ok()) {
      shown_->presented = PlaybackStats::Clock::now();
    }
    stats_.add_frame(*shown_, *shown_->taken);
  }
  if (!failed.ok()) {
    hook_failure_ = failed;  // every later tick refuses with it
    return failed;
  }
  VKC_TRY(drawn);
  if (!ended.ok()) {
    if (gfx::windowing::swapchain_stale(ended)) {
      return false;
    }
    return ended;
  }
  return true;
} catch (...) {
  // An exception can interrupt a begun ImGui frame or GPU recording. Neither
  // is safe to resume. Keep all owners alive until normal teardown drains
  // submitted work; later ticks must not acquire another swapchain image.
  frame_failed_ = true;
  return core::exception_status("PlayerView::frame",
                                core::Status::Code::InvalidArgument);
}

const gfx::FrameMetrics& PlayerView::render_metrics() const noexcept {
  static const gfx::FrameMetrics kUnprofiled;
  return profiled_ ? profiler_.metrics() : kUnprofiled;
}

std::uint32_t PlayerView::mesh_flags() const noexcept {
  return normals_ ? pipelines::kHybridMeshNormals : pipelines::kHybridMeshLit;
}

void PlayerView::record_mesh(VkCommandBuffer cmd, VkExtent2D extent) const {
  const recon::mesh::DeviceMesh& live = exchange_.live();
  if (!live.valid() || live.empty()) {
    return;
  }
  const pipelines::HybridMeshDraw draw{live_mesh(live)};
  pipelines::HybridMeshFrame draws;
  draws.extent = extent;
  draws.view_proj = camera_
                        .to_camera(kFovY,
                                   static_cast<float>(extent.width) /
                                       static_cast<float>(extent.height),
                                   kNear, kFar)
                        .view_proj();
  draws.flags = mesh_flags() & ~binding_.mesh_flags_clear;
  draws.atlas = binding_.descriptor_set;
  draws.draws = &draw;
  draws.draw_count = 1;
  mesh_pipeline_.submit(cmd, draws);
}

gfx::RenderTargetLayout PlayerView::target_layout() const {
  return app_.swapchain().layout();
}

bool PlayerView::ui_wants_pointer() const noexcept {
  ImGuiContext* const current = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(overlay_.context());
  const bool wants = ImGui::GetIO().WantCaptureMouse;
  ImGui::SetCurrentContext(current);
  return wants;
}

}  // namespace volumetric_kit::player
