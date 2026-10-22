// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file player_view.hpp
/// @brief The window side of playback, on any window system: one device that
///        recon and gfx share, a swapchain, the newest mesh under an orbit
///        camera, the controls and the stats.

#include <cstdint>
#include <memory>
#include <optional>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/shared_device.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"
#include "volumetric_kit/gfx/app/windowed_app.hpp"
#include "volumetric_kit/gfx/camera/orbit_camera.hpp"
#include "volumetric_kit/gfx/core/profiler.hpp"
#include "volumetric_kit/gfx/pipelines/hybrid_mesh_pipeline.hpp"
#include "volumetric_kit/gfx/ui/imgui_overlay.hpp"
#include "volumetric_kit/gfx/windowing/surface_source.hpp"
#include "volumetric_kit/gfx/windowing/swapchain.hpp"
#include "volumetric_kit/player/controls.hpp"
#include "volumetric_kit/player/playback_stats.hpp"
#include "volumetric_kit/player/stream_player.hpp"

struct ImGuiContext;

namespace volumetric_kit::player {

/// @brief What a shell may set beyond the surface.
struct PlayerViewConfig {
  /// The UI's size (gfx::ui::ImGuiOverlayConfig::scale): 1 on desktop, 1.3
  /// on a phone.
  float ui_scale = 1.0f;
  /// The UI's least frame height, in points: the controls' and the stats'
  /// header's (gfx::ui::ImGuiOverlayConfig::min_frame_height). 0 on desktop,
  /// 44 on iOS.
  float ui_min_frame_height = 0.0f;
  /// The Khronos validation layer on the shared instance, where it loads (a
  /// logged warning where it does not). The desktop shell sets it from its
  /// scenario's window.validation, and always in Debug; iOS has no layer
  /// (DECISIONS.md); the tests take it from VKC_TEST_VALIDATION.
  bool validation = false;
};

/// @brief The swapchain @ref PlayerView asks gfx for: FIFO and D32 depth;
///        live, two images, so a mesh taken shows a vsync sooner, and a
///        file's, gfx's default (the surface's least + 1).
///
/// @code
/// app_config.swapchain = view_swapchain(extent, player->live());
/// @endcode
/// @param extent  The drawable, in pixels.
/// @param live    Whether the player plays a live stream.
/// @return The config; gfx clamps its image count to the surface's range.
gfx::windowing::SwapchainConfig view_swapchain(VkExtent2D extent, bool live);

/// @brief The window side of playback. It holds one device that the worker
///        and the renderer share, a FIFO swapchain, and the newest mesh drawn
///        with an orbit camera, under the controls and the stats panel
///        (StatsPanel) drawn by gfx. Each call from the window system is one
///        tick.
///
/// **Threads.** Every member but @ref player's controls, and the destructor,
/// runs on one thread: the one that owns the window or layer (main on Apple).
/// @ref player's controls are safe from any thread. The worker builds its own
/// compute kernels, so @ref create builds only the window side.
///
/// @code
/// VKC_ASSIGN(auto view,
///            PlayerView::create(surface, extent, std::move(player)));
/// VKC_ASSIGN(auto ui, glfw::ImGuiGlfw::create(window, view->ui_context()));
/// while (open) {
///   ui->new_frame();
///   if (!view->ui_wants_pointer()) view->camera().drag_orbit(drag);
///   VKC_ASSIGN(bool presented, view->frame(extent));
/// }
/// @endcode
class PlayerView {
 public:
  static constexpr float kFovY = 0.87f;  ///< Vertical field of view, radians.

  /// @brief Make the shared device on @p surface (gfx::app::make_shared_device)
  ///        and adopt it for gfx (@ref view_swapchain, 2 frames in flight)
  ///        and for recon. Build the mesh pipeline, the overlay (no imgui.ini)
  ///        and a gfx::Profiler for the stats' "draw" row, attached to the
  ///        frame loop. Aim the camera at the first view: target (0, 0.85, 0),
  ///        3 m. Then start @p player; its worker builds its kernels, and it
  ///        plays unless the caller paused it.
  /// @param surface  The window system's source; its window or layer must
  ///                 outlive the view.
  /// @param extent   The drawable now, in pixels; both sides > 0.
  /// @param player   An opened player, not started.
  /// @param config   The UI's sizes and validation.
  /// @return The view; `core::Status::Code::InvalidArgument` for a null
  ///         @p player or an empty @p extent; or the first failure (device,
  ///         swapchain, pipeline, overlay, profiler, StreamPlayer::start).
  static core::Result<std::unique_ptr<PlayerView>> create(
      const gfx::windowing::SurfaceSource& surface, VkExtent2D extent,
      std::unique_ptr<StreamPlayer> player,
      const PlayerViewConfig& config = {});

