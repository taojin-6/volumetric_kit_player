// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file player_view.hpp
/// @brief The window side of playback, on any window system: one device that
///        recon and gfx share, a swapchain, the newest mesh under an orbit
///        camera, and the controls.

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
#include "volumetric_kit/gfx/pipelines/hybrid_mesh_pipeline.hpp"
#include "volumetric_kit/gfx/ui/imgui_overlay.hpp"
#include "volumetric_kit/gfx/windowing/surface_source.hpp"
#include "volumetric_kit/player/stream_player.hpp"

struct ImGuiContext;

namespace volumetric_kit::player {

/// @brief What a shell may set beyond the surface.
struct PlayerViewConfig {
  /// The controls' size (gfx::ui::ImGuiOverlayConfig::scale): 1 on desktop,
  /// about 2 on a phone.
  float ui_scale = 1.0f;
  /// The Khronos validation layer on the shared instance, where it loads (a
  /// logged warning where it does not). Every shell sets it in Debug; the
  /// tests take it from VKC_TEST_VALIDATION.
  bool validation = false;
};

/// @brief The window side of playback. It holds one device that the worker
///        and the renderer share, a FIFO swapchain, and the newest mesh drawn
///        with an orbit camera, under a controls panel drawn by gfx. Each call
///        from the window system is one tick.
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
  ///        and adopt it for gfx (FIFO, D32 depth, 2 frames in flight) and for
  ///        recon. Build the mesh pipeline and the controls overlay (no
  ///        imgui.ini). Aim the camera at the first view: target (0, 0.85, 0),
  ///        3 m. Then start @p player; its worker builds its kernels, and it
  ///        plays unless the caller paused it.
  /// @param surface  The window system's source; its window or layer must
  ///                 outlive the view.
  /// @param extent   The drawable now, in pixels; both sides > 0.
  /// @param player   An opened player, not started.
  /// @param config   The UI scale and validation.
  /// @return The view; `core::Status::Code::InvalidArgument` for a null
  ///         @p player or an empty @p extent; or the first failure (device,
  ///         swapchain, pipeline, overlay, StreamPlayer::start).
  static core::Result<std::unique_ptr<PlayerView>> create(
      const gfx::windowing::SurfaceSource& surface, VkExtent2D extent,
      std::unique_ptr<StreamPlayer> player,
      const PlayerViewConfig& config = {});

  /// @brief Stop the worker and wait until every queue is idle; the members
  ///        then go in reverse order.
  ~PlayerView();
  PlayerView(const PlayerView&) = delete;
  PlayerView& operator=(const PlayerView&) = delete;
  PlayerView(PlayerView&&) = delete;
  PlayerView& operator=(PlayerView&&) = delete;

  /// @brief One tick: draw the controls, take the newest mesh, draw it, and
  ///        present.
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
  /// @return The ImGui context the controls draw in. Bind the window system's
  ///         platform half to it (glfw::ImGuiGlfw, gfx::ui::ImGuiTouch),
  ///         declared after the view so it dies first.
  ImGuiContext* ui_context() const noexcept { return overlay_.context(); }
  /// @return Whether the controls held the pointer as of the last frame
  ///         (ImGui's WantCaptureMouse); if so, leave the camera alone.
  bool ui_wants_pointer() const noexcept;
  /// @return The last stream frame drawn, for a read-out (the CI smoke).
  const std::optional<FrameStamp>& shown() const noexcept { return shown_; }

 private:
  static constexpr std::uint32_t kFramesInFlight = 2;
  static constexpr float kNear = 0.05f;  // clip planes, metres
  static constexpr float kFar = 100.0f;

  PlayerView(std::unique_ptr<core::SharedDevice> shared,
             gfx::app::WindowedApp app, core::Device recon_device,
             core::Allocator recon_allocator,
             gfx::pipelines::HybridMeshPipeline mesh_pipeline,
             gfx::ui::ImGuiOverlay overlay,
             const recon::mesh::MeshExchangeConfig& exchange,
             std::unique_ptr<StreamPlayer> player);

  // Declared in teardown order, reversed: StreamPlayer goes before its
  // exchange and device (stream_player.hpp), and both adopted devices before
  // SharedDevice.
  std::unique_ptr<core::SharedDevice> shared_;
  gfx::app::WindowedApp app_;  // gfx's device and the swapchain
  core::Device recon_device_;
  core::Allocator recon_allocator_;
  gfx::pipelines::HybridMeshPipeline mesh_pipeline_;
  gfx::ui::ImGuiOverlay overlay_;
  FrameExchange exchange_;
  gfx::camera::OrbitCamera camera_;
  std::unique_ptr<StreamPlayer> player_;
  std::optional<FrameStamp> shown_;
};

}  // namespace volumetric_kit::player
