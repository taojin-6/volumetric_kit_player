// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file player_view.hpp
/// @brief The window side of playback, on any window system: one device that
///        grid and gfx share, a swapchain, the newest mesh under an orbit
///        camera, the controls and the stats.

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/shared_device.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"
#include "volumetric_kit/gfx/app/windowed_app.hpp"
#include "volumetric_kit/gfx/camera/orbit_camera.hpp"
#include "volumetric_kit/gfx/core/profiler.hpp"
#include "volumetric_kit/gfx/core/render_target.hpp"
#include "volumetric_kit/gfx/pipelines/hybrid_mesh_pipeline.hpp"
#include "volumetric_kit/gfx/ui/imgui_overlay.hpp"
#include "volumetric_kit/gfx/windowing/frame_loop.hpp"
#include "volumetric_kit/gfx/windowing/surface_source.hpp"
#include "volumetric_kit/gfx/windowing/swapchain.hpp"
#include "volumetric_kit/player/controls.hpp"
#include "volumetric_kit/player/frame_hooks.hpp"
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
  /// The Source button at the top right (draw_source), which asks the shell
  /// for something else to play (PlayerView::take_source_request): on iOS;
  /// the desktop takes a scenario instead.
  bool source_button = false;
  /// The Khronos validation layer on the shared instance, where it loads (a
  /// logged warning where it does not). The desktop shell sets it from its
  /// scenario's window.validation, and always in Debug; iOS has no layer
  /// (DECISIONS.md); the tests take it from VKC_TEST_VALIDATION.
  bool validation = false;
  /// The stats compact, for a phone (StatsPanel): the iOS shell sets it on
  /// an iPhone.
  bool compact_stats = false;
  /// The worker's device timing (StreamPlayer::set_device_timing), fixed, as
  /// a measurement sets it. Empty, the default: every frame while the stats
  /// panel is open, and none while it is closed. The draw is timed, every
  /// display frame, only while the panel is open and the worker's timing is
  /// not off.
  std::optional<DeviceTiming> device_timing;
  /// The worker's tile depth (StreamPlayer::set_tile_depth): 0, every scope;
  /// 1, "decode" and "mesh" alone. For M4's A/B.
  std::uint32_t tile_depth = 0;
  /// Debug labels on the shared instance
  /// (`core::InstanceConfig::request_debug_utils`): on, as grid labels its
  /// dispatches in Release; off for M4's A/B. Validation keeps them on.
  bool debug_labels = true;
  /// Whether the extractor shares vertices between triangles
  /// (grid::mesh::MarchingCubesConfig::share_vertices, through mesh_wiring):
  /// true, as by default; false, no vertex shared, three a triangle: a
  /// cheaper extraction for about three times the vertices (DECISIONS.md).
  bool share_vertices = true;
  /// Called in create, after the device and the pipeline, before the worker
  /// starts (frame_hooks.hpp). Given, the ring holds one mesh more
  /// (mesh_wiring's `holds`); what the hooks capture must outlive the view.
  MakeHooks make_hooks;
  /// What a program's hooks need of the worker's device beyond grid's, such
  /// as importing external memory (@ref worker_requirements). The hooks'
  /// devices (HookContext::device and commit_device, one `VkDevice`) enable
  /// them. The worker's queue never presents, so `needs_present` is refused.
  /// The default adds nothing.
  core::DeviceRequirements device_requirements;
  /// The panel's Unlit, at start (PlayerView::unlit).
  bool unlit = false;
  /// The panel's Texture, at start (PlayerView::textured).
  bool textured = false;
};

/// @brief The requirements of the device the worker decodes and meshes on, as
///        @ref PlayerView and vp_probe (probe.hpp) make it: grid's
///        (grid::device_requirements) merged with a program's (core::merge),
///        so grid's stay required.
///
/// @code
/// PlayerViewConfig config;
/// config.device_requirements.optional_extensions.push_back(
///     VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
/// VKC_ASSIGN(const core::DeviceRequirements requirements,
///            worker_requirements(config.device_requirements));
/// @endcode
/// @param program  A program's (PlayerViewConfig::device_requirements).
/// @return The merged requirements, or core::merge's failure. Host allocation
///         failures return `OutOfMemory`.
core::Result<core::DeviceRequirements> worker_requirements(
    const core::DeviceRequirements& program);