  /// @brief Stop the worker, wait until every queue is idle and detach the
  ///        profiler; the members then go in reverse order.
  ~PlayerView();
  PlayerView(const PlayerView&) = delete;
  PlayerView& operator=(const PlayerView&) = delete;
  PlayerView(PlayerView&&) = delete;
  PlayerView& operator=(PlayerView&&) = delete;

  /// @brief One tick: draw the controls and the stats, take the newest mesh,
  ///        draw it, and present.
  /// @pre The UI platform half bound to @ref ui_context ran its new_frame this
  ///      tick, unless @p extent is 0 x 0 (a touch shell skips the tick's
  ///      new_frame then).
  /// @param extent  The drawable now, in pixels; 0 x 0 (minimized) skips.
  /// @return true once presented. false when nothing could render (0 x 0, or
  ///         the surface settling after a rebuild), so a desktop pump waits.
  ///         An error for device loss, a refused mesh, or the worker's own
  ///         failure, its kernel build included (StreamPlayer::status).
  core::Result<bool> frame(VkExtent2D extent);

  /// @return The player; its controls are safe from any thread.
  StreamPlayer& player() noexcept { return *player_; }
  /// @return The camera, for G1's drag verbs (gfx::camera::OrbitCamera).
  gfx::camera::OrbitCamera& camera() noexcept { return camera_; }
  /// @return Whether the mesh is coloured by its world-space normals rather
  ///         than lit; false at first. The panel's Normals flips it.
  bool normals() const noexcept { return normals_; }
  /// @brief Colour the mesh by its normals, or light it, from the next frame
  ///        (the desktop's N key).
  /// @param normals  Whether to colour it by its normals.
  void set_normals(bool normals) noexcept { normals_ = normals; }
  /// @return The flags the mesh draws with (gfx::pipelines::HybridMeshFlags):
  ///         kHybridMeshNormals while @ref normals, else kHybridMeshLit.
  std::uint32_t mesh_flags() const noexcept;
  /// @return The ImGui context the controls draw in. Bind the window system's
  ///         platform half to it (glfw::ImGuiGlfw, gfx::ui::ImGuiTouch),
  ///         declared after the view so it dies first.
  ImGuiContext* ui_context() const noexcept { return overlay_.context(); }
  /// @return Whether the controls or the stats' header held the pointer as of
  ///         the last frame (ImGui's WantCaptureMouse); if so, leave the
  ///         camera alone.
  bool ui_wants_pointer() const noexcept;
  /// @return The last stream frame drawn, for a read-out (the CI smoke).
  const std::optional<FrameStamp>& shown() const noexcept { return shown_; }
  /// @return Its mesh, in the extractor's ring, for a read-out of its counts;
  ///         generation 0 before the first.
  const recon::mesh::DeviceMesh& mesh() const noexcept {
    return exchange_.live();
  }
  /// @return The stats panel, for a read-out of its figures (vp_live_probe).
  const StatsPanel& stats_panel() const noexcept { return stats_panel_; }

 private:
  static constexpr std::uint32_t kFramesInFlight = 2;
  static constexpr float kNear = 0.05f;  // clip planes, metres
  static constexpr float kFar = 100.0f;

  PlayerView(std::unique_ptr<core::SharedDevice> shared,
             gfx::app::WindowedApp app, gfx::Profiler profiler,
             core::Device recon_device, core::Allocator recon_allocator,
             gfx::pipelines::HybridMeshPipeline mesh_pipeline,
             gfx::ui::ImGuiOverlay overlay,
             const recon::mesh::MeshExchangeConfig& exchange,
             std::unique_ptr<StreamPlayer> player);

  // Declared in teardown order, reversed: StreamPlayer goes before its
  // exchange and device (stream_player.hpp), the profiler before the app it
  // is attached to, and both adopted devices before SharedDevice. All but the
  // player's controls are the drawing thread's.
  std::unique_ptr<core::SharedDevice> shared_;
  gfx::app::WindowedApp app_;  // gfx's device and the swapchain
  gfx::Profiler profiler_;     // on app_'s device
  core::Device recon_device_;
  core::Allocator recon_allocator_;
  gfx::pipelines::HybridMeshPipeline mesh_pipeline_;
  gfx::ui::ImGuiOverlay overlay_;
  FrameExchange exchange_;
  gfx::camera::OrbitCamera camera_;
  bool normals_ = false;  // the mesh's shading (mesh_flags)
  std::unique_ptr<StreamPlayer> player_;
  PlaybackStats stats_;  // from player_'s track
  std::optional<FrameStamp> shown_;
  std::uint32_t shown_triangles_ = 0;  // shown_'s mesh's
  StatsPanel stats_panel_;
};

}  // namespace volumetric_kit::player
