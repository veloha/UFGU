#include "render/ScaleformBoundary.hpp"

#include "config/Settings.hpp"
#include "render/DynamicResolution.hpp"
#include "render/NativeUiPolicy.hpp"
#include "render/PresentationBridge.hpp"
#include "render/SharedResources.hpp"
#include "render/UpscalingPass.hpp"
#include "streamline/SuperResolution.hpp"

#include <SKSE/SKSE.h>

namespace mfgdlss::render {
namespace {
namespace logger = SKSE::log;

constexpr std::size_t kBeginDisplayVtableIndex = 12;
constexpr std::size_t kEndDisplayVtableIndex = 13;

[[nodiscard]] RE::GRenderer *scaleform_renderer() noexcept {
  auto *const manager = RE::BSScaleformManager::GetSingleton();
  if (manager == nullptr || manager->renderer == nullptr) {
    return nullptr;
  }
  auto *const config = manager->renderer->config.get();
  return config != nullptr ? config->GetRenderer() : nullptr;
}
}

ScaleformBoundary &ScaleformBoundary::instance() noexcept {
  static ScaleformBoundary boundary;
  return boundary;
}

bool ScaleformBoundary::ensure_installed() noexcept {
  if (installed_) {
    return true;
  }
  auto *const renderer = scaleform_renderer();
  if (renderer == nullptr) {

    constexpr std::uint32_t kMaximumAttempts{600};
    if (++install_attempts_ == kMaximumAttempts && !renderer_absent_logged_) {
      renderer_absent_logged_ = true;
      logger::warn(
          "Skyrim's Scaleform renderer has not appeared after {} frames; "
          "the scene/UI boundary cannot be installed and direct UI "
          "separation stays unavailable",
          kMaximumAttempts);
    }
    return false;
  }

  const auto vtable_address = *reinterpret_cast<std::uintptr_t *>(renderer);
  auto *const entries = reinterpret_cast<std::uintptr_t *>(vtable_address);
  const auto begin_address = entries[kBeginDisplayVtableIndex];
  const auto end_address = entries[kEndDisplayVtableIndex];
  const auto begin_thunk_address =
      reinterpret_cast<std::uintptr_t>(begin_display_thunk);
  const auto end_thunk_address =
      reinterpret_cast<std::uintptr_t>(end_display_thunk);
  if (begin_address == 0 || end_address == 0 ||
      begin_address == begin_thunk_address ||
      end_address == end_thunk_address) {
    if (!invalid_vtable_logged_) {
      invalid_vtable_logged_ = true;
      logger::error("Scaleform lifecycle boundary rejected invalid or already-"
                    "hooked vtable entries");
    }
    return false;
  }

  original_begin_display_ =
      reinterpret_cast<BeginDisplayFunction>(begin_address);
  original_end_display_ = reinterpret_cast<EndDisplayFunction>(end_address);
  REL::Relocation<std::uintptr_t> vtable{vtable_address};
  const auto replaced_begin =
      vtable.write_vfunc(kBeginDisplayVtableIndex, begin_display_thunk);
  const auto replaced_end =
      vtable.write_vfunc(kEndDisplayVtableIndex, end_display_thunk);
  if (replaced_begin != begin_address || replaced_end != end_address) {

    vtable.write_vfunc(kBeginDisplayVtableIndex,
                       reinterpret_cast<BeginDisplayFunction>(replaced_begin));
    vtable.write_vfunc(kEndDisplayVtableIndex,
                       reinterpret_cast<EndDisplayFunction>(replaced_end));
    original_begin_display_ = nullptr;
    original_end_display_ = nullptr;
    if (!vtable_changed_logged_) {
      vtable_changed_logged_ = true;
      logger::error("Scaleform lifecycle boundary changed during installation; "
                    "both vtable slots were restored");
    }
    return false;
  }

  installed_ = true;
  logger::info(
      "Scaleform UI lifecycle boundary installed on the renderer at {}",
      static_cast<void *>(renderer));
  return true;
}

bool ScaleformBoundary::installed() const noexcept { return installed_; }

void ScaleformBoundary::begin_frame() noexcept {
  inside_display_ = false;
  displays_this_frame_ = 0;
  capture_pending_ = false;
  ui_phase_opened_this_frame_ = false;
}

bool ScaleformBoundary::ui_phase_opened_this_frame() const noexcept {
  return ui_phase_opened_this_frame_;
}

std::uint32_t ScaleformBoundary::displays_this_frame() const noexcept {
  return displays_this_frame_;
}

void ScaleformBoundary::note_first_composite() noexcept {
  if (stage_composite_logged_) {
    return;
  }
  stage_composite_logged_ = true;
  logger::info(
      "Scaleform boundary stage 5 of 5: first native UI composite completed; "
      "the whole boundary path has executed once without terminating");
}

void ScaleformBoundary::shutdown() noexcept {
  displays_this_frame_ = 0;
  capture_pending_ = false;
  ui_phase_opened_this_frame_ = false;
}

void ScaleformBoundary::begin_display_thunk(
    RE::GRenderer *const renderer, const RE::GColor *const background_color,
    const RE::GViewport &viewport, const float x0, const float x1,
    const float y0, const float y1) {
  auto &boundary = instance();

  if (!boundary.abi_confirmed_) {
    boundary.abi_confirmed_ = true;
    const auto address = reinterpret_cast<std::uintptr_t>(background_color);
    std::uint8_t alpha{};
    std::uint32_t raw{};
    if (background_color != nullptr) {
      alpha = background_color->colorData.channels.alpha;
      raw = background_color->colorData.raw;
    }
    logger::info(
        "Scaleform background colour received by hidden reference at "
        "{:#018x} ({} bits significant); alpha={} raw={:#010x}. The "
        "aggregate is no longer narrowed on the way through the boundary",
        address, address > 0xFFFFFFFFULL ? 64 : 32, alpha, raw);
  }

  boundary.before_display();

  original_begin_display_(renderer, background_color, viewport, x0, x1, y0, y1);

  boundary.after_display(viewport);
}

void ScaleformBoundary::end_display_thunk(RE::GRenderer *const renderer) {
  original_end_display_(renderer);

  instance().note_display_completed();
}

bool ScaleformBoundary::inside_display() const noexcept {
  return inside_display_;
}

void ScaleformBoundary::before_display() noexcept {
  inside_display_ = true;
  auto &presentation = PresentationBridge::instance();
  const auto virtual_render_surface =
      presentation.uses_virtual_render_surface();
  ++displays_this_frame_;
  if (capture_pending_ || ui_phase_opened_this_frame_) {
    return;
  }

  if (!virtual_render_surface) {
    const auto native_ui_policy = make_native_ui_policy(
        config::Settings::instance().frame_generation_enabled(), false,
        SharedResources::instance().ui_rendering());
    if (!native_ui_policy.capture_at_scaleform_boundary) {
      return;
    }

    if (!stage_before_logged_) {
      stage_before_logged_ = true;
      logger::info(
          "Scaleform boundary stage 1 of 5: entering the first "
          "Scaleform display of a native frame; preserving the "
          "completed backbuffer as HUD-less colour before the original "
          "BeginDisplay and substituting nothing");
    }

    auto &resources = SharedResources::instance();
    if (!resources.capture_hudless(presentation.d3d11_back_buffer()) ||
        !resources.frame_generation_hudless_captured()) {
      if (!hudless_preserve_failure_logged_) {
        hudless_preserve_failure_logged_ = true;
        logger::warn("The completed native frame could not be preserved at "
                     "the Scaleform UI boundary; direct UI separation and "
                     "frame generation are suspended for this frame");
      }
      return;
    }
    DynamicResolution::instance().begin_full_resolution_ui();
    if (!resources.begin_ui_rendering()) {
      DynamicResolution::instance().end_full_resolution_ui();
      if (!full_resolution_capture_start_logged_) {
        full_resolution_capture_start_logged_ = true;
        logger::warn("Direct transparent-target UI capture could not start "
                     "before the native Scaleform boundary; recomposition={} "
                     "hudless-captured={}",
                     resources.ui_recomposition_available(),
                     resources.hudless_captured());
      }
      return;
    }
    ui_phase_opened_this_frame_ = true;
    capture_pending_ = true;
    return;
  }

  auto &pass = UpscalingPass::instance();
  if (!pass.scene_resolve_pending()) {

    return;
  }

  if (!stage_before_logged_) {
    stage_before_logged_ = true;
    logger::info(
        "Scaleform boundary stage 1 of 5: entering the first Scaleform "
        "display of the frame with a resolve owed; committing it before the "
        "original BeginDisplay and binding nothing");
  }

  if (!pass.commit_pending_scene_resolve("scaleform-ui-lifecycle")) {
    if (!scene_resolve_incomplete_logged_) {
      scene_resolve_incomplete_logged_ = true;
      logger::warn("The scene resolve did not complete at the Scaleform UI "
                   "boundary; this frame's UI stays on the reduced surface and "
                   "frame generation is not eligible");
    }
    return;
  }
  auto &resources = SharedResources::instance();
  if (!resources.begin_ui_rendering()) {
    DynamicResolution::instance().end_full_resolution_ui();
    if (!reduced_capture_start_logged_) {
      reduced_capture_start_logged_ = true;
      logger::warn("Direct transparent-target UI capture could not start "
                   "before the reduced-resolution Scaleform boundary; "
                   "recomposition={} hudless-captured={}",
                   resources.ui_recomposition_available(),
                   resources.hudless_captured());
    }
    return;
  }
  ui_phase_opened_this_frame_ = true;
  capture_pending_ = true;
}

void ScaleformBoundary::after_display(const RE::GViewport &viewport) noexcept {
  auto &resources = SharedResources::instance();
  if (!capture_pending_) {
    if (ui_phase_opened_this_frame_) {

      resources.rebind_ui_target();
    }
    return;
  }
  capture_pending_ = false;

  if (!stage_after_logged_) {
    stage_after_logged_ = true;
    if (PresentationBridge::instance().uses_virtual_render_surface()) {
      logger::info("Scaleform boundary stage 2 of 5: the original "
                   "GRenderer::BeginDisplay returned normally against Skyrim's "
                   "own reduced framebuffer and cached display state");
    } else {
      logger::info("Scaleform boundary stage 2 of 5: the original "
                   "GRenderer::BeginDisplay returned normally against Skyrim's "
                   "own native backbuffer and cached display state");
    }
  }

  if (!resources.ui_rendering()) {
    ui_phase_opened_this_frame_ = false;
    DynamicResolution::instance().end_full_resolution_ui();
    if (!capture_ended_early_logged_) {
      capture_ended_early_logged_ = true;
      logger::warn("Direct transparent-target UI capture ended unexpectedly "
                   "inside the Scaleform boundary; recomposition={} "
                   "hudless-captured={}",
                   resources.ui_recomposition_available(),
                   resources.hudless_captured());
    }
    return;
  }
  resources.rebind_ui_target();

  if (!stage_bound_logged_) {
    stage_bound_logged_ = true;
    logger::info("Scaleform boundary stage 3 of 5: pre-BeginDisplay native UI "
                 "colour target and D24S8 companion remain active and were "
                 "rebound after display setup; Skyrim's raw "
                 "renderTargets[kFRAMEBUFFER] entry was not written");
  }

  constexpr std::uint32_t kMaximumReports{3};
  if (boundary_reports_ < kMaximumReports) {
    ++boundary_reports_;
    const auto &super_resolution = streamline::SuperResolution::instance();
    if (PresentationBridge::instance().uses_virtual_render_surface()) {
      logger::info(
          "Scene/UI boundary {} of {} taken at Skyrim's first Scaleform "
          "display of the frame: movie viewport={}x{} at {},{} "
          "buffer={}x{}; reduced {}x{} resolved to native {}x{}; "
          "Skyrim's HUD, console and menus now rasterise into the "
          "transparent native UI layer",
          boundary_reports_, kMaximumReports, viewport.width, viewport.height,
          viewport.left, viewport.top, viewport.bufferWidth,
          viewport.bufferHeight, super_resolution.render_width(),
          super_resolution.render_height(), super_resolution.output_width(),
          super_resolution.output_height());
    } else {
      logger::info(
          "Scene/UI boundary {} of {} taken at Skyrim's first Scaleform "
          "display of the frame: movie viewport={}x{} at {},{} "
          "buffer={}x{}; exact native {}x{} HUD-less colour preserved; "
          "Skyrim's HUD, console and menus now rasterise into the "
          "transparent native UI layer",
          boundary_reports_, kMaximumReports, viewport.width, viewport.height,
          viewport.left, viewport.top, viewport.bufferWidth,
          viewport.bufferHeight, super_resolution.output_width(),
          super_resolution.output_height());
    }
  }
}

void ScaleformBoundary::note_display_completed() noexcept {
  inside_display_ = false;
  if (ui_phase_opened_this_frame_) {
    SharedResources::instance().bind_output_target_after_ui_display();
  }
  if (stage_display_completed_logged_ || !ui_phase_opened_this_frame_) {
    return;
  }
  stage_display_completed_logged_ = true;
  logger::info(
      "Scaleform boundary stage 4 of 5: the first Scaleform display "
      "completed on the native UI layer and EndDisplay returned normally");
}
}