/// @brief The swapchain @ref PlayerView asks gfx for: gfx's FIFO default and
///        D32 depth; live, two images, so a mesh taken shows a vsync sooner,
///        and a file's, gfx's default (the surface's least + 1).
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

  /// @brief Make the shared device on @p surface
  ///        (gfx::app::make_shared_device), its compute side with
  ///        @ref worker_requirements of the config's device_requirements, and
  ///        adopt it for gfx (@ref view_swapchain, 2 frames in flight) and for
  ///        grid. Build the mesh pipeline, the overlay (no imgui.ini) and a
  ///        gfx::Profiler for the stats' "draw (mesh+UI)" row, which
  ///        @ref frame attaches to the frame loop while the draw is timed
  ///        (PlayerViewConfig::device_timing). Aim the camera at the first
  ///        view: target (0, 0.85, 0), 3 m. Make the hooks
  ///        (PlayerViewConfig::make_hooks). Then start @p player at the
  ///        config's tile depth, with the hooks; its worker builds its
  ///        kernels, and it plays unless the caller paused it.
  /// @param surface  The window system's source; its window or layer must
  ///                 outlive the view.
  /// @param extent   The drawable now, in pixels; both sides > 0.
  /// @param player   An opened player, not started.
  /// @param config   The UI's sizes, the Source button, validation, a
  ///                 measurement's timing, tile depth and labels, vertex
  ///                 sharing, the hooks and their device requirements, Unlit
  ///                 and Texture.
  /// @return The view; `core::Status::Code::InvalidArgument` for a null
  ///         @p player or an empty @p extent; or the first failure (device,
  ///         `Unsupported` naming what no GPU offers of a program's required
  ///         extensions or features among them, swapchain, pipeline, overlay,
  ///         profiler, make_hooks, StreamPlayer::start). Host allocation
  ///         failures return `OutOfMemory`.
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

  /// @brief One tick: draw the controls and the stats, take the newest mesh
  ///        (FrameTaker, with the commit hook), run before_draw, draw the
  ///        mesh behind a GPU wait on its `ready` value (already reached, so
  ///        it stalls nothing; it carries the worker's writes across queues)
  ///        with the binding it returned, unless Texture is off and it names
  ///        a texture set (@ref textured), and the view's own (Unlit), and
  ///        present. A frame taken is stamped when taken and when presented
  ///        (FrameStamp::taken, presented), and counted in the stats once
  ///        presented; a skipped frame is neither.
  /// @pre The UI platform half bound to @ref ui_context ran its new_frame this
  ///      tick, unless @p extent is 0 x 0 (a touch shell skips the tick's
  ///      new_frame then).
  /// @param extent  The drawable now, in pixels; 0 x 0 (minimized) skips.
  /// @return true once presented. false when nothing could render (0 x 0, or
  ///         the surface settling after a rebuild), so a desktop pump waits.
  ///         An error for device loss, a refused mesh, or the worker's own
  ///         failure, its kernel build included (StreamPlayer::status).
  ///         An allocation exception returns `OutOfMemory`; other exceptions
  ///         return `InvalidArgument`. After an exception, destroy and recreate
  ///         the view: subsequent ticks refuse without acquiring or submitting
  ///         a frame. Destruction still drains any submitted device work.
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
  /// @return Whether the mesh shows its vertex colours unshaded: the view's
  ///         own DrawBinding then clears kHybridMeshLit. @ref normals takes
  ///         precedence. PlayerViewConfig::unlit at first; the panel's Unlit
  ///         flips it.
  bool unlit() const noexcept { return unlit_; }
  /// @brief Show the vertex colours unshaded, or light them, from the next
  ///        frame (the desktop's U key).
  /// @param unlit  Whether to leave them unshaded.
  void set_unlit(bool unlit) noexcept { unlit_ = unlit; }
  /// @return Whether the mesh is drawn with the texture set its program's
  ///         before_draw binds (DrawBinding::descriptor_set). Off, a binding
  ///         that names a set is left out whole, its flags' clears too, so
  ///         the frame draws as with no binding: lit, unless Unlit; Normals
  ///         as ever. before_draw still runs each frame. A binding that names
  ///         no set applies either way. The panel shows the Texture box once
  ///         a before_draw has bound a set, and keeps it.
  ///         PlayerViewConfig::textured at first; the box flips it.
  bool textured() const noexcept { return textured_; }
  /// @brief Draw the mesh with its binding's texture set, or without it, from
  ///        the next frame (the desktop's T key).
  /// @param textured  Whether to draw it with the texture.
  void set_textured(bool textured) noexcept { textured_ = textured; }
  /// @return The flags the mesh draws with (gfx::pipelines::HybridMeshFlags)
  ///         before a DrawBinding clears any: kHybridMeshNormals while
  ///         @ref normals, else kHybridMeshLit.
  std::uint32_t mesh_flags() const noexcept;
  /// @return The frames a commit hook skipped (FrameTaker::frames_skipped).
  std::uint64_t frames_skipped() const noexcept {
    return taker_.frames_skipped();
  }
  /// @return The frames held before their commit or skip
  ///         (FrameTaker::frames_held).
  std::uint64_t frames_held() const noexcept { return taker_.frames_held(); }
  /// @brief Record the mesh as the last frame drew it: the live mesh under
  ///        the camera, with its flags less the frame's DrawBinding clears
  ///        and that binding's set. @ref frame records its draw through
  ///        this; with @ref target_layout's target, a caller can read back
  ///        what it drew, as the swapchain's images cannot be (the tests).
  /// @param cmd     A command buffer of the drawing device's, inside a pass
  ///                of a target laid out as @ref target_layout; whatever
  ///                submits it waits for the mesh's `ready` value.
  /// @param extent  The pass's, in pixels.
  void record_mesh(VkCommandBuffer cmd, VkExtent2D extent) const;
  /// @return The swapchain's target layout, which the mesh pipeline was built
  ///         for: a target of it takes @ref record_mesh.
  gfx::RenderTargetLayout target_layout() const;
  /// @return Whether the Source button (PlayerViewConfig::source_button) was
  ///         pressed since the last call: true once per press. The shell
  ///         takes it after @ref frame, so it asks what to play outside the
  ///         UI's frame.
  bool take_source_request() noexcept {
    return std::exchange(source_request_, false);
  }
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
  /// @return The stream frames taken for drawing so far, each once, and a
  ///         paused toggle's rebuilds (FrameStamp::rebuild): a frame that
  ///         raised it is @ref shown, for a measurement's log (MeasureLog,
  ///         which writes no rebuild).
  std::uint64_t frames_taken() const noexcept { return frames_taken_; }
  /// @return The device the worker decodes and meshes on, for a
  ///         measurement's device line (device_line).
  const core::Device& compute_device() const noexcept { return grid_device_; }
  /// @return Its mesh, in the extractor's ring, for a read-out of its counts;
  ///         generation 0 before the first.
  const grid::mesh::DeviceMesh& mesh() const noexcept {
    return exchange_.live();
  }
  /// @return The stats panel, for a read-out of its figures (vp_probe).
  const StatsPanel& stats_panel() const noexcept { return stats_panel_; }
  /// @return The renderer's figures this tick: gfx's profiler's newest
  ///         resolved frame while the draw is timed, else empty. It lags the
  ///         frames in flight, 2: the display frame drawn two ticks before.
  ///         For a measurement's log (MeasureLog::add).
  // TODO: hand a measurement the rows of the display frame that drew each
  // stream frame, once gfx's FrameMetrics names its frame (a gfx task).
  const gfx::FrameMetrics& render_metrics() const noexcept;
  /// @return The GPU waits the last frame submitted with
  ///         (gfx::windowing::Frame::waits): its mesh's `ready` value at
  ///         `VERTEX_INPUT | DRAW_INDIRECT` when it drew one. For a read-out
  ///         (the tests).
  const std::vector<gfx::windowing::FrameWait>& frame_waits() const noexcept {
    return frame_waits_;
  }

 private:
  static constexpr std::uint32_t kFramesInFlight = 2;
  static constexpr float kNear = 0.05f;  // clip planes, metres
  static constexpr float kFar = 100.0f;

  PlayerView(std::unique_ptr<core::SharedDevice> shared,
             gfx::app::WindowedApp app, gfx::Profiler profiler,
             core::Device grid_device, core::Allocator grid_allocator,
             gfx::pipelines::HybridMeshPipeline mesh_pipeline,
             gfx::ui::ImGuiOverlay overlay,
             const grid::mesh::MeshExchangeConfig& exchange,
             std::unique_ptr<StreamPlayer> player);

  // Declared in teardown order, reversed: StreamPlayer goes before its
  // exchange and device (stream_player.hpp), the profiler before the app it
  // is attached to, and both adopted devices before SharedDevice. All but the
  // player's controls are the drawing thread's.
  std::unique_ptr<core::SharedDevice> shared_;
  gfx::app::WindowedApp app_;  // gfx's device and the swapchain
  gfx::Profiler profiler_;     // on app_'s device
  core::Device grid_device_;
  core::Allocator grid_allocator_;
  gfx::pipelines::HybridMeshPipeline mesh_pipeline_;
  gfx::ui::ImGuiOverlay overlay_;
  FrameExchange exchange_;
  gfx::camera::OrbitCamera camera_;
  bool normals_ = false;         // the mesh's shading (mesh_flags)
  bool unlit_ = false;           // its vertex colours unshaded
  bool textured_ = false;        // drawn with before_draw's texture set
  bool texture_bound_ = false;   // a before_draw has bound one: Texture shows
  bool source_button_ = false;   // frame() draws Source
  bool source_request_ = false;  // pressed since take_source_request
  std::optional<DeviceTiming> device_timing_;  // fixed; else the panel's
  bool profiled_ = false;      // the draw timed this tick (render_metrics)
  bool frame_failed_ = false;  // an interrupted frame requires a new view
  core::Status hook_failure_;  // a commit or before_draw failure, kept
  std::unique_ptr<StreamPlayer> player_;
  FrameHooks hooks_;     // the worker's until stop, the drawing thread's
  FrameTaker taker_;     // over player_ and exchange_, with hooks_
  DrawBinding binding_;  // the last frame's draw's: before_draw's and Unlit
  PlaybackStats stats_;  // from player_'s track
  std::optional<FrameStamp> shown_;
  std::uint32_t shown_triangles_ = 0;  // shown_'s mesh's
  std::uint64_t frames_taken_ = 0;
  StatsPanel stats_panel_;
  std::vector<gfx::windowing::FrameWait> frame_waits_;  // the last frame's
};

}  // namespace volumetric_kit::player
