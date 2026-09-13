#include "render/DynamicResolution.hpp"

#include "render/ScaleformBoundary.hpp"

#include "config/Settings.hpp"
#include "enb/EnbApi.hpp"
#include "render/CameraData.hpp"
#include "render/JitterOwnership.hpp"
#include "render/MainDepthTracker.hpp"
#include "render/PresentationBridge.hpp"
#include "render/RenderDebug.hpp"
#include "render/RuntimeCompatibilitySkse.hpp"
#include "render/SharedResources.hpp"
#include "render/UpscalingPass.hpp"
#include "streamline/SuperResolution.hpp"

#include <SKSE/SKSE.h>

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <wrl/client.h>

namespace mfgdlss::render {
namespace {
namespace logger = SKSE::log;
using Microsoft::WRL::ComPtr;

std::ptrdiff_t kProjectionScaleXOffset = 0x44;
std::ptrdiff_t kProjectionScaleYOffset = 0x48;
std::ptrdiff_t kFrameCountOffset = 0x4C;
std::ptrdiff_t kDynamicWidthScaleOffset = 0xFC;
std::ptrdiff_t kDynamicHeightScaleOffset = 0x100;
std::ptrdiff_t kDynamicPreviousWidthScaleOffset = 0x104;
std::ptrdiff_t kDynamicPreviousHeightScaleOffset = 0x108;
std::ptrdiff_t kDynamicResolutionLockOffset = 0x110;
std::ptrdiff_t kImageSpaceRuntimeStateOffset = 0x1F0;
std::ptrdiff_t kRuntimeTaaEnabledOffset = 0x18;
constexpr float kViewportMatchTolerance = 1.0F;
constexpr LONG kScissorMatchTolerance = 1;

struct RenderTargetExtent {
  std::uint32_t width{};
  std::uint32_t height{};
};

[[nodiscard]] RenderTargetExtent
bound_render_target_extent(ID3D11DeviceContext *context) noexcept {
  if (context == nullptr) {
    return {};
  }

  ComPtr<ID3D11RenderTargetView> target;
  context->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
  if (target == nullptr) {
    return {};
  }

  ComPtr<ID3D11Resource> resource;
  target->GetResource(resource.GetAddressOf());
  if (resource == nullptr) {
    return {};
  }

  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(resource.As(&texture)) || texture == nullptr) {
    return {};
  }

  D3D11_TEXTURE2D_DESC description{};
  texture->GetDesc(&description);
  return {description.Width, description.Height};
}

[[nodiscard]] bool
matches_viewport_extent(const D3D11_VIEWPORT &viewport,
                        const std::uint32_t width,
                        const std::uint32_t height) noexcept {
  return std::fabs(viewport.TopLeftX) <= kViewportMatchTolerance &&
         std::fabs(viewport.TopLeftY) <= kViewportMatchTolerance &&
         std::fabs(viewport.Width - static_cast<float>(width)) <=
             kViewportMatchTolerance &&
         std::fabs(viewport.Height - static_cast<float>(height)) <=
             kViewportMatchTolerance;
}

[[nodiscard]] bool matches_scissor_extent(const D3D11_RECT &rectangle,
                                          const std::uint32_t width,
                                          const std::uint32_t height) noexcept {
  return std::abs(rectangle.left) <= kScissorMatchTolerance &&
         std::abs(rectangle.top) <= kScissorMatchTolerance &&
         std::abs(rectangle.right - static_cast<LONG>(width)) <=
             kScissorMatchTolerance &&
         std::abs(rectangle.bottom - static_cast<LONG>(height)) <=
             kScissorMatchTolerance;
}

[[nodiscard]] bool memory_allows_access(const void *address,
                                        const std::size_t size,
                                        const bool require_write) noexcept {
  if (address == nullptr || size == 0) {
    return false;
  }

  MEMORY_BASIC_INFORMATION information{};
  if (VirtualQuery(address, &information, sizeof(information)) !=
          sizeof(information) ||
      information.State != MEM_COMMIT ||
      (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
    return false;
  }

  const auto protection = information.Protect & 0xFFU;
  const auto readable =
      protection == PAGE_READONLY || protection == PAGE_READWRITE ||
      protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
      protection == PAGE_EXECUTE_READWRITE ||
      protection == PAGE_EXECUTE_WRITECOPY;
  const auto writable = protection == PAGE_READWRITE ||
                        protection == PAGE_WRITECOPY ||
                        protection == PAGE_EXECUTE_READWRITE ||
                        protection == PAGE_EXECUTE_WRITECOPY;
  if (!readable || (require_write && !writable)) {
    return false;
  }

  const auto begin = reinterpret_cast<std::uintptr_t>(address);
  const auto region_begin =
      reinterpret_cast<std::uintptr_t>(information.BaseAddress);
  const auto region_end = region_begin + information.RegionSize;
  return begin >= region_begin && begin <= region_end &&
         size <= region_end - begin;
}

[[nodiscard]] bool *runtime_taa_flag() noexcept {
  static REL::Relocation<std::byte *&> image_space_state{
      REL::RelocationID(527731, 414660)};
  auto *outer = image_space_state.get();
  if (outer == nullptr ||
      !memory_allows_access(outer + kImageSpaceRuntimeStateOffset,
                            sizeof(std::byte *), false)) {
    return nullptr;
  }

  std::byte *inner{};
  std::memcpy(&inner, outer + kImageSpaceRuntimeStateOffset, sizeof(inner));
  if (inner == nullptr ||
      !memory_allows_access(inner + kRuntimeTaaEnabledOffset, sizeof(bool),
                            true)) {
    return nullptr;
  }

  auto *flag = reinterpret_cast<bool *>(inner + kRuntimeTaaEnabledOffset);
  const auto value = static_cast<unsigned char>(*flag);
  return value <= 1U ? flag : nullptr;
}

template <class Value>
[[nodiscard]] Value &state_value(RE::BSGraphics::State *state,
                                 const std::ptrdiff_t offset) noexcept {
  return *reinterpret_cast<Value *>(reinterpret_cast<std::byte *>(state) +
                                    offset);
}

[[nodiscard]] float halton(std::uint32_t index,
                           const std::uint32_t base) noexcept {
  float result{};
  float fraction = 1.0F;
  while (index != 0) {
    fraction /= static_cast<float>(base);
    result += fraction * static_cast<float>(index % base);
    index /= base;
  }
  return result;
}

[[nodiscard]] bool
uses_full_resolution_menu(const RE::BSFixedString &menu) noexcept {
  return menu == RE::MainMenu::MENU_NAME ||
         menu == RE::LoadingMenu::MENU_NAME ||
         menu == RE::RaceSexMenu::MENU_NAME;
}

void update_camera_data() {
  using Function = void();
  static REL::Relocation<Function> function{REL::RelocationID(75472, 77258)};
  function();
}

struct SurfaceModelState final {

  bool complete_frame{};

  bool sub_rect{};
};

[[nodiscard]] SurfaceModelState surface_model_state() noexcept {
  const auto &presentation = PresentationBridge::instance();
  SurfaceModelState surface{};
  surface.sub_rect = presentation.uses_sub_rect_render_surface();
  surface.complete_frame =
      presentation.uses_virtual_render_surface() && !surface.sub_rect;
  return surface;
}

[[nodiscard]] bool validate_branch(const std::uintptr_t address,
                                   const CodeSignature &signature,
                                   const char *const label) noexcept {
  if (address == 0U || signature.length == 0U ||
      signature.length > kMaximumHookSignatureBytes) {
    return false;
  }
  auto *const bytes = reinterpret_cast<const std::uint8_t *>(address);
  if (!memory_allows_access(reinterpret_cast<std::byte *>(address),
                            signature.length, false)) {
    logger::warn(
        "Jitter fold: the {} branch region at 0x{:016X} is not readable; "
        "refusing to patch",
        label, address);
    return false;
  }

  std::string original;
  original.reserve(signature.length * 3U);
  for (std::size_t index = 0; index < signature.length; ++index) {
    const auto value = bytes[index];
    original += std::format("{:02X} ", value);
  }
  logger::info(
      "Jitter fold: {} branch region at 0x{:016X}, {} bytes, currently: {}",
      label, address, signature.length, original);

  const auto validation = validate_code_signature(
      std::span<const std::uint8_t>(bytes, signature.length), signature);
  if (validation != HookValidationFailure::none) {
    logger::error(
        "Jitter fold: the {} branch signature was rejected ({}); refusing "
        "to patch",
        label, static_cast<unsigned>(validation));
    return false;
  }
  return true;
}

void neutralise_verified_branch(const std::uintptr_t address,
                                const std::size_t length,
                                const char *const label) noexcept {
  std::array<std::uint8_t, kMaximumHookSignatureBytes> nops{};
  nops.fill(0x90U);
  REL::safe_write(address, nops.data(), length);
  logger::info("Jitter fold: {} branch neutralised at 0x{:016X}", label,
               address);
}

[[nodiscard]] std::int32_t dynamic_resolution_lock_for(
    const SurfaceModelState &surface, const bool reduced_resolution,
    const bool upscaler_enabled, const std::int32_t original) noexcept {

  if (surface.sub_rect) {
    return 1;
  }
  if (reduced_resolution) {
    return 0;
  }
  return upscaler_enabled ? 1 : original;
}
}

DynamicResolution &DynamicResolution::instance() noexcept {
  static DynamicResolution resolution;
  return resolution;
}

bool DynamicResolution::install() {
  if (installed_) {
    return true;
  }
  const auto *const compatibility = active_runtime_profile();
  if (compatibility == nullptr) {
    logger::error("Dynamic-resolution hooks do not support Skyrim {}",
                  REL::Module::get().version().string());
    return false;
  }

  kProjectionScaleXOffset = compatibility->state.projection_scale_x;
  kProjectionScaleYOffset = compatibility->state.projection_scale_y;
  kFrameCountOffset = compatibility->state.frame_count;
  kDynamicWidthScaleOffset = compatibility->state.dynamic_width_scale;
  kDynamicHeightScaleOffset = compatibility->state.dynamic_height_scale;
  kDynamicPreviousWidthScaleOffset = compatibility->state.previous_width_scale;
  kDynamicPreviousHeightScaleOffset = compatibility->state.previous_height_scale;
  kDynamicResolutionLockOffset = compatibility->state.dynamic_resolution_lock;
  kImageSpaceRuntimeStateOffset = compatibility->state.image_space_runtime_state;
  kRuntimeTaaEnabledOffset = compatibility->state.runtime_taa_enabled;

  const auto resolution_address =
      REL::ID(compatibility->address_ids.dynamic_resolution_update).address() +
      compatibility->hooks.resolution_call;
  const auto jitter_address =
      REL::ID(compatibility->address_ids.temporal_jitter_update).address() +
      compatibility->hooks.jitter_call;
  const auto resolution_preflight = preflight_direct_call(
      resolution_address, compatibility->hooks.resolution_call_signature);
  const auto jitter_preflight = preflight_direct_call(
      jitter_address, compatibility->hooks.jitter_call_signature);
  const auto module_base = REL::Module::get().base();
  logger::info(
      "Hook preflight on {}: resolution call at rva 0x{:X} -> {} (target rva "
      "0x{:X}), jitter call at rva 0x{:X} -> {} (target rva 0x{:X})",
      compatibility->name, resolution_address - module_base,
      hook_validation_failure_name(resolution_preflight.failure),
      resolution_preflight.target != 0U
          ? resolution_preflight.target - module_base
          : 0U,
      jitter_address - module_base,
      hook_validation_failure_name(jitter_preflight.failure),
      jitter_preflight.target != 0U ? jitter_preflight.target - module_base
                                    : 0U);
  if (!atomic_patch_allowed(resolution_preflight.valid(),
                            jitter_preflight.valid())) {
    logger::error(
        "Dynamic-resolution and temporal-jitter hooks refused on {}: the "
        "bytes at the call sites do not match this runtime profile, so "
        "patching them could corrupt the game's code. Nothing was patched and "
        "upscaling and frame generation stay unavailable",
        compatibility->name);
    return false;
  }

  dynamic_resolution_setting_ =
      RE::GetINISetting("bEnableAutoDynamicResolution:Display");
  dynamic_resolution_clamp_setting_ =
      RE::GetINISetting("fDRClampOffset:Display");
  temporal_aa_setting_ = RE::GetINISetting("bUseTAA:Display");
  if (dynamic_resolution_setting_ == nullptr ||
      dynamic_resolution_clamp_setting_ == nullptr ||
      temporal_aa_setting_ == nullptr) {
    logger::error("Required Skyrim display settings were not found");
    return false;
  }
  original_dynamic_resolution_enabled_ = dynamic_resolution_setting_->GetBool();
  original_dynamic_resolution_clamp_ =
      dynamic_resolution_clamp_setting_->GetFloat();
  original_temporal_aa_enabled_ = temporal_aa_setting_->GetBool();
  if (auto *state = RE::BSGraphics::State::GetSingleton(); state != nullptr) {
    original_dynamic_resolution_lock_ =
        state_value<std::int32_t>(state, kDynamicResolutionLockOffset);
  }

  logger::info(
      "Skyrim display state captured: dynamic-resolution={}, clamp={:.3f}, "
      "TAA={}",
      original_dynamic_resolution_enabled_, original_dynamic_resolution_clamp_,
      original_temporal_aa_enabled_);

  if (streamline::SuperResolution::instance().enabled() &&
      !PresentationBridge::instance().uses_virtual_render_surface()) {
    dynamic_resolution_setting_->data.b = true;
    dynamic_resolution_clamp_setting_->data.f = 0.0F;
    logger::info("Skyrim dynamic-resolution lifecycle enabled for native DLSS; "
                 "menus retain a 1.0 scene ratio");
  }

  if (config::Settings::instance().jitter_fold() ==
      config::JitterFold::nop_gate) {
    const auto jitter_update =
        REL::ID(compatibility->address_ids.jitter_update_gate).address() +
        compatibility->hooks.jitter_update_patch;
    const auto camera_state =
        REL::ID(compatibility->address_ids.camera_state_build).address() +
        compatibility->hooks.camera_state_patch;
    const auto first = validate_branch(
        jitter_update, compatibility->hooks.jitter_update_patch_signature,
        "jitter-update");
    const auto second = validate_branch(
        camera_state, compatibility->hooks.camera_state_patch_signature,
        "camera-state-build");
    if (first && second) {
      neutralise_verified_branch(
          jitter_update,
          compatibility->hooks.jitter_update_patch_signature.length,
          "jitter-update");
      neutralise_verified_branch(
          camera_state,
          compatibility->hooks.camera_state_patch_signature.length,
          "camera-state-build");
      jitter_fold_patched_ = true;
    }
    logger::info(
        "Jitter fold gate: JitterFold=NopGate requested; "
        "jitter-update={}, camera-state-build={}, fold-enabled={}. With "
        "fold-enabled the live image-space TAA state is held FALSE for the "
        "whole frame, so Skyrim's own TAA resolve never runs and the "
        "upscaler receives a raster that has not already been resolved "
        "temporally. Do not read engine-applied as evidence either way: "
        "it compares matrices that are not a jittered/unjittered pair.",
        first, second, jitter_fold_patched_);
  } else {
    logger::info(
        "Jitter fold gate: JitterFold=Engine; no code patch applied. The "
        "raster is expected to carry no jitter in this mode.");
  }

  if (enb::Api::instance().connected()) {
    if (PresentationBridge::instance().uses_virtual_render_surface()) {
      logger::info("External post-processing renderer detected; complete-frame "
                   "input surface replaces viewport-only dynamic resolution");
    } else {
      logger::info("External post-processing renderer detected; native DLSS "
                   "will resolve HDR kMAIN before image-space processing");
    }
  }

  SKSE::AllocTrampoline(128);
  auto &trampoline = SKSE::GetTrampoline();
  original_update_resolution_ =
      trampoline.write_call<5>(resolution_address, update_resolution_thunk);
  original_update_jitter_ =
      trampoline.write_call<5>(jitter_address, update_jitter_thunk);

  auto *ui = RE::UI::GetSingleton();
  if (ui != nullptr) {
    ui->GetEventSource<RE::MenuOpenCloseEvent>()->AddEventSink(this);
    menu_override_ = ui->GameIsPaused() ||
                     ui->IsMenuOpen(RE::MainMenu::MENU_NAME) ||
                     ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) ||
                     ui->IsMenuOpen(RE::RaceSexMenu::MENU_NAME);
  } else {
    logger::warn(
        "UI event source unavailable; menu resolution override disabled");
  }

  installed_ = true;
  logger::info("Skyrim dynamic-resolution and temporal-jitter hooks installed");
  return true;
}

bool DynamicResolution::attach_context(ID3D11DeviceContext *context) {
  if (viewport_hook_installed_) {
    return true;
  }
  if (context == nullptr) {
    return false;
  }

  constexpr std::size_t kOmSetRenderTargetsVtableIndex = 33;
  constexpr std::size_t kOmSetRenderTargetsAndUnorderedAccessViewsVtableIndex =
      34;
  constexpr std::size_t kRsSetViewportsVtableIndex = 44;
  constexpr std::size_t kRsSetScissorRectsVtableIndex = 45;
  constexpr std::size_t kClearDepthStencilViewVtableIndex = 53;
  const auto vtable_address = *reinterpret_cast<std::uintptr_t *>(context);
  REL::Relocation<std::uintptr_t> vtable{vtable_address};
  original_om_set_render_targets_ =
      reinterpret_cast<OmSetRenderTargetsFunction>(vtable.write_vfunc(
          kOmSetRenderTargetsVtableIndex, om_set_render_targets_thunk));
  original_om_set_render_targets_and_unordered_access_views_ =
      reinterpret_cast<OmSetRenderTargetsAndUnorderedAccessViewsFunction>(
          vtable.write_vfunc(
              kOmSetRenderTargetsAndUnorderedAccessViewsVtableIndex,
              om_set_render_targets_and_unordered_access_views_thunk));
  original_rs_set_viewports_ = reinterpret_cast<RsSetViewportsFunction>(
      vtable.write_vfunc(kRsSetViewportsVtableIndex, rs_set_viewports_thunk));
  original_rs_set_scissor_rects_ =
      reinterpret_cast<RsSetScissorRectsFunction>(vtable.write_vfunc(
          kRsSetScissorRectsVtableIndex, rs_set_scissor_rects_thunk));
  original_clear_depth_stencil_view_ =
      reinterpret_cast<ClearDepthStencilViewFunction>(vtable.write_vfunc(
          kClearDepthStencilViewVtableIndex, clear_depth_stencil_view_thunk));
  if (original_om_set_render_targets_ == nullptr ||
      original_om_set_render_targets_and_unordered_access_views_ == nullptr ||
      original_rs_set_viewports_ == nullptr ||
      original_rs_set_scissor_rects_ == nullptr ||
      original_clear_depth_stencil_view_ == nullptr) {
    logger::error(
        "Unable to install D3D11 scene-viewport/post-processing extent "
        "hooks");
    return false;
  }

  viewport_hook_installed_ = true;
  logger::info(
      "D3D11 order-independent scene-viewport and post-processing extent "
      "hooks installed");
  return true;
}

void DynamicResolution::shutdown() noexcept {
  MainDepthTracker::instance().reset();
  const auto dynamic_setting_restored = dynamic_resolution_setting_ != nullptr;
  if (dynamic_setting_restored) {
    dynamic_resolution_setting_->data.b = original_dynamic_resolution_enabled_;
  }
  const auto clamp_setting_restored =
      dynamic_resolution_clamp_setting_ != nullptr;
  if (clamp_setting_restored) {
    dynamic_resolution_clamp_setting_->data.f =
        original_dynamic_resolution_clamp_;
  }
  const auto setting_restored = temporal_aa_setting_ != nullptr;
  if (setting_restored) {
    temporal_aa_setting_->data.b = original_temporal_aa_enabled_;
  }
  auto runtime_restored = true;
  if (runtime_temporal_state_captured_) {
    auto *const flag = runtime_taa_flag();
    runtime_restored = flag != nullptr;
    if (runtime_restored) {
      *flag = original_runtime_temporal_aa_enabled_;
    }
  }
  auto *const graphics_state = RE::BSGraphics::State::GetSingleton();
  const auto graphics_state_restored = graphics_state != nullptr;
  if (graphics_state_restored) {
    state_value<float>(graphics_state, kDynamicWidthScaleOffset) = 1.0F;
    state_value<float>(graphics_state, kDynamicHeightScaleOffset) = 1.0F;
    state_value<float>(graphics_state, kDynamicPreviousWidthScaleOffset) = 1.0F;
    state_value<float>(graphics_state, kDynamicPreviousHeightScaleOffset) =
        1.0F;
    state_value<std::int32_t>(graphics_state, kDynamicResolutionLockOffset) =
        original_dynamic_resolution_lock_;
  }
  if (!dynamic_setting_restored || !clamp_setting_restored ||
      !setting_restored || !runtime_restored || !graphics_state_restored) {

    logger::warn(
        "Skyrim's own graphics state could not be fully put back on teardown. "
        "Restored: bDynamicResolution={}, fDynamicResolutionClamp={}, "
        "anti-aliasing setting={}, live anti-aliasing flag={}, live render "
        "scale and resolution lock={}. This plugin promises to return every "
        "setting it touches exactly as it was. Whichever of those reads false "
        "was left as this plugin had it. Check Settings, Display in game, and "
        "bUseTAA, bDynamicResolution and fDynamicResolutionClamp in "
        "SkyrimPrefs.ini, before assuming another mod changed them",
        dynamic_setting_restored,
        clamp_setting_restored,
        setting_restored,
        runtime_restored,
        graphics_state_restored);
  }
  jitter_index_ = 0;
  jitter_x_ = 0.0F;
  jitter_y_ = 0.0F;
  active_frame_ = UINT32_MAX;
  expected_viewport_width_ = 0;
  expected_viewport_height_ = 0;
  expected_viewport_seen_ = false;
  original_projection_scale_x_ = 0.0F;
  original_projection_scale_y_ = 0.0F;
  scene_scale_ = 1.0F;
  last_scene_scale_ = 1.0F;
  menu_override_ = false;
  should_evaluate_ = false;
  temporal_aa_override_active_ = false;
  temporal_aa_override_logged_ = false;
  dynamic_resolution_mode_initialized_ = false;
  dynamic_resolution_active_ = false;
  runtime_temporal_state_captured_ = false;
  runtime_temporal_state_logged_ = false;
  runtime_taa_flag_absent_logged_ = false;
  runtime_taa_write_rejected_logged_ = false;
  full_resolution_restore_logged_ = false;
  full_resolution_ui_ = false;
  ui_resolution_promotion_logged_ = false;
  ui_target_redirection_logged_ = false;
  ui_target_redirection_failed_ = false;
  ui_target_failure_logged_ = false;
  full_resolution_post_processing_ = false;
  post_processing_guard_armed_logged_ = false;
  post_processing_promotion_logged_ = false;
  post_processing_guard_summary_logged_ = false;
  virtual_viewport_correction_logged_ = false;
  virtual_scissor_correction_logged_ = false;
}

float DynamicResolution::engine_projection_scale_x() const noexcept {
  return projection_scale_trace_.jitter_entry_x;
}

float DynamicResolution::engine_projection_scale_y() const noexcept {
  return projection_scale_trace_.jitter_entry_y;
}

float DynamicResolution::jitter_x() const noexcept { return jitter_x_; }

float DynamicResolution::jitter_y() const noexcept { return jitter_y_; }

FoldGateObservation DynamicResolution::observe_fold_gate() const noexcept {

  FoldGateObservation observation{};
  if (auto *state = RE::BSGraphics::State::GetSingleton(); state != nullptr) {
    observation.state_available = true;
    observation.projection_scale_x =
        state_value<float>(state, kProjectionScaleXOffset);
    observation.projection_scale_y =
        state_value<float>(state, kProjectionScaleYOffset);
  }
  if (const auto *flag = runtime_taa_flag(); flag != nullptr) {
    observation.taa_flag_available = true;
    observation.taa_flag_enabled = *flag;
  }
  return observation;
}

bool DynamicResolution::should_evaluate() const noexcept {
  return should_evaluate_;
}

bool DynamicResolution::scene_viewport_verified() const noexcept {
  return expected_viewport_seen_;
}

bool DynamicResolution::ui_target_redirection_failed() const noexcept {

  return ui_target_redirection_failed_ || ui_target_declined_this_frame_;
}

bool DynamicResolution::full_resolution_ui_active() const noexcept {
  return full_resolution_ui_;
}

RE::BSEventNotifyControl
DynamicResolution::ProcessEvent(const RE::MenuOpenCloseEvent *event,
                                RE::BSTEventSource<RE::MenuOpenCloseEvent> *) {
  if (event != nullptr && uses_full_resolution_menu(event->menuName)) {
    menu_override_ = event->opening;
    jitter_index_ = 0;
  } else if (event != nullptr && event->menuName == RE::FaderMenu::MENU_NAME &&
             !event->opening) {
    menu_override_ = false;
    jitter_index_ = 0;
  }
  return RE::BSEventNotifyControl::kContinue;
}

void DynamicResolution::update_resolution_thunk(RE::BSGraphics::State *state) {

  auto &self = instance();
  if (state != nullptr) {
    self.projection_scale_trace_.resolution_entry_x =
        state_value<float>(state, kProjectionScaleXOffset);
    self.projection_scale_trace_.resolution_entry_y =
        state_value<float>(state, kProjectionScaleYOffset);
  }
  self.update_resolution(state);
  original_update_resolution_(state);
  self.reapply_scene_scale(state);
  if (state != nullptr) {
    self.projection_scale_trace_.resolution_exit_x =
        state_value<float>(state, kProjectionScaleXOffset);
    self.projection_scale_trace_.resolution_exit_y =
        state_value<float>(state, kProjectionScaleYOffset);
    self.projection_scale_trace_.resolution_seen = true;
  }
}

void DynamicResolution::update_jitter_thunk(RE::BSGraphics::State *state) {

  instance().enable_vanilla_taa_for_world_render();
  original_update_jitter_(state);
  instance().update_jitter(state);
}

void STDMETHODCALLTYPE DynamicResolution::rs_set_viewports_thunk(
    ID3D11DeviceContext *context, const UINT viewport_count,
    const D3D11_VIEWPORT *viewports) {
  auto &resolution = instance();
  constexpr auto kMaximumCount =
      D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  std::array<D3D11_VIEWPORT, kMaximumCount> adjusted{};
  const auto *applied_viewports = viewports;
  auto promoted = false;
  const auto &super_resolution = streamline::SuperResolution::instance();
  if ((resolution.full_resolution_post_processing_ ||
       resolution.full_resolution_ui_) &&
      viewports != nullptr && viewport_count != 0 &&
      viewport_count <= adjusted.size() &&
      resolution.full_resolution_target_bound(context)) {
    std::copy_n(viewports, viewport_count, adjusted.begin());
    for (UINT index = 0; index < viewport_count; ++index) {
      if (matches_viewport_extent(adjusted[index],
                                  super_resolution.render_width(),
                                  super_resolution.render_height())) {
        adjusted[index].TopLeftX = 0.0F;
        adjusted[index].TopLeftY = 0.0F;
        adjusted[index].Width =
            static_cast<float>(super_resolution.output_width());
        adjusted[index].Height =
            static_cast<float>(super_resolution.output_height());
        promoted = true;
      }
    }
    applied_viewports = adjusted.data();
  }
  original_rs_set_viewports_(context, viewport_count, applied_viewports);
  if (PresentationBridge::instance().uses_virtual_render_surface()) {
    resolution.enforce_virtual_render_extent(context);
    resolution.observe_current_viewports(context);
  } else {
    resolution.observe_viewports(context, viewport_count, applied_viewports);
  }
  if (promoted) {
    if (resolution.full_resolution_ui_) {
      if (!resolution.ui_resolution_promotion_logged_) {
        resolution.ui_resolution_promotion_logged_ = true;
        logger::info("Promoted Skyrim UI viewport from the DLSS render "
                     "extent to the native output extent");
      }
    } else {
      resolution.log_post_processing_promotion(true, false);
    }
  }
}

void STDMETHODCALLTYPE DynamicResolution::rs_set_scissor_rects_thunk(
    ID3D11DeviceContext *context, const UINT rectangle_count,
    const D3D11_RECT *rectangles) {
  auto &resolution = instance();
  constexpr auto kMaximumCount =
      D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  std::array<D3D11_RECT, kMaximumCount> adjusted{};
  const auto *applied_rectangles = rectangles;
  auto promoted = false;
  const auto &super_resolution = streamline::SuperResolution::instance();
  if ((resolution.full_resolution_post_processing_ ||
       resolution.full_resolution_ui_) &&
      rectangles != nullptr && rectangle_count != 0 &&
      rectangle_count <= adjusted.size() &&
      resolution.full_resolution_target_bound(context)) {
    std::copy_n(rectangles, rectangle_count, adjusted.begin());
    for (UINT index = 0; index < rectangle_count; ++index) {
      if (matches_scissor_extent(adjusted[index],
                                 super_resolution.render_width(),
                                 super_resolution.render_height())) {
        adjusted[index].left = 0;
        adjusted[index].top = 0;
        adjusted[index].right =
            static_cast<LONG>(super_resolution.output_width());
        adjusted[index].bottom =
            static_cast<LONG>(super_resolution.output_height());
        promoted = true;
      }
    }
    applied_rectangles = adjusted.data();
  }
  original_rs_set_scissor_rects_(context, rectangle_count, applied_rectangles);
  if (PresentationBridge::instance().uses_virtual_render_surface()) {
    resolution.enforce_virtual_render_extent(context);
  }
  if (promoted && !resolution.full_resolution_ui_) {
    resolution.log_post_processing_promotion(false, true);
  }
}

namespace {

[[nodiscard]] const void *
identity_of(ID3D11RenderTargetView *const view) noexcept {
  if (view == nullptr) {
    return nullptr;
  }
  ComPtr<ID3D11Resource> resource;
  view->GetResource(resource.GetAddressOf());
  return static_cast<const void *>(resource.Get());
}

[[nodiscard]] const void *com_identity(IUnknown *const object) noexcept {
  if (object == nullptr) {
    return nullptr;
  }
  ComPtr<IUnknown> canonical;
  if (FAILED(object->QueryInterface(IID_PPV_ARGS(canonical.GetAddressOf()))) ||
      !canonical) {

    return static_cast<const void *>(object);
  }
  return static_cast<const void *>(canonical.Get());
}

[[nodiscard]] int
engine_render_target_index(const void *const resource) noexcept {
  if (resource == nullptr) {
    return -1;
  }
  auto *renderer = RE::BSGraphics::Renderer::GetRendererData();
  if (renderer == nullptr) {
    return -1;
  }
  constexpr auto kTotal = static_cast<std::size_t>(RE::RENDER_TARGETS::kTOTAL);
  for (std::size_t index = 0; index < kTotal; ++index) {
    if (resource ==
        static_cast<const void *>(renderer->renderTargets[index].texture)) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

[[nodiscard]] bool scene_domain_resource(const void *const resource) noexcept {
  const auto index = engine_render_target_index(resource);
  return index >= 0 && index != RE::RENDER_TARGETS::kFRAMEBUFFER;
}

[[nodiscard]] std::string_view
resource_role(const void *const resource) noexcept {
  if (resource == nullptr) {
    return "null";
  }
  const auto &presentation = PresentationBridge::instance();
  if (resource ==
      static_cast<const void *>(presentation.d3d11_render_buffer())) {
    return "reduced-proxy-framebuffer";
  }
  if (resource == static_cast<const void *>(presentation.d3d11_back_buffer())) {
    return "native-output";
  }
  auto *renderer = RE::BSGraphics::Renderer::GetRendererData();
  if (renderer == nullptr) {
    return "unknown";
  }
  struct NamedTarget {
    RE::RENDER_TARGETS::RENDER_TARGET target;
    std::string_view name;
  };
  static constexpr std::array kNamed{
      NamedTarget{RE::RENDER_TARGETS::kFRAMEBUFFER, "skyrim-kFRAMEBUFFER"},
      NamedTarget{RE::RENDER_TARGETS::kMAIN, "skyrim-kMAIN"},
      NamedTarget{RE::RENDER_TARGETS::kMOTION_VECTOR, "skyrim-kMOTION_VECTOR"},
      NamedTarget{RE::RENDER_TARGETS::kNORMAL_TAAMASK_SSRMASK,
                  "skyrim-kNORMAL_TAAMASK_SSRMASK"}};
  for (const auto &entry : kNamed) {
    if (resource == static_cast<const void *>(
                        renderer->renderTargets[entry.target].texture)) {
      return entry.name;
    }
  }
  if (resource == static_cast<const void *>(
                      MainDepthTracker::instance().texture())) {
    return "skyrim-depth-kMAIN";
  }
  return "unknown";
}

void describe_caller(const void *const address,
                     std::array<char, MAX_PATH> &module_name,
                     std::uintptr_t &relative) noexcept {
  module_name[0] = '\0';
  relative = 0;
  if (address == nullptr) {
    return;
  }
  HMODULE owner{};
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCSTR>(address), &owner) == 0 ||
      owner == nullptr) {
    return;
  }
  std::array<char, MAX_PATH> full{};
  if (GetModuleFileNameA(owner, full.data(), static_cast<DWORD>(full.size())) ==
      0) {
    return;
  }
  const std::string_view path{full.data()};
  const auto slash = path.find_last_of("\\/");
  const auto leaf =
      slash == std::string_view::npos ? path : path.substr(slash + 1);
  const auto copied = (std::min)(leaf.size(), module_name.size() - 1U);
  std::memcpy(module_name.data(), leaf.data(), copied);
  module_name[copied] = '\0';
  relative = reinterpret_cast<std::uintptr_t>(address) -
             reinterpret_cast<std::uintptr_t>(owner);
}

[[nodiscard]] HMODULE external_overlay_module(const void *const address) noexcept {
  if (address == nullptr) {
    return nullptr;
  }
  HMODULE owner{};
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCWSTR>(address), &owner) == 0 ||
      owner == nullptr) {
    return nullptr;
  }
  static const HMODULE game = GetModuleHandleW(nullptr);
  static const HMODULE plugin = [] {
    static const int anchor{};
    HMODULE self{};
    static_cast<void>(GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&anchor), &self));
    return self;
  }();
  return owner == game || owner == plugin ? nullptr : owner;
}
}

namespace {

struct BindSurface {
  ComPtr<ID3D11Texture2D> before;
  ComPtr<ID3D11Texture2D> after;
  ComPtr<ID3D11Texture2D> staging;
  unsigned width{};
  unsigned height{};
  unsigned bytes_per_pixel{};
  DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
  bool valid{};
};

[[nodiscard]] unsigned format_bytes(const DXGI_FORMAT format) noexcept {
  switch (format) {
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_R16G16_FLOAT:
  case DXGI_FORMAT_R24G8_TYPELESS:
  case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
  case DXGI_FORMAT_D24_UNORM_S8_UINT:
    return 4U;
  default:
    return 0U;
  }
}

[[nodiscard]] bool prepare_bind_surface(ID3D11Device *const device,
                                        ID3D11Texture2D *const source,
                                        BindSurface &surface) noexcept {

  const auto invalidate = [&surface]() noexcept {
    surface = BindSurface{};
    return false;
  };
  if (device == nullptr || source == nullptr) {
    return invalidate();
  }
  D3D11_TEXTURE2D_DESC description{};
  source->GetDesc(&description);
  const auto bytes = format_bytes(description.Format);
  if (bytes == 0U || description.SampleDesc.Count != 1 ||
      description.ArraySize != 1) {
    return invalidate();
  }

  if (surface.valid && surface.width == description.Width &&
      surface.height == description.Height &&
      surface.format == description.Format &&
      surface.bytes_per_pixel == bytes && surface.before && surface.after &&
      surface.staging) {
    return true;
  }

  BindSurface replacement;
  auto copy = description;
  copy.Usage = D3D11_USAGE_DEFAULT;
  copy.BindFlags = 0;
  copy.CPUAccessFlags = 0;
  copy.MiscFlags = 0;
  copy.MipLevels = 1;
  if (FAILED(device->CreateTexture2D(&copy, nullptr,
                                     replacement.before.GetAddressOf())) ||
      FAILED(device->CreateTexture2D(&copy, nullptr,
                                     replacement.after.GetAddressOf()))) {
    return invalidate();
  }
  auto staging = copy;
  staging.Usage = D3D11_USAGE_STAGING;
  staging.BindFlags = 0;
  staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (FAILED(device->CreateTexture2D(&staging, nullptr,
                                     replacement.staging.GetAddressOf()))) {
    return invalidate();
  }
  replacement.width = description.Width;
  replacement.height = description.Height;
  replacement.bytes_per_pixel = bytes;
  replacement.format = description.Format;
  replacement.valid = true;
  surface = std::move(replacement);
  return true;
}

[[nodiscard]] bool bind_surface_usable(const BindSurface &surface) noexcept {
  return surface.valid && surface.before && surface.after && surface.staging &&
         surface.width != 0U && surface.height != 0U &&
         surface.bytes_per_pixel != 0U;
}

struct BindDifference {
  bool valid{};
  std::uint64_t changed{};
  std::uint64_t total{};
  unsigned min_x{};
  unsigned min_y{};
  unsigned max_x{};
  unsigned max_y{};
};

[[nodiscard]] BindDifference
compare_bind_surface(ID3D11DeviceContext *const context,
                     BindSurface &surface) noexcept {
  BindDifference difference{};
  if (context == nullptr || !bind_surface_usable(surface)) {
    return difference;
  }
  const auto width = static_cast<std::size_t>(surface.width);
  const auto height = static_cast<std::size_t>(surface.height);
  const auto stride = static_cast<std::size_t>(surface.bytes_per_pixel);
  constexpr auto maximum_size = (std::numeric_limits<std::size_t>::max)();
  if (width == 0 || height == 0 || stride == 0 ||
      width > maximum_size / height) {
    logger::warn(
        "Frame-tail diagnostic rejected an invalid surface byte count: "
        "{}x{}x{}",
        width, height, stride);
    return difference;
  }
  const auto pixel_count = width * height;
  if (pixel_count > maximum_size / stride) {
    logger::warn("Frame-tail diagnostic rejected an overflowing surface byte "
                 "count: {} pixels x {} bytes",
                 pixel_count, stride);
    return difference;
  }
  const auto byte_count = pixel_count * stride;

  context->CopyResource(surface.staging.Get(), surface.before.Get());
  std::vector<std::uint8_t> baseline;
  try {
    baseline.resize(byte_count);
  } catch (...) {

    logger::warn("Frame-tail diagnostic skipped a {}-byte surface comparison "
                 "because its CPU baseline could not be allocated",
                 byte_count);
    return difference;
  }
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(
          context->Map(surface.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return difference;
  }
  for (std::size_t row = 0; row < height; ++row) {
    std::memcpy(baseline.data() + (row * width * stride),
                static_cast<const std::uint8_t *>(mapped.pData) +
                    (row * static_cast<std::size_t>(mapped.RowPitch)),
                width * stride);
  }
  context->Unmap(surface.staging.Get(), 0);

  context->CopyResource(surface.staging.Get(), surface.after.Get());
  if (FAILED(
          context->Map(surface.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return difference;
  }
  auto min_x = width;
  auto min_y = height;
  std::size_t max_x{};
  std::size_t max_y{};
  std::uint64_t changed{};
  for (std::size_t row = 0; row < height; ++row) {
    const auto *current = static_cast<const std::uint8_t *>(mapped.pData) +
                          (row * static_cast<std::size_t>(mapped.RowPitch));
    const auto *previous = baseline.data() + (row * width * stride);
    for (std::size_t column = 0; column < width; ++column) {
      if (std::memcmp(current + (column * stride), previous + (column * stride),
                      stride) == 0) {
        continue;
      }
      ++changed;
      min_x = (std::min)(min_x, column);
      min_y = (std::min)(min_y, row);
      max_x = (std::max)(max_x, column);
      max_y = (std::max)(max_y, row);
    }
  }
  context->Unmap(surface.staging.Get(), 0);

  difference.valid = true;
  difference.changed = changed;
  difference.total = static_cast<std::uint64_t>(pixel_count);
  difference.min_x = min_x > max_x ? 0U : static_cast<unsigned>(min_x);
  difference.min_y = min_y > max_y ? 0U : static_cast<unsigned>(min_y);
  difference.max_x = static_cast<unsigned>(max_x);
  difference.max_y = static_cast<unsigned>(max_y);
  return difference;
}

struct FrameEpisodeTrace {

  static constexpr unsigned kBudget = 24U;

  static constexpr unsigned kArmOnFrame = 3U;

  static constexpr std::uint64_t kPixelBudget = 120ULL * 1000ULL * 1000ULL;

  BindSurface colour;
  BindSurface secondary;
  BindSurface depth;
  ComPtr<ID3D11Texture2D> colour_source;
  ComPtr<ID3D11Texture2D> secondary_source;
  ComPtr<ID3D11Texture2D> depth_source;

  BindSurface window_proxy;
  BindSurface window_output;
  ComPtr<ID3D11Texture2D> window_proxy_source;
  ComPtr<ID3D11Texture2D> window_output_source;

  ID3D11DeviceContext *opened_context{};

  const void *opened_identity{};

  std::array<char, MAX_PATH> caller_module{};
  std::uintptr_t caller_relative{};
  std::string_view colour_role{"none"};
  std::string_view secondary_role{"none"};
  std::string_view depth_role{"none"};
  std::uint64_t measured_pixels{};
  unsigned bind_index{};
  unsigned measured{};
  unsigned bound_targets{};
  unsigned colour_width{};
  unsigned colour_height{};
  unsigned colour_format{};
  unsigned secondary_width{};
  unsigned secondary_height{};
  unsigned secondary_format{};
  unsigned depth_width{};
  unsigned depth_height{};
  unsigned depth_format{};
  unsigned frame{};
  unsigned phase_begin_episode{};
  unsigned phase_end_episode{};

  enum class MarkerPosition : std::uint8_t {
    not_observed,

    before_trace,
    during_episode,
    between_episodes
  };

  MarkerPosition phase_begin_position{MarkerPosition::not_observed};
  MarkerPosition phase_end_position{MarkerPosition::not_observed};

  unsigned phase_begin_open_episode{};
  unsigned phase_end_open_episode{};
  bool caller_unordered{};

  bool caller_known{};

  bool implicit_checkpoint{};

  unsigned armed_bound_targets{};
  bool armed_depth_bound{};
  bool armed_inside_phase{};
  bool episode_in_phase{};
  bool content_skipped{};
  bool phase_seen{};
  bool phase_ended{};
  bool window_ready{};
  bool closing_at_present{};

  bool phase_ever_opened{};
  bool armed{};
  bool open{};
  bool completed{};

  void release() noexcept {
    colour = BindSurface{};
    secondary = BindSurface{};
    depth = BindSurface{};
    colour_source.Reset();
    secondary_source.Reset();
    depth_source.Reset();
    window_proxy = BindSurface{};
    window_output = BindSurface{};
    window_proxy_source.Reset();
    window_output_source.Reset();
    window_ready = false;
    opened_context = nullptr;
    opened_identity = nullptr;
    open = false;
  }
};

FrameEpisodeTrace frame_episode_trace;

}

void DynamicResolution::open_ui_phase_episode(
    ID3D11DeviceContext *const context,
    const bool implicit_checkpoint) noexcept {
  auto &trace = frame_episode_trace;
  if (!trace.armed || trace.completed || trace.open || context == nullptr) {
    return;
  }
  if (trace.measured >= FrameEpisodeTrace::kBudget) {

    return;
  }

  ComPtr<ID3D11Device> device;
  context->GetDevice(device.GetAddressOf());
  if (!device) {
    return;
  }

  constexpr auto kMaximumTargets = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
  std::array<ID3D11RenderTargetView *, kMaximumTargets> views{};
  ID3D11DepthStencilView *depth_view{};
  context->OMGetRenderTargets(static_cast<UINT>(views.size()), views.data(),
                              &depth_view);
  std::array<ComPtr<ID3D11RenderTargetView>, kMaximumTargets> owners;
  for (std::size_t index = 0; index < views.size(); ++index) {
    owners[index].Attach(views[index]);
  }
  ComPtr<ID3D11DepthStencilView> depth_owner;
  depth_owner.Attach(depth_view);

  unsigned bound = 0;
  for (auto *const view : views) {
    if (view != nullptr) {
      ++bound;
    }
  }
  if (bound == 0 && depth_view == nullptr) {
    return;
  }

  const auto adopt = [](ID3D11View *const view,
                        ComPtr<ID3D11Texture2D> &texture,
                        std::string_view &role, unsigned &width,
                        unsigned &height, unsigned &format) noexcept {
    texture.Reset();
    role = "none";
    width = 0;
    height = 0;
    format = 0;
    if (view == nullptr) {
      return;
    }
    ComPtr<ID3D11Resource> resource;
    view->GetResource(resource.GetAddressOf());
    if (!resource) {
      return;
    }
    role = resource_role(static_cast<const void *>(resource.Get()));
    if (FAILED(resource.As(&texture))) {
      texture.Reset();
      return;
    }
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    width = description.Width;
    height = description.Height;
    format = static_cast<unsigned>(description.Format);
  };

  adopt(views[0], trace.colour_source, trace.colour_role, trace.colour_width,
        trace.colour_height, trace.colour_format);
  adopt(views[1], trace.secondary_source, trace.secondary_role,
        trace.secondary_width, trace.secondary_height, trace.secondary_format);
  adopt(depth_view, trace.depth_source, trace.depth_role, trace.depth_width,
        trace.depth_height, trace.depth_format);

  trace.bound_targets = bound;
  trace.bind_index = ui_bind_index_;
  trace.episode_in_phase = full_resolution_ui_;
  trace.implicit_checkpoint = implicit_checkpoint;
  if (implicit_checkpoint) {

    trace.caller_known = false;
    trace.caller_unordered = false;
    trace.caller_module[0] = '\0';
    trace.caller_relative = 0;
  } else {
    trace.caller_known = declined_caller_address_ != nullptr;
    trace.caller_unordered = declined_caller_unordered_;
    describe_caller(declined_caller_address_, trace.caller_module,
                    trace.caller_relative);
  }

  trace.content_skipped =
      trace.measured_pixels >= FrameEpisodeTrace::kPixelBudget;
  if (trace.content_skipped) {
    trace.colour = BindSurface{};
    trace.secondary = BindSurface{};
    trace.depth = BindSurface{};
    trace.opened_context = context;
    trace.opened_identity = com_identity(context);
    trace.open = true;
    return;
  }

  static_cast<void>(prepare_bind_surface(
      device.Get(), trace.colour_source.Get(), trace.colour));
  static_cast<void>(prepare_bind_surface(
      device.Get(), trace.secondary_source.Get(), trace.secondary));
  static_cast<void>(prepare_bind_surface(device.Get(), trace.depth_source.Get(),
                                         trace.depth));

  if (bind_surface_usable(trace.colour) && trace.colour_source) {
    context->CopyResource(trace.colour.before.Get(), trace.colour_source.Get());
  }
  if (bind_surface_usable(trace.secondary) && trace.secondary_source) {
    context->CopyResource(trace.secondary.before.Get(),
                          trace.secondary_source.Get());
  }
  if (bind_surface_usable(trace.depth) && trace.depth_source) {
    context->CopyResource(trace.depth.before.Get(), trace.depth_source.Get());
  }
  trace.opened_context = context;
  trace.opened_identity = com_identity(context);
  trace.open = true;
}

void DynamicResolution::close_ui_phase_episode(
    ID3D11DeviceContext *const context) noexcept {
  auto &probe = frame_episode_trace;
  if (!probe.open || context == nullptr) {
    return;
  }

  const auto *const closing_identity = com_identity(context);
  if (probe.opened_identity == nullptr) {

    probe.opened_context = nullptr;
    probe.colour_source.Reset();
    probe.secondary_source.Reset();
    probe.depth_source.Reset();
    if (!orphan_episode_logged_) {
      orphan_episode_logged_ = true;
      logger::warn("Frame episode after bind-index {} discarded: it carried no "
                   "recorded opening context identity, so no before/after "
                   "comparison is possible; closing interface={} canonical={}",
                   probe.bind_index, static_cast<const void *>(context),
                   closing_identity);
    }
    return;
  }
  if (probe.opened_identity != closing_identity) {

    const auto *const opened_identity = probe.opened_identity;
    probe.opened_context = nullptr;
    probe.opened_identity = nullptr;
    probe.colour_source.Reset();
    probe.secondary_source.Reset();
    probe.depth_source.Reset();
    logger::warn(
        "Frame episode after bind-index {} abandoned: it opened on device "
        "context object {} and is closing on {}; these are distinct COM "
        "objects, not two interfaces on one",
        probe.bind_index, opened_identity, closing_identity);
    return;
  }
  probe.opened_context = nullptr;
  probe.opened_identity = nullptr;
  const auto at_present = probe.closing_at_present;
  probe.closing_at_present = false;
  ++probe.measured;

  ComPtr<ID3D11BlendState> blend;
  std::array<FLOAT, 4> blend_factor{};
  UINT sample_mask{};
  context->OMGetBlendState(blend.GetAddressOf(), blend_factor.data(),
                           &sample_mask);
  D3D11_BLEND_DESC blend_description{};
  auto blend_known = false;
  if (blend) {
    blend->GetDesc(&blend_description);
    blend_known = true;
  }
  ComPtr<ID3D11DepthStencilState> depth_state;
  UINT stencil_reference{};
  context->OMGetDepthStencilState(depth_state.GetAddressOf(),
                                  &stencil_reference);
  D3D11_DEPTH_STENCIL_DESC depth_description{};
  auto depth_known = false;
  if (depth_state) {
    depth_state->GetDesc(&depth_description);
    depth_known = true;
  }
  ComPtr<ID3D11PixelShader> pixel_shader;
  ComPtr<ID3D11VertexShader> vertex_shader;
  context->PSGetShader(pixel_shader.GetAddressOf(), nullptr, nullptr);
  context->VSGetShader(vertex_shader.GetAddressOf(), nullptr, nullptr);

  constexpr auto kSlots =
      D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  std::array<D3D11_VIEWPORT, kSlots> viewports{};
  auto viewport_count = static_cast<UINT>(viewports.size());
  context->RSGetViewports(&viewport_count, viewports.data());
  std::array<D3D11_RECT, kSlots> scissors{};
  auto scissor_count = static_cast<UINT>(scissors.size());
  context->RSGetScissorRects(&scissor_count, scissors.data());

  if (bind_surface_usable(probe.colour) && probe.colour_source) {
    context->CopyResource(probe.colour.after.Get(), probe.colour_source.Get());
  }
  if (bind_surface_usable(probe.secondary) && probe.secondary_source) {
    context->CopyResource(probe.secondary.after.Get(),
                          probe.secondary_source.Get());
  }
  if (bind_surface_usable(probe.depth) && probe.depth_source) {
    context->CopyResource(probe.depth.after.Get(), probe.depth_source.Get());
  }

  const auto colour = compare_bind_surface(context, probe.colour);
  const auto secondary = compare_bind_surface(context, probe.secondary);
  const auto depth_difference = compare_bind_surface(context, probe.depth);
  probe.measured_pixels +=
      colour.total + secondary.total + depth_difference.total;

  const auto percent = [](const BindDifference &difference) {
    return difference.total != 0U
               ? (static_cast<double>(difference.changed) * 100.0) /
                     static_cast<double>(difference.total)
               : 0.0;
  };

  const auto shape = [&]() -> std::string_view {
    if (!colour.valid) {
      return "unmeasured";
    }
    if (colour.changed == 0U) {
      return "no-colour-write";
    }
    const auto coverage = percent(colour);
    const auto span_x =
        colour.max_x >= colour.min_x ? (colour.max_x - colour.min_x) + 1U : 0U;
    const auto span_y =
        colour.max_y >= colour.min_y ? (colour.max_y - colour.min_y) + 1U : 0U;
    const auto spans_surface = probe.colour.width != 0U &&
                               probe.colour.height != 0U &&
                               span_x * 10U >= probe.colour.width * 9U &&
                               span_y * 10U >= probe.colour.height * 9U;
    if (coverage >= 50.0 && spans_surface) {
      return "full-frame-write";
    }
    if (coverage < 10.0 && !spans_surface) {
      return "sparse-localized-write";
    }
    return "mixed-write";
  }();

  logger::warn(
      "Frame episode {} of budget {}: origin={} phase={} closed-at={} "
      "bind-index={} bound-targets={} api={} caller={}+0x{:X}; "
      "slot0 role={} {}x{} format={}; slot1 role={} {}x{} format={}; "
      "dsv role={} {}x{} format={}; "
      "slot0 changed={} of {} ({:.4f}%) bounds={},{}..{},{} valid={}; "
      "slot1 changed={} of {} ({:.4f}%) bounds={},{}..{},{} valid={}; "
      "depth changed={} of {} ({:.4f}%) valid={}; "
      "depth-state known={} DepthEnable={} DepthWriteMask={} DepthFunc={} "
      "StencilEnable={}; blend known={} rt0-writemask={} rt1-writemask={} "
      "independent-blend={}; ps={} vs={}; viewport={}x{} scissor={},{},{},{}; "
      "content={}; shape={}",
      probe.measured, FrameEpisodeTrace::kBudget,
      probe.implicit_checkpoint ? "checkpoint-current-binding"
                                : "om-set-render-targets",
      probe.episode_in_phase
          ? "in-production-ui-phase"
          : (probe.phase_ended ? "after-phase" : "before-phase"),
      at_present ? "present" : "next-bind", probe.bind_index,
      probe.bound_targets,
      probe.implicit_checkpoint
          ? "none-bind-preceded-checkpoint"
          : (probe.caller_unordered
                 ? "OMSetRenderTargetsAndUnorderedAccessViews"
                 : "OMSetRenderTargets"),
      probe.implicit_checkpoint
          ? "unavailable-bind-preceded-checkpoint"
          : (probe.caller_module[0] != '\0'
                 ? static_cast<const char *>(probe.caller_module.data())
                 : "unknown"),
      probe.caller_relative, probe.colour_role, probe.colour_width,
      probe.colour_height, probe.colour_format, probe.secondary_role,
      probe.secondary_width, probe.secondary_height, probe.secondary_format,
      probe.depth_role, probe.depth_width, probe.depth_height,
      probe.depth_format, colour.changed, colour.total, percent(colour),
      colour.min_x, colour.min_y, colour.max_x, colour.max_y, colour.valid,
      secondary.changed, secondary.total, percent(secondary), secondary.min_x,
      secondary.min_y, secondary.max_x, secondary.max_y, secondary.valid,
      depth_difference.changed, depth_difference.total,
      percent(depth_difference), depth_difference.valid, depth_known,
      depth_known ? depth_description.DepthEnable != FALSE : false,
      depth_known ? static_cast<unsigned>(depth_description.DepthWriteMask)
                  : 0U,
      depth_known ? static_cast<unsigned>(depth_description.DepthFunc) : 0U,
      depth_known ? depth_description.StencilEnable != FALSE : false,
      blend_known,
      blend_known ? static_cast<unsigned>(
                        blend_description.RenderTarget[0].RenderTargetWriteMask)
                  : 0U,
      blend_known ? static_cast<unsigned>(
                        blend_description.RenderTarget[1].RenderTargetWriteMask)
                  : 0U,
      blend_known ? blend_description.IndependentBlendEnable != FALSE : false,
      static_cast<const void *>(pixel_shader.Get()),
      static_cast<const void *>(vertex_shader.Get()),
      static_cast<int>(viewports[0].Width),
      static_cast<int>(viewports[0].Height), scissors[0].left, scissors[0].top,
      scissors[0].right, scissors[0].bottom,
      probe.content_skipped ? "skipped-pixel-budget" : "measured", shape);

  probe.colour_source.Reset();
  probe.secondary_source.Reset();
  probe.depth_source.Reset();
}

void DynamicResolution::arm_frame_episode_trace() noexcept {

  auto &trace = frame_episode_trace;
  if (trace.completed || trace.armed || !trace.phase_ever_opened) {
    return;
  }
  if (++trace.frame != FrameEpisodeTrace::kArmOnFrame) {
    return;
  }

  auto &presentation = PresentationBridge::instance();
  auto *const context = presentation.d3d11_context();
  auto *const device = presentation.d3d11_device();
  trace.armed = true;
  trace.measured = 0;
  trace.measured_pixels = 0;
  trace.phase_ended = false;
  trace.phase_begin_episode = 0;
  trace.phase_end_episode = 0;
  trace.phase_begin_open_episode = 0;
  trace.phase_end_open_episode = 0;
  trace.phase_end_position = FrameEpisodeTrace::MarkerPosition::not_observed;

  trace.armed_inside_phase = full_resolution_ui_;
  trace.phase_seen = full_resolution_ui_;
  trace.phase_begin_position =
      full_resolution_ui_ ? FrameEpisodeTrace::MarkerPosition::before_trace
                          : FrameEpisodeTrace::MarkerPosition::not_observed;

  if (context != nullptr && device != nullptr) {
    trace.window_proxy_source = presentation.d3d11_render_buffer();
    trace.window_output_source = presentation.d3d11_back_buffer();
    const auto proxy_ready = prepare_bind_surface(
        device, trace.window_proxy_source.Get(), trace.window_proxy);
    const auto output_ready = prepare_bind_surface(
        device, trace.window_output_source.Get(), trace.window_output);
    if (proxy_ready && bind_surface_usable(trace.window_proxy) &&
        trace.window_proxy_source) {
      context->CopyResource(trace.window_proxy.before.Get(),
                            trace.window_proxy_source.Get());
    }
    if (output_ready && bind_surface_usable(trace.window_output) &&
        trace.window_output_source) {
      context->CopyResource(trace.window_output.before.Get(),
                            trace.window_output_source.Get());
    }
    trace.window_ready = true;
  }

  unsigned checkpoint_targets = 0;
  auto checkpoint_depth = false;
  if (context != nullptr) {
    constexpr auto kMaximumTargets = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
    std::array<ID3D11RenderTargetView *, kMaximumTargets> views{};
    ID3D11DepthStencilView *depth_view{};
    context->OMGetRenderTargets(static_cast<UINT>(views.size()), views.data(),
                                &depth_view);

    std::array<ComPtr<ID3D11RenderTargetView>, kMaximumTargets> owners;
    for (std::size_t index = 0; index < views.size(); ++index) {
      owners[index].Attach(views[index]);
      if (views[index] != nullptr) {
        ++checkpoint_targets;
      }
    }
    ComPtr<ID3D11DepthStencilView> depth_owner;
    depth_owner.Attach(depth_view);
    checkpoint_depth = depth_view != nullptr;
  }
  trace.armed_bound_targets = checkpoint_targets;
  trace.armed_depth_bound = checkpoint_depth;

  logger::warn(
      "Frame episode trace marker: ARMED at the MainDraw checkpoint; "
      "current binding at the checkpoint: colour-targets={} depth-bound={} "
      "bind-index={}; armed inside active production UI phase={}; "
      "budget {} episodes and {} megapixels of content; window baselines "
      "captured={}; finalized at Present",
      checkpoint_targets, checkpoint_depth, ui_bind_index_,
      trace.armed_inside_phase, FrameEpisodeTrace::kBudget,
      FrameEpisodeTrace::kPixelBudget / 1000000ULL, trace.window_ready);

  if (checkpoint_targets != 0 || checkpoint_depth) {
    open_ui_phase_episode(context, true);
  }
}

void DynamicResolution::mark_frame_episode_phase(const bool begin) noexcept {
  auto &trace = frame_episode_trace;
  if (begin) {

    trace.phase_ever_opened = true;
  }
  if (!trace.armed || trace.completed) {
    return;
  }

  const auto open_episode = trace.open ? trace.measured + 1U : 0U;
  const auto position =
      trace.open ? FrameEpisodeTrace::MarkerPosition::during_episode
                 : FrameEpisodeTrace::MarkerPosition::between_episodes;

  if (begin) {
    if (trace.phase_seen) {
      return;
    }
    trace.phase_seen = true;
    trace.phase_begin_episode = trace.measured;
    trace.phase_begin_open_episode = open_episode;
    trace.phase_begin_position = position;
    logger::warn(
        "Frame episode trace marker: production UI phase BEGIN with {} "
        "episode(s) completed; open episode={} ({})",
        trace.measured, open_episode,
        trace.open ? "marker fell inside an open episode"
                   : "marker fell between episodes");
    return;
  }
  if (!trace.phase_seen || trace.phase_ended) {
    return;
  }
  trace.phase_end_open_episode = open_episode;
  trace.phase_end_position = position;

  trace.phase_ended = true;
  trace.phase_end_episode = trace.measured;
  logger::warn(
      "Frame episode trace marker: production UI phase END with {} "
      "episode(s) completed; open episode={} ({}); the open episode is "
      "deliberately left open and continues to the next bind or to Present",
      trace.measured, open_episode,
      trace.open ? "marker fell inside an open episode"
                 : "marker fell between episodes");
}

void DynamicResolution::note_production_ui_phase_end_boundary() noexcept {

  if (!full_resolution_ui_) {
    return;
  }
  mark_frame_episode_phase(false);
}

void DynamicResolution::finalize_frame_episode_trace(
    ID3D11DeviceContext *const context) noexcept {
  auto &trace = frame_episode_trace;
  if (!trace.armed || trace.completed) {
    return;
  }
  logger::warn(
      "Frame episode trace marker: PRE-PRESENT final closure; every overlay "
      "that contributes to the visible frame has drawn by this point");

  auto *const closing =
      trace.opened_context != nullptr ? trace.opened_context : context;
  trace.closing_at_present = true;
  close_ui_phase_episode(closing);
  trace.closing_at_present = false;

  if (trace.window_ready && context != nullptr) {
    if (bind_surface_usable(trace.window_proxy) && trace.window_proxy_source) {
      context->CopyResource(trace.window_proxy.after.Get(),
                            trace.window_proxy_source.Get());
    }
    if (bind_surface_usable(trace.window_output) &&
        trace.window_output_source) {
      context->CopyResource(trace.window_output.after.Get(),
                            trace.window_output_source.Get());
    }
    const auto proxy = compare_bind_surface(context, trace.window_proxy);
    const auto output = compare_bind_surface(context, trace.window_output);
    const auto percent = [](const BindDifference &difference) {
      return difference.total != 0U
                 ? (static_cast<double>(difference.changed) * 100.0) /
                       static_cast<double>(difference.total)
                 : 0.0;
    };
    logger::warn("Frame episode trace window MainDraw..Present: "
                 "reduced-proxy changed={} of {} ({:.4f}%) bounds={},{}..{},{} "
                 "valid={}; native-output changed={} of {} ({:.4f}%) "
                 "bounds={},{}..{},{} valid={}",
                 proxy.changed, proxy.total, percent(proxy), proxy.min_x,
                 proxy.min_y, proxy.max_x, proxy.max_y, proxy.valid,
                 output.changed, output.total, percent(output), output.min_x,
                 output.min_y, output.max_x, output.max_y, output.valid);
  }

  const auto position_name =
      [](const FrameEpisodeTrace::MarkerPosition value) -> std::string_view {
    switch (value) {
    case FrameEpisodeTrace::MarkerPosition::before_trace:
      return "before-trace-armed";
    case FrameEpisodeTrace::MarkerPosition::during_episode:
      return "inside-an-open-episode";
    case FrameEpisodeTrace::MarkerPosition::between_episodes:
      return "between-episodes";
    case FrameEpisodeTrace::MarkerPosition::not_observed:
      break;
    }
    return "not-observed";
  };

  logger::warn(
      "Frame episode trace finished: {} episode(s) measured, {} megapixels "
      "compared, budget {}; armed-inside-active-production-ui-phase={} "
      "checkpoint-binding: colour-targets={} depth-bound={}; production UI "
      "phase seen={} BEGIN position={} (completed={} open-episode={}) "
      "END position={} (completed={} open-episode={})",
      trace.measured, trace.measured_pixels / 1000000ULL,
      FrameEpisodeTrace::kBudget, trace.armed_inside_phase,
      trace.armed_bound_targets, trace.armed_depth_bound, trace.phase_seen,
      position_name(trace.phase_begin_position), trace.phase_begin_episode,
      trace.phase_begin_open_episode, position_name(trace.phase_end_position),
      trace.phase_end_episode, trace.phase_end_open_episode);
  trace.armed = false;
  trace.completed = true;
  trace.release();
}

void DynamicResolution::note_ui_bind(ID3D11DeviceContext *const context,
                                     const void *const caller_address,
                                     const bool unordered_variant) noexcept {

  const auto tracing = frame_episode_trace.armed;
  if (!full_resolution_ui_ && !tracing) {
    return;
  }
  declined_probe_context_ = context;

  close_ui_phase_episode(context);
  declined_caller_address_ = caller_address;
  declined_caller_unordered_ = unordered_variant;

  if (full_resolution_ui_) {
    ++ui_bind_index_;
  }
}

void DynamicResolution::report_declined_ui_target(
    const UINT target_count, const UINT failing_index,
    ID3D11RenderTargetView *const *targets,
    ID3D11RenderTargetView *const *adjusted_targets,
    ID3D11DepthStencilView *depth) noexcept {

  constexpr std::uint32_t kMaximumReports{6};
  if (declined_ui_target_reports_ >= kMaximumReports) {
    return;
  }
  ++declined_ui_target_reports_;

  const auto describe =
      [](ID3D11RenderTargetView *const view, const void *&identity,
         unsigned &width, unsigned &height, unsigned &format,
         unsigned &array_size, unsigned &samples, unsigned &mip) {
        if (view == nullptr) {
          return;
        }
        D3D11_RENDER_TARGET_VIEW_DESC view_description{};
        view->GetDesc(&view_description);
        mip = view_description.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D
                  ? view_description.Texture2D.MipSlice
                  : 0U;
        ComPtr<ID3D11Resource> resource;
        view->GetResource(resource.GetAddressOf());
        identity = static_cast<const void *>(resource.Get());
        ComPtr<ID3D11Texture2D> texture;
        if (resource && SUCCEEDED(resource.As(&texture))) {
          D3D11_TEXTURE2D_DESC description{};
          texture->GetDesc(&description);
          width = description.Width;
          height = description.Height;
          format = static_cast<unsigned>(description.Format);
          array_size = description.ArraySize;
          samples = description.SampleDesc.Count;
        }
      };

  const void *requested_identity{};
  const void *applied_identity{};
  unsigned requested_width{};
  unsigned requested_height{};
  unsigned requested_format{};
  unsigned requested_array{};
  unsigned requested_samples{};
  unsigned requested_mip{};
  unsigned applied_width{};
  unsigned applied_height{};
  unsigned applied_format{};
  unsigned applied_array{};
  unsigned applied_samples{};
  unsigned applied_mip{};
  describe(targets[failing_index], requested_identity, requested_width,
           requested_height, requested_format, requested_array,
           requested_samples, requested_mip);
  describe(adjusted_targets[failing_index], applied_identity, applied_width,
           applied_height, applied_format, applied_array, applied_samples,
           applied_mip);

  auto remapped_slot = -1;
  for (UINT index = 0; index < target_count; ++index) {
    if (adjusted_targets[index] != targets[index]) {
      remapped_slot = static_cast<int>(index);
      break;
    }
  }

  const void *depth_identity{};
  unsigned depth_width{};
  unsigned depth_height{};
  unsigned depth_format{};
  unsigned depth_samples{};
  if (depth != nullptr) {
    ComPtr<ID3D11Resource> resource;
    depth->GetResource(resource.GetAddressOf());
    depth_identity = static_cast<const void *>(resource.Get());
    ComPtr<ID3D11Texture2D> texture;
    if (resource && SUCCEEDED(resource.As(&texture))) {
      D3D11_TEXTURE2D_DESC description{};
      texture->GetDesc(&description);
      depth_width = description.Width;
      depth_height = description.Height;
      depth_format = static_cast<unsigned>(description.Format);
      depth_samples = description.SampleDesc.Count;
    }
  }

  std::array<char, MAX_PATH> caller_module{};
  std::uintptr_t caller_relative{};
  describe_caller(declined_caller_address_, caller_module, caller_relative);

  const auto &presentation = PresentationBridge::instance();
  logger::warn(
      "Declined UI target remap {} of {}: api={}, ui-bind-index={}, "
      "target-count={}, failing-slot={}, remapped-slot={}, "
      "caller={}+0x{:X}, "
      "slot0-role={}, failing-role={}, dsv-role={}, "
      "requested-resource={} ({}x{} format={} array={} samples={} mip={}), "
      "applied-resource={} ({}x{} format={} array={} samples={} mip={}), "
      "dsv-resource={} ({}x{} format={} samples={}), required-output={}x{}",
      declined_ui_target_reports_, kMaximumReports,
      declined_caller_unordered_ ? "OMSetRenderTargetsAndUnorderedAccessViews"
                                 : "OMSetRenderTargets",
      ui_bind_index_, target_count, failing_index, remapped_slot,
      caller_module[0] != '\0' ? caller_module.data() : "unknown-module",
      caller_relative,
      resource_role(target_count > 0 ? identity_of(targets[0]) : nullptr),
      resource_role(requested_identity), resource_role(depth_identity),
      requested_identity, requested_width, requested_height, requested_format,
      requested_array, requested_samples, requested_mip, applied_identity,
      applied_width, applied_height, applied_format, applied_array,
      applied_samples, applied_mip, depth_identity, depth_width, depth_height,
      depth_format, depth_samples, presentation.output_width(),
      presentation.output_height());
}

void DynamicResolution::report_scene_domain_bind(
    const UINT target_count, const UINT scene_index,
    ID3D11RenderTargetView *const *targets) noexcept {

  constexpr std::uint32_t kMaximumReports{4};
  if (scene_domain_bind_reports_ >= kMaximumReports) {
    return;
  }
  ++scene_domain_bind_reports_;

  const auto *const scene_identity = identity_of(targets[scene_index]);
  unsigned scene_width{};
  unsigned scene_height{};
  if (targets[scene_index] != nullptr) {
    ComPtr<ID3D11Resource> resource;
    targets[scene_index]->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> texture;
    if (resource && SUCCEEDED(resource.As(&texture))) {
      D3D11_TEXTURE2D_DESC description{};
      texture->GetDesc(&description);
      scene_width = description.Width;
      scene_height = description.Height;
    }
  }

  std::array<char, MAX_PATH> caller_module{};
  std::uintptr_t caller_relative{};
  describe_caller(declined_caller_address_, caller_module, caller_relative);

  logger::info(
      "Scene-domain bind passed through {} of {}: ui-bind-index={}, "
      "target-count={}, scene-slot={}, engine-render-target-index={}, "
      "caller={}+0x{:X}, slot0-role={}, scene-slot-role={}, "
      "scene-slot-extent={}x{}; this is Skyrim's own scene work during the "
      "native UI phase, it stays on the reduced targets, and it is not a UI "
      "redirection failure",
      scene_domain_bind_reports_, kMaximumReports, ui_bind_index_, target_count,
      scene_index, engine_render_target_index(scene_identity),
      caller_module[0] != '\0' ? caller_module.data() : "unknown-module",
      caller_relative,
      resource_role(target_count > 0 ? identity_of(targets[0]) : nullptr),
      resource_role(scene_identity), scene_width, scene_height);
}

void DynamicResolution::report_external_overlay_bind(
    const HMODULE overlay, const UINT target_count,
    ID3D11RenderTargetView *const *targets) noexcept {
  for (auto *const logged : external_overlay_modules_logged_) {
    if (logged == static_cast<void *>(overlay)) {
      return;
    }
  }
  auto slot = external_overlay_modules_logged_.end();
  for (auto entry = external_overlay_modules_logged_.begin();
       entry != external_overlay_modules_logged_.end(); ++entry) {
    if (*entry == nullptr) {
      slot = entry;
      break;
    }
  }
  if (slot == external_overlay_modules_logged_.end()) {
    return;
  }
  *slot = static_cast<void *>(overlay);

  const auto *const slot0_identity =
      target_count > 0 ? identity_of(targets[0]) : nullptr;
  std::array<char, MAX_PATH> caller_module{};
  std::uintptr_t caller_relative{};
  describe_caller(declined_caller_address_, caller_module, caller_relative);

  logger::info(
      "Overlay drawing from {}+0x{:X} bound the game surface during the "
      "full-resolution UI phase outside any Scaleform display "
      "(target-count={}, slot0-role={}, engine-render-target-index={}). It is "
      "drawn straight onto the native output frame instead of the transparent "
      "UI capture layer, exactly as it would be without UFGU, because a "
      "layer composited as premultiplied alpha adds the scene back on top of "
      "anything blended into it with its own alpha rules. Before this, ENB's "
      "editor windows were captured that way and washed out over bright "
      "scenes on Skyrim 1.5.97",
      caller_module[0] != '\0' ? caller_module.data() : "unknown-module",
      caller_relative, target_count, resource_role(slot0_identity),
      engine_render_target_index(slot0_identity));
}

bool DynamicResolution::remap_full_resolution_ui_targets(
    const UINT target_count, ID3D11RenderTargetView *const *targets,
    ID3D11DepthStencilView *depth, ID3D11RenderTargetView **adjusted_targets,
    ID3D11DepthStencilView *&adjusted_depth) noexcept {
  if (!full_resolution_ui_ || ui_target_redirection_failed_ ||
      targets == nullptr || adjusted_targets == nullptr || target_count == 0) {
    return false;
  }

  auto &resources = SharedResources::instance();
  auto *const capture_target =
      resources.ui_rendering() ? resources.ui_draw_target_view() : nullptr;
  if (capture_target == nullptr) {
    return false;
  }

  auto &presentation = PresentationBridge::instance();
  auto redirected = false;
  for (UINT index = 0; index < target_count; ++index) {

    const auto ui_source =
        presentation.full_resolution_ui_source(targets[index]);
    auto *const native = presentation.full_resolution_ui_target(targets[index]);
    adjusted_targets[index] = ui_source ? capture_target : native;
    redirected |= ui_source;
  }
  if (!redirected) {
    return false;
  }

  for (UINT index = 0; index < target_count; ++index) {
    if (!scene_domain_resource(identity_of(targets[index]))) {
      continue;
    }
    report_scene_domain_bind(target_count, index, targets);
    return false;
  }

  if (!ScaleformBoundary::instance().inside_display()) {
    if (const auto overlay =
            external_overlay_module(declined_caller_address_);
        overlay != nullptr) {
      for (UINT index = 0; index < target_count; ++index) {
        adjusted_targets[index] =
            presentation.full_resolution_ui_target(targets[index]);
      }
      report_external_overlay_bind(overlay, target_count, targets);
    } else {
      for (UINT index = 0; index < target_count; ++index) {
        if (engine_render_target_index(identity_of(targets[index])) !=
            static_cast<int>(RE::RENDER_TARGETS::kFRAMEBUFFER)) {
          continue;
        }
        if (!framebuffer_outside_display_logged_) {
          framebuffer_outside_display_logged_ = true;
          logger::warn(
              "A kFRAMEBUFFER bind arrived during the full-resolution UI "
              "phase but OUTSIDE any Scaleform display, so it is Skyrim's own "
              "scene work rather than UI. It is being passed through to the "
              "native target instead of being redirected into the UI capture "
              "layer. Screen blood splatter is drawn exactly this way, which "
              "is why frame generation died for as long as blood was on "
              "screen and recovered the moment it faded, confirmed on "
              "hardware by veloha on 2026-08-29. scene_domain_resource "
              "deliberately excludes kFRAMEBUFFER, so nothing else caught "
              "this");
        }
        return false;
      }
    }
  }

  for (UINT index = 0; index < target_count; ++index) {
    if (presentation.full_resolution_ui_target_compatible(
            adjusted_targets[index])) {
      continue;
    }

    ui_target_declined_this_frame_ = true;
    report_declined_ui_target(target_count, index, targets, adjusted_targets,
                              depth);
    return false;
  }

  adjusted_depth = depth != nullptr
                       ? resources.ui_depth_view()
                       : static_cast<ID3D11DepthStencilView *>(nullptr);
  if (depth != nullptr && adjusted_depth == nullptr) {
    ui_target_redirection_failed_ = true;
    if (!ui_target_failure_logged_) {
      ui_target_failure_logged_ = true;
      logger::warn("Full-resolution UI target remap could not provide a "
                   "matching depth/stencil view; the complete reduced frame "
                   "will be presented spatially");
    }
    return false;
  }
  return true;
}

void STDMETHODCALLTYPE DynamicResolution::om_set_render_targets_thunk(
    ID3D11DeviceContext *context, const UINT target_count,
    ID3D11RenderTargetView *const *targets, ID3D11DepthStencilView *depth) {
  MainDepthTracker::instance().observe_binding(target_count, targets, depth);
  instance().note_ui_bind(context, _ReturnAddress(), false);
  RenderDebug::instance().scaleform_trace_end_target_episode(context);
  constexpr auto kMaximumTargets = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
  std::array<ID3D11RenderTargetView *, kMaximumTargets> adjusted{};
  auto *applied_targets = targets;
  auto *applied_depth = depth;
  auto &resolution = instance();
  auto redirected = false;
  if (target_count == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL &&
      resolution.full_resolution_ui_) {
    std::array<ID3D11RenderTargetView *, kMaximumTargets> current{};
    ID3D11DepthStencilView *current_depth{};
    context->OMGetRenderTargets(static_cast<UINT>(current.size()),
                                current.data(), &current_depth);
    std::array<ComPtr<ID3D11RenderTargetView>, kMaximumTargets> current_owners;
    for (std::size_t index = 0; index < current.size(); ++index) {
      current_owners[index].Attach(current[index]);
    }
    ComPtr<ID3D11DepthStencilView> current_depth_owner;
    current_depth_owner.Attach(current_depth);

    auto *remapped_depth = current_depth;
    redirected = resolution.remap_full_resolution_ui_targets(
        static_cast<UINT>(current.size()), current.data(), current_depth,
        adjusted.data(), remapped_depth);
    if (redirected) {
      original_om_set_render_targets_(context,
                                      static_cast<UINT>(adjusted.size()),
                                      adjusted.data(), remapped_depth);
    }
  } else if (target_count <= adjusted.size() &&
             resolution.remap_full_resolution_ui_targets(target_count, targets,
                                                         depth, adjusted.data(),
                                                         applied_depth)) {
    redirected = true;
    applied_targets = adjusted.data();
  }

  if (auto &render_debug = RenderDebug::instance();
      render_debug.scaleform_trace_active() && targets != nullptr &&
      target_count <= adjusted.size()) {
    for (UINT index = 0; index < target_count; ++index) {
      render_debug.scaleform_trace_observe_target(
          context, targets[index], applied_targets[index], false);
    }
  }
  original_om_set_render_targets_(context, target_count, applied_targets,
                                  applied_depth);
  if (redirected && !resolution.ui_target_redirection_logged_) {
    resolution.ui_target_redirection_logged_ = true;
    logger::info("Redirected reduced proxy framebuffer binding to the native "
                 "output target during the full-resolution UI phase");
  }
  resolution.enforce_virtual_render_extent(context);
  resolution.observe_current_viewports(context);
  resolution.enforce_full_resolution_extent(context);

  if (target_count > 0 && target_count <= adjusted.size() &&
      applied_targets != nullptr) {
    RenderDebug::instance().scaleform_trace_begin_target_episode(
        context, applied_targets[0]);
  }

  resolution.open_ui_phase_episode(context);
}

void STDMETHODCALLTYPE
DynamicResolution::om_set_render_targets_and_unordered_access_views_thunk(
    ID3D11DeviceContext *context, const UINT target_count,
    ID3D11RenderTargetView *const *targets, ID3D11DepthStencilView *depth,
    const UINT unordered_start_slot, const UINT unordered_count,
    ID3D11UnorderedAccessView *const *unordered_views,
    const UINT *initial_counts) {
  MainDepthTracker::instance().observe_binding(target_count, targets, depth);
  instance().note_ui_bind(context, _ReturnAddress(), true);
  RenderDebug::instance().scaleform_trace_end_target_episode(context);
  constexpr auto kMaximumTargets = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
  std::array<ID3D11RenderTargetView *, kMaximumTargets> adjusted{};
  auto *applied_targets = targets;
  auto *applied_depth = depth;
  auto &resolution = instance();
  const auto redirected =
      target_count <= adjusted.size() &&
      resolution.remap_full_resolution_ui_targets(
          target_count, targets, depth, adjusted.data(), applied_depth);
  if (redirected) {
    applied_targets = adjusted.data();
  }
  if (auto &render_debug = RenderDebug::instance();
      render_debug.scaleform_trace_active() && targets != nullptr &&
      target_count <= adjusted.size()) {
    for (UINT index = 0; index < target_count; ++index) {
      render_debug.scaleform_trace_observe_target(context, targets[index],
                                                  applied_targets[index], true);
    }
  }
  original_om_set_render_targets_and_unordered_access_views_(
      context, target_count, applied_targets, applied_depth,
      unordered_start_slot, unordered_count, unordered_views, initial_counts);
  if (redirected && !resolution.ui_target_redirection_logged_) {
    resolution.ui_target_redirection_logged_ = true;
    logger::info("Redirected reduced proxy framebuffer binding to the native "
                 "output target during the full-resolution UI phase");
  }
  resolution.enforce_virtual_render_extent(context);
  resolution.observe_current_viewports(context);
  resolution.enforce_full_resolution_extent(context);

  if (target_count > 0 && target_count <= adjusted.size() &&
      applied_targets != nullptr) {
    RenderDebug::instance().scaleform_trace_begin_target_episode(
        context, applied_targets[0]);
  }
  resolution.open_ui_phase_episode(context);
}

void STDMETHODCALLTYPE DynamicResolution::clear_depth_stencil_view_thunk(
    ID3D11DeviceContext *context, ID3D11DepthStencilView *depth,
    const UINT clear_flags, const FLOAT depth_value,
    const UINT8 stencil_value) {
  original_clear_depth_stencil_view_(context, depth, clear_flags, depth_value,
                                     stencil_value);
  if (!instance().full_resolution_ui_ || depth == nullptr) {
    return;
  }

  auto *mapped =
      PresentationBridge::instance().mapped_full_resolution_ui_clear_depth(
          depth);
  if (mapped != nullptr && mapped != depth) {
    original_clear_depth_stencil_view_(context, mapped, clear_flags,
                                       depth_value, stencil_value);
  }
}

void DynamicResolution::observe_current_viewports(
    ID3D11DeviceContext *context) noexcept {
  if (!should_evaluate_ || expected_viewport_seen_ || context == nullptr) {
    return;
  }

  D3D11_VIEWPORT
      viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
  UINT viewport_count =
      D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  context->RSGetViewports(&viewport_count, viewports);
  observe_viewports(context, viewport_count, viewports);
}

void DynamicResolution::arm_extent_observation(const bool armed) noexcept {

  extent_violation_observed_.store(false, std::memory_order_relaxed);
  extent_violation_reason_ = nullptr;
  extent_violation_left_ = 0;
  extent_violation_top_ = 0;
  extent_violation_right_ = 0;
  extent_violation_bottom_ = 0;
  extent_observation_armed_.store(armed, std::memory_order_relaxed);
}

bool DynamicResolution::extent_observation_armed() const noexcept {
  return extent_observation_armed_.load(std::memory_order_relaxed);
}

bool DynamicResolution::extent_violation_observed() const noexcept {
  return extent_violation_observed_.load(std::memory_order_relaxed);
}

const char *DynamicResolution::extent_violation_reason() const noexcept {
  return extent_violation_reason_;
}

bool DynamicResolution::extent_violation_rectangle(
    std::int32_t &left, std::int32_t &top, std::int32_t &right,
    std::int32_t &bottom) const noexcept {
  if (!extent_violation_observed_.load(std::memory_order_relaxed)) {
    return false;
  }
  left = extent_violation_left_;
  top = extent_violation_top_;
  right = extent_violation_right_;
  bottom = extent_violation_bottom_;
  return true;
}

void DynamicResolution::note_extent_violation(
    const char *const reason, const std::int32_t left, const std::int32_t top,
    const std::int32_t right, const std::int32_t bottom) noexcept {

  if (!extent_observation_armed_.load(std::memory_order_relaxed)) {
    return;
  }

  if (extent_violation_observed_.load(std::memory_order_relaxed)) {
    return;
  }
  extent_violation_reason_ = reason;
  extent_violation_left_ = left;
  extent_violation_top_ = top;
  extent_violation_right_ = right;
  extent_violation_bottom_ = bottom;
  extent_violation_observed_.store(true, std::memory_order_relaxed);
}

void DynamicResolution::enforce_virtual_render_extent(
    ID3D11DeviceContext *context) noexcept {
  const auto &presentation = PresentationBridge::instance();

  if (presentation.uses_sub_rect_render_surface()) {
    return;
  }
  if (!presentation.uses_virtual_render_surface() || context == nullptr ||
      original_rs_set_viewports_ == nullptr ||
      original_rs_set_scissor_rects_ == nullptr) {
    return;
  }

  const auto extent = bound_render_target_extent(context);
  const auto render_width = presentation.render_width();
  const auto render_height = presentation.render_height();
  if (extent.width != render_width || extent.height != render_height ||
      render_width == 0 || render_height == 0) {
    return;
  }

  constexpr auto kMaximumCount =
      D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  std::array<D3D11_VIEWPORT, kMaximumCount> viewports{};
  UINT viewport_count = static_cast<UINT>(viewports.size());
  context->RSGetViewports(&viewport_count, viewports.data());
  auto viewport_corrected = false;
  float first_viewport_width{};
  float first_viewport_height{};
  for (UINT index = 0; index < viewport_count; ++index) {
    auto &viewport = viewports[index];
    const auto maximum_width =
        (std::max)(0.0F, static_cast<float>(render_width) - viewport.TopLeftX);
    const auto maximum_height =
        (std::max)(0.0F, static_cast<float>(render_height) - viewport.TopLeftY);
    if (viewport.Width > maximum_width + kViewportMatchTolerance ||
        viewport.Height > maximum_height + kViewportMatchTolerance) {
      if (!viewport_corrected) {
        first_viewport_width = viewport.Width;
        first_viewport_height = viewport.Height;

        note_extent_violation(
            "a viewport larger than the reduced render surface was "
            "set and corrected",
            static_cast<std::int32_t>(viewport.TopLeftX),
            static_cast<std::int32_t>(viewport.TopLeftY),
            static_cast<std::int32_t>(viewport.TopLeftX + viewport.Width),
            static_cast<std::int32_t>(viewport.TopLeftY + viewport.Height));
      }
      viewport.Width = (std::min)(viewport.Width, maximum_width);
      viewport.Height = (std::min)(viewport.Height, maximum_height);
      viewport_corrected = true;
    }
  }
  if (viewport_corrected) {
    original_rs_set_viewports_(context, viewport_count, viewports.data());
    if (!virtual_viewport_correction_logged_) {
      virtual_viewport_correction_logged_ = true;
      logger::info("Corrected virtual-render viewport {:.0f}x{:.0f} to fit "
                   "{}x{} target",
                   first_viewport_width, first_viewport_height, render_width,
                   render_height);
    }
  }

  std::array<D3D11_RECT, kMaximumCount> rectangles{};
  UINT rectangle_count = static_cast<UINT>(rectangles.size());
  context->RSGetScissorRects(&rectangle_count, rectangles.data());
  auto scissor_corrected = false;
  LONG first_scissor_width{};
  LONG first_scissor_height{};
  for (UINT index = 0; index < rectangle_count; ++index) {
    auto &rectangle = rectangles[index];
    if (rectangle.right > static_cast<LONG>(render_width) ||
        rectangle.bottom > static_cast<LONG>(render_height)) {
      if (!scissor_corrected) {
        first_scissor_width = rectangle.right - rectangle.left;
        first_scissor_height = rectangle.bottom - rectangle.top;
        note_extent_violation(
            "a scissor rectangle larger than the reduced render "
            "surface was set and corrected",
            static_cast<std::int32_t>(rectangle.left),
            static_cast<std::int32_t>(rectangle.top),
            static_cast<std::int32_t>(rectangle.right),
            static_cast<std::int32_t>(rectangle.bottom));
      }
      rectangle.right =
          (std::min)(rectangle.right, static_cast<LONG>(render_width));
      rectangle.bottom =
          (std::min)(rectangle.bottom, static_cast<LONG>(render_height));
      scissor_corrected = true;
    }
  }
  if (scissor_corrected) {
    original_rs_set_scissor_rects_(context, rectangle_count, rectangles.data());
    if (!virtual_scissor_correction_logged_) {
      virtual_scissor_correction_logged_ = true;
      logger::info("Corrected virtual-render scissor {}x{} to fit {}x{} "
                   "target",
                   first_scissor_width, first_scissor_height, render_width,
                   render_height);
    }
  }
}

bool DynamicResolution::full_resolution_target_bound(
    ID3D11DeviceContext *context, std::uint32_t *width,
    std::uint32_t *height) const noexcept {
  const auto extent = bound_render_target_extent(context);
  if (width != nullptr) {
    *width = extent.width;
  }
  if (height != nullptr) {
    *height = extent.height;
  }
  const auto &super_resolution = streamline::SuperResolution::instance();
  return extent.width != 0 && extent.height != 0 &&
         extent.width == super_resolution.output_width() &&
         extent.height == super_resolution.output_height();
}

void DynamicResolution::enforce_full_resolution_extent(
    ID3D11DeviceContext *context) noexcept {
  if ((!full_resolution_post_processing_ && !full_resolution_ui_) ||
      context == nullptr || original_rs_set_viewports_ == nullptr ||
      original_rs_set_scissor_rects_ == nullptr ||
      !full_resolution_target_bound(context)) {
    return;
  }

  const auto &super_resolution = streamline::SuperResolution::instance();
  constexpr auto kMaximumCount =
      D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

  std::array<D3D11_VIEWPORT, kMaximumCount> viewports{};
  UINT viewport_count = static_cast<UINT>(viewports.size());
  context->RSGetViewports(&viewport_count, viewports.data());
  auto viewport_promoted = false;
  for (UINT index = 0; index < viewport_count; ++index) {
    if (matches_viewport_extent(viewports[index],
                                super_resolution.render_width(),
                                super_resolution.render_height())) {
      viewports[index].TopLeftX = 0.0F;
      viewports[index].TopLeftY = 0.0F;
      viewports[index].Width =
          static_cast<float>(super_resolution.output_width());
      viewports[index].Height =
          static_cast<float>(super_resolution.output_height());
      viewport_promoted = true;
    }
  }
  if (viewport_promoted) {
    original_rs_set_viewports_(context, viewport_count, viewports.data());
  }

  std::array<D3D11_RECT, kMaximumCount> rectangles{};
  UINT rectangle_count = static_cast<UINT>(rectangles.size());
  context->RSGetScissorRects(&rectangle_count, rectangles.data());
  auto scissor_promoted = false;
  for (UINT index = 0; index < rectangle_count; ++index) {
    if (matches_scissor_extent(rectangles[index],
                               super_resolution.render_width(),
                               super_resolution.render_height())) {
      rectangles[index].left = 0;
      rectangles[index].top = 0;
      rectangles[index].right =
          static_cast<LONG>(super_resolution.output_width());
      rectangles[index].bottom =
          static_cast<LONG>(super_resolution.output_height());
      scissor_promoted = true;
    }
  }
  if (scissor_promoted) {
    original_rs_set_scissor_rects_(context, rectangle_count, rectangles.data());
  }

  if (viewport_promoted || scissor_promoted) {
    if (full_resolution_ui_) {
      if (!ui_resolution_promotion_logged_) {
        ui_resolution_promotion_logged_ = true;
        logger::info("Promoted Skyrim UI {}{} from the DLSS render extent "
                     "to the native output extent",
                     viewport_promoted ? "viewport" : "",
                     scissor_promoted
                         ? (viewport_promoted ? " and scissor" : "scissor")
                         : "");
      }
    } else {
      log_post_processing_promotion(viewport_promoted, scissor_promoted);
    }
  }
}

void DynamicResolution::log_post_processing_promotion(
    const bool viewport_promoted, const bool scissor_promoted) noexcept {
  if (post_processing_promotion_logged_) {
    return;
  }
  post_processing_promotion_logged_ = true;
  const auto &super_resolution = streamline::SuperResolution::instance();
  logger::info(
      "Promoted post-processing {}{} extent from {}x{} to {}x{} while a "
      "full-resolution target was bound",
      viewport_promoted ? "viewport" : "",
      scissor_promoted ? (viewport_promoted ? " and scissor" : "scissor") : "",
      super_resolution.render_width(), super_resolution.render_height(),
      super_resolution.output_width(), super_resolution.output_height());
}

void DynamicResolution::update_resolution(
    RE::BSGraphics::State *state) noexcept {
  if (state == nullptr) {
    return;
  }

  const auto &super_resolution = streamline::SuperResolution::instance();
  const auto evaluation_ready =
      super_resolution.enabled() &&
      !UpscalingPass::instance().evaluation_failed() && !menu_override_ &&
      CameraData::instance().temporal_inputs_valid() &&
      (RE::UI::GetSingleton() == nullptr ||
       !RE::UI::GetSingleton()->GameIsPaused());
  const auto surface = surface_model_state();
  if (surface.complete_frame) {

    const auto &presentation = PresentationBridge::instance();
    state->frameBufferViewport[0] = presentation.render_width();
    state->frameBufferViewport[1] = presentation.render_height();
  }

  const auto scale = evaluation_ready && !surface.complete_frame
                         ? super_resolution.render_scale()
                         : 1.0F;
  const auto reduced_resolution = evaluation_ready && scale < 0.999F;

  update_dynamic_resolution_mode(super_resolution.enabled() &&
                                     !surface.complete_frame,
                                 reduced_resolution);
  scene_scale_ = scale;
  state_value<float>(state, kDynamicWidthScaleOffset) = scale;
  state_value<float>(state, kDynamicHeightScaleOffset) = scale;
  state_value<float>(state, kDynamicPreviousWidthScaleOffset) =
      last_scene_scale_;
  state_value<float>(state, kDynamicPreviousHeightScaleOffset) =
      last_scene_scale_;
  state_value<std::int32_t>(state, kDynamicResolutionLockOffset) =
      dynamic_resolution_lock_for(surface, reduced_resolution,
                                  super_resolution.enabled(),
                                  original_dynamic_resolution_lock_);
}

void DynamicResolution::update_dynamic_resolution_mode(
    const bool dlss_enabled, const bool reduced_resolution) noexcept {
  if (dynamic_resolution_setting_ == nullptr ||
      dynamic_resolution_clamp_setting_ == nullptr) {
    return;
  }

  const auto enabled =
      dlss_enabled ? true : original_dynamic_resolution_enabled_;
  const auto clamp = dlss_enabled ? 0.0F : original_dynamic_resolution_clamp_;
  dynamic_resolution_setting_->data.b = enabled;
  dynamic_resolution_clamp_setting_->data.f = clamp;

  if (!dynamic_resolution_mode_initialized_ ||
      dynamic_resolution_active_ != reduced_resolution) {
    dynamic_resolution_mode_initialized_ = true;
    dynamic_resolution_active_ = reduced_resolution;
    if (reduced_resolution) {
      logger::info("Skyrim dynamic-resolution viewport enabled with zero clamp "
                   "for reduced-resolution DLSS");
    } else if (dlss_enabled) {
      logger::info("Skyrim dynamic-resolution lifecycle retained at a 1.0 "
                   "scene ratio");
    } else {
      logger::info("Skyrim dynamic-resolution settings restored for DLSS Off");
    }
  }
}

void DynamicResolution::reapply_scene_scale(
    RE::BSGraphics::State *state) noexcept {
  if (state == nullptr) {
    return;
  }
  state_value<float>(state, kDynamicWidthScaleOffset) = scene_scale_;
  state_value<float>(state, kDynamicHeightScaleOffset) = scene_scale_;
  state_value<float>(state, kDynamicPreviousWidthScaleOffset) =
      last_scene_scale_;
  state_value<float>(state, kDynamicPreviousHeightScaleOffset) =
      last_scene_scale_;
  state_value<std::int32_t>(state, kDynamicResolutionLockOffset) =
      dynamic_resolution_lock_for(
          surface_model_state(), scene_scale_ < 0.999F,
          streamline::SuperResolution::instance().enabled(),
          original_dynamic_resolution_lock_);
}

void DynamicResolution::observe_viewports(
    ID3D11DeviceContext *context, const UINT viewport_count,
    const D3D11_VIEWPORT *viewports) noexcept {
  if (!should_evaluate_ || expected_viewport_seen_ || viewports == nullptr ||
      expected_viewport_width_ == 0 || expected_viewport_height_ == 0) {
    return;
  }

  if (context == nullptr) {
    return;
  }
  ++observed_viewport_samples_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
  context->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
  if (target == nullptr) {
    return;
  }
  Microsoft::WRL::ComPtr<ID3D11Resource> target_resource;
  target->GetResource(target_resource.GetAddressOf());
  const auto *renderer = RE::BSGraphics::Renderer::GetRendererData();
  if (renderer == nullptr) {
    return;
  }
  const auto &main = renderer->renderTargets[RE::RENDER_TARGETS::kMAIN];
  if (main.texture == nullptr ||
      target_resource.Get() != static_cast<ID3D11Resource *>(main.texture)) {
    return;
  }
  ++observed_main_bound_samples_;

  for (UINT index = 0; index < viewport_count; ++index) {
    const auto width =
        static_cast<std::uint32_t>(std::lround(viewports[index].Width));
    const auto height =
        static_cast<std::uint32_t>(std::lround(viewports[index].Height));
    const auto width_difference =
        std::abs(static_cast<long long>(width) -
                 static_cast<long long>(expected_viewport_width_));
    const auto height_difference =
        std::abs(static_cast<long long>(height) -
                 static_cast<long long>(expected_viewport_height_));
    if (width_difference <= 2 && height_difference <= 2 &&
        std::abs(viewports[index].TopLeftX) <= 0.5F &&
        std::abs(viewports[index].TopLeftY) <= 0.5F) {
      expected_viewport_seen_ = true;
      return;
    }

    const auto distance = static_cast<long long>(width_difference) +
                          static_cast<long long>(height_difference);
    if (closest_viewport_distance_ < 0 ||
        distance < closest_viewport_distance_) {
      closest_viewport_distance_ = distance;
      closest_viewport_width_ = width;
      closest_viewport_height_ = height;
      closest_viewport_left_ = viewports[index].TopLeftX;
      closest_viewport_top_ = viewports[index].TopLeftY;
    }
  }
}

void DynamicResolution::update_jitter(RE::BSGraphics::State *state) noexcept {
  const auto &super_resolution = streamline::SuperResolution::instance();
  if (state == nullptr) {
    return;
  }

  original_projection_scale_x_ =
      state_value<float>(state, kProjectionScaleXOffset);
  original_projection_scale_y_ =
      state_value<float>(state, kProjectionScaleYOffset);
  projection_scale_trace_.jitter_entry_x = original_projection_scale_x_;
  projection_scale_trace_.jitter_entry_y = original_projection_scale_y_;

  jitter_ownership_.upscaler_active = super_resolution.enabled();
  jitter_ownership_.offset_requested = jitter_x_ != 0.0F || jitter_y_ != 0.0F;
  jitter_ownership_.engine_applied_jitter =
      CameraData::instance().engine_applied_jitter();
  if (jitter_ownership_.engine_applied_jitter) {

    jitter_ownership_.frames_since_split_enabled = 0U;
  } else if (jitter_ownership_.upscaler_active &&
             jitter_ownership_.offset_requested && !jitter_fold_patched_ &&
             !jitter_ownership_.split_abandoned) {

    ++jitter_ownership_.frames_since_split_enabled;
    if (split_probe_exhausted(jitter_ownership_)) {
      jitter_ownership_.split_abandoned = true;
    }
  }

  update_vanilla_taa(super_resolution.enabled());

  const auto evaluation_ready =
      super_resolution.enabled() &&
      !UpscalingPass::instance().evaluation_failed() && !menu_override_ &&
      CameraData::instance().temporal_inputs_valid() &&
      (RE::UI::GetSingleton() == nullptr ||
       !RE::UI::GetSingleton()->GameIsPaused());
  const auto surface = surface_model_state();
  if (surface.complete_frame) {

    const auto &presentation = PresentationBridge::instance();
    state->frameBufferViewport[0] = presentation.render_width();
    state->frameBufferViewport[1] = presentation.render_height();
  }

  const auto scale = evaluation_ready && !surface.complete_frame
                         ? super_resolution.render_scale()
                         : 1.0F;
  const auto reduced_resolution = evaluation_ready && scale < 0.999F;

  update_dynamic_resolution_mode(super_resolution.enabled() &&
                                     !surface.complete_frame,
                                 reduced_resolution);
  state_value<float>(state, kDynamicPreviousWidthScaleOffset) =
      last_scene_scale_;
  state_value<float>(state, kDynamicPreviousHeightScaleOffset) =
      last_scene_scale_;
  scene_scale_ = scale;
  last_scene_scale_ = scale;
  const auto frame = state_value<std::uint32_t>(state, kFrameCountOffset);
  if (frame != active_frame_) {

    if (should_evaluate_ && !expected_viewport_seen_ &&
        viewport_diagnostic_budget_ != 0) {
      --viewport_diagnostic_budget_;
      if (observed_main_bound_samples_ == 0) {
        logger::warn(
            "Reduced-scene viewport not verified: expected {}x{} at the "
            "origin, but kMAIN was never the bound render target across "
            "{} observed viewport changes this frame. Frame generation "
            "is refused while this is the case.",
            expected_viewport_width_, expected_viewport_height_,
            observed_viewport_samples_);
      } else if (closest_viewport_distance_ < 0) {
        logger::warn(
            "Reduced-scene viewport not verified: expected {}x{} at the "
            "origin. kMAIN was bound on {} of {} observed viewport "
            "changes but no viewport was reported with it.",
            expected_viewport_width_, expected_viewport_height_,
            observed_main_bound_samples_, observed_viewport_samples_);
      } else {
        logger::warn(
            "Reduced-scene viewport not verified: expected {}x{} at the "
            "origin, nearest match while kMAIN was bound was {}x{} at "
            "({:.1f}, {:.1f}), off by {} pixels total. kMAIN bound on {} "
            "of {} observed viewport changes.",
            expected_viewport_width_, expected_viewport_height_,
            closest_viewport_width_, closest_viewport_height_,
            closest_viewport_left_, closest_viewport_top_,
            closest_viewport_distance_, observed_main_bound_samples_,
            observed_viewport_samples_);
      }
    }
    observed_viewport_samples_ = 0;
    observed_main_bound_samples_ = 0;
    closest_viewport_distance_ = -1;
    active_frame_ = frame;
    expected_viewport_seen_ = false;
  }

  expected_viewport_width_ = evaluation_ready || surface.complete_frame
                                 ? super_resolution.render_width()
                                 : super_resolution.output_width();
  expected_viewport_height_ = evaluation_ready || surface.complete_frame
                                  ? super_resolution.render_height()
                                  : super_resolution.output_height();
  state_value<float>(state, kDynamicWidthScaleOffset) = scale;
  state_value<float>(state, kDynamicHeightScaleOffset) = scale;
  state_value<std::int32_t>(state, kDynamicResolutionLockOffset) =
      dynamic_resolution_lock_for(surface, reduced_resolution,
                                  super_resolution.enabled(),
                                  original_dynamic_resolution_lock_);
  should_evaluate_ = evaluation_ready;

  if (!evaluation_ready) {
    jitter_x_ = 0.0F;
    jitter_y_ = 0.0F;
    return;
  }

  if (config::Settings::instance().temporal_jitter() ==
      config::TemporalJitter::disabled) {
    jitter_index_ = 0;
    jitter_x_ = 0.0F;
    jitter_y_ = 0.0F;
    state_value<float>(state, kProjectionScaleXOffset) = 0.0F;
    state_value<float>(state, kProjectionScaleYOffset) = 0.0F;
  } else {
    const auto ratio = static_cast<float>(super_resolution.output_width()) /
                       static_cast<float>(super_resolution.render_width());
    const auto phase_count =
        (std::max)(1U, static_cast<std::uint32_t>(
                           std::round(8.0F * ratio * ratio)));
    const auto sample = frame % phase_count + 1;
    jitter_index_ = sample;
    jitter_x_ = halton(sample, 2) - 0.5F;
    jitter_y_ = halton(sample, 3) - 0.5F;

    {
      static std::uint64_t jitter_reports = 0ULL;
      static float widest_jitter = 0.0F;
      const auto magnitude =
          std::sqrt(jitter_x_ * jitter_x_ + jitter_y_ * jitter_y_);
      widest_jitter = magnitude > widest_jitter ? magnitude : widest_jitter;
      if (++jitter_reports % 1800ULL == 0ULL) {
        logger::info(
            "Temporal jitter census over {} evaluated frames: current offset "
            "({:.4f}, {:.4f}) px at Halton sample {} of {}, widest magnitude "
            "{:.4f} px this session. MEASURED DURING GAMEPLAY, unlike the "
            "startup jitter contract line which fires one millisecond before "
            "the first gameplay frame and is not evidence. A widest magnitude "
            "at or near zero means DLAA is reconstructing from an unjittered "
            "raster, which would smear every generated frame at every "
            "multiplier. Anything near 0.5 px is a healthy Halton spread",
            jitter_reports,
            jitter_x_,
            jitter_y_,
            sample,
            phase_count,
            widest_jitter);
      }
    }

    state_value<float>(state, kProjectionScaleXOffset) =
        -2.0F * jitter_x_ / static_cast<float>(super_resolution.render_width());
    state_value<float>(state, kProjectionScaleYOffset) =
        2.0F * jitter_y_ / static_cast<float>(super_resolution.render_height());
  }

  projection_scale_trace_.jitter_exit_x =
      state_value<float>(state, kProjectionScaleXOffset);
  projection_scale_trace_.jitter_exit_y =
      state_value<float>(state, kProjectionScaleYOffset);

  const auto trace_state =
      (projection_scale_trace_.jitter_entry_x != 0.0F ? 0x1U : 0U) |
      (projection_scale_trace_.jitter_exit_x != 0.0F ? 0x2U : 0U) |
      (projection_scale_trace_.resolution_entry_x != 0.0F ? 0x4U : 0U) |
      (projection_scale_trace_.resolution_exit_x != 0.0F ? 0x8U : 0U) |
      (projection_scale_trace_.resolution_seen ? 0x10U : 0U);
  if (projection_scale_trace_budget_.admit(trace_state)) {
    logger::info(
        "projectionPosScale trace (offset 0x44/0x48, confirmed against "
        "CommonLibSSE-NG RE::BSGraphics::State): "
        "resolution-hook entry={:.8f},{:.8f} exit={:.8f},{:.8f} ran={}; "
        "jitter-hook entry={:.8f},{:.8f} exit={:.8f},{:.8f}. "
        "entry values are what the engine left in the field, exit values "
        "are what this plugin left. A jitter-exit that is non-zero while "
        "the following resolution-entry is zero means the field is being "
        "overwritten before the draws.",
        projection_scale_trace_.resolution_entry_x,
        projection_scale_trace_.resolution_entry_y,
        projection_scale_trace_.resolution_exit_x,
        projection_scale_trace_.resolution_exit_y,
        projection_scale_trace_.resolution_seen,
        projection_scale_trace_.jitter_entry_x,
        projection_scale_trace_.jitter_entry_y,
        projection_scale_trace_.jitter_exit_x,
        projection_scale_trace_.jitter_exit_y);
  }

  update_camera_data();
}

void DynamicResolution::update_vanilla_taa(const bool dlss_enabled) noexcept {
  if (temporal_aa_setting_ == nullptr) {
    return;
  }

  const auto policy = select_temporal_aa_policy(jitter_ownership_);
  const auto keep_engine_jitter =
      policy == TemporalAaPolicy::engine_jitter_without_resolve;
  const auto desired = !dlss_enabled        ? original_temporal_aa_enabled_
                       : keep_engine_jitter ? true
                                            : false;
  temporal_aa_setting_->data.b = desired;
  temporal_aa_override_active_ = dlss_enabled;
  if (dlss_enabled && temporal_aa_policy_logged_ != policy) {
    temporal_aa_policy_logged_ = policy;
    temporal_aa_override_logged_ = true;
    if (keep_engine_jitter) {
      logger::info(
          "Skyrim's own sub-pixel jitter path is left enabled -- BOTH "
          "the bUseTAA setting and the live image-space state -- so the "
          "world raster can carry the offset. Clearing the live state "
          "here is what previously made this probe fail: it gates the "
          "fold, not only the resolve. Skyrim's TAA resolve therefore "
          "runs as well for now. Whether the engine actually applies "
          "the offset is measured from its own matrices and reported by "
          "the jitter contract line.");
    } else {
      logger::warn(
          "Vanilla Skyrim TAA fully disabled: the engine did not apply "
          "a sub-pixel offset within {} frames that requested one, so "
          "the jitter/resolve split is not separable on this build. "
          "Reconstruction will run without new sub-pixel samples.",
          kSplitProbeFrames);
    }
  }

  auto *runtime_flag = runtime_taa_flag();
  if (runtime_flag == nullptr) {
    if (!runtime_taa_flag_absent_logged_) {
      runtime_taa_flag_absent_logged_ = true;
      logger::warn("Skyrim live TAA shader state was unavailable; the INI "
                   "setting was updated but could not be runtime-verified");
    }
    return;
  }

  if (!runtime_temporal_state_captured_) {
    original_runtime_temporal_aa_enabled_ = *runtime_flag;
    runtime_temporal_state_captured_ = true;
  }

  const auto runtime_desired =
      dlss_enabled ? true : original_runtime_temporal_aa_enabled_;
  *runtime_flag = runtime_desired;
  if (*runtime_flag != runtime_desired) {
    if (!runtime_taa_write_rejected_logged_) {
      runtime_taa_write_rejected_logged_ = true;
      logger::warn("Skyrim live TAA shader state did not accept the requested "
                   "value");
    }
    return;
  }
  if (dlss_enabled &&
      (!runtime_temporal_state_logged_ ||
       runtime_temporal_state_last_logged_ != runtime_desired)) {
    runtime_temporal_state_logged_ = true;
    runtime_temporal_state_last_logged_ = runtime_desired;
    if (runtime_desired) {
      logger::info(
          "Jitter/resolve split active: Skyrim's live image-space TAA "
          "state is ENABLED for the world render, so the engine folds "
          "this frame's sub-pixel offset into the raster, and cleared "
          "again immediately before the engine's post-processing so the "
          "vanilla TAA resolve does not consume the frame this plugin "
          "reconstructs. One state, two values, one frame -- the same "
          "split Community Shaders uses. Previously this state was "
          "cleared for the whole frame, which suppressed the resolve and "
          "the jitter fold together and left the raster unjittered.");
    } else {
      logger::info(
          "Vanilla Skyrim live TAA shader state disabled and verified");
    }
  }
}

void DynamicResolution::prepare_ui() noexcept {
  if (PresentationBridge::instance().uses_virtual_render_surface()) {
    return;
  }
  auto *state = RE::BSGraphics::State::GetSingleton();
  if (state == nullptr || !should_evaluate_) {
    return;
  }
  restore_full_resolution(state, true);
}

void DynamicResolution::enable_vanilla_taa_for_world_render() noexcept {

  if (!streamline::SuperResolution::instance().enabled()) {
    return;
  }
  auto *flag = runtime_taa_flag();
  if (flag == nullptr) {
    return;
  }

  *flag = !jitter_fold_patched_;
}

void DynamicResolution::suppress_vanilla_taa_resolve() noexcept {

  if (!streamline::SuperResolution::instance().enabled()) {
    return;
  }
  if (auto *flag = runtime_taa_flag(); flag != nullptr) {
    *flag = false;
  }
}

void DynamicResolution::begin_full_resolution_ui() noexcept {
  const auto &presentation = PresentationBridge::instance();
  full_resolution_ui_ = true;
  ui_target_redirection_failed_ = false;
  ui_target_declined_this_frame_ = false;
  ui_bind_index_ = 0;

  mark_frame_episode_phase(true);
  auto *state = RE::BSGraphics::State::GetSingleton();
  if (state == nullptr) {
    return;
  }
  if (presentation.uses_sub_rect_render_surface()) {

    restore_full_resolution(state, true);
    return;
  }

  state->frameBufferViewport[0] = presentation.output_width();
  state->frameBufferViewport[1] = presentation.output_height();
}

void DynamicResolution::end_full_resolution_ui() noexcept {
  if (!full_resolution_ui_) {
    return;
  }

  mark_frame_episode_phase(false);
  full_resolution_ui_ = false;
  const auto &presentation = PresentationBridge::instance();
  if (auto *state = RE::BSGraphics::State::GetSingleton(); state != nullptr) {

    const auto sub_rect = presentation.uses_sub_rect_render_surface();
    state->frameBufferViewport[0] =
        sub_rect ? presentation.output_width() : presentation.render_width();
    state->frameBufferViewport[1] =
        sub_rect ? presentation.output_height() : presentation.render_height();
  }
}

void DynamicResolution::begin_full_resolution_post_processing() noexcept {
  const auto &super_resolution = streamline::SuperResolution::instance();
  full_resolution_post_processing_ =
      !PresentationBridge::instance().uses_virtual_render_surface() &&
      super_resolution.enabled() && super_resolution.render_width() != 0 &&
      super_resolution.render_height() != 0 &&
      super_resolution.output_width() != 0 &&
      super_resolution.output_height() != 0 &&
      (super_resolution.render_width() < super_resolution.output_width() ||
       super_resolution.render_height() < super_resolution.output_height());
  if (full_resolution_post_processing_ &&
      !post_processing_guard_armed_logged_) {
    post_processing_guard_armed_logged_ = true;
    logger::info(
        "Full-resolution post-processing extent guard armed for "
        "{}x{} -> {}x{}",
        super_resolution.render_width(), super_resolution.render_height(),
        super_resolution.output_width(), super_resolution.output_height());
  }
}

void DynamicResolution::end_full_resolution_post_processing() noexcept {
  if (!full_resolution_post_processing_) {
    return;
  }

  auto *context = PresentationBridge::instance().d3d11_context();
  if (context != nullptr && !post_processing_guard_summary_logged_) {
    post_processing_guard_summary_logged_ = true;
    std::uint32_t target_width{};
    std::uint32_t target_height{};
    static_cast<void>(
        full_resolution_target_bound(context, &target_width, &target_height));

    D3D11_VIEWPORT viewport{};
    UINT viewport_count = 1;
    context->RSGetViewports(&viewport_count, &viewport);
    D3D11_RECT rectangle{};
    UINT rectangle_count = 1;
    context->RSGetScissorRects(&rectangle_count, &rectangle);
    logger::info("Post-processing extent guard completed: target={}x{}, "
                 "viewport={:.0f}x{:.0f}, scissor={}x{}, promotion={}",
                 target_width, target_height,
                 viewport_count != 0 ? viewport.Width : 0.0F,
                 viewport_count != 0 ? viewport.Height : 0.0F,
                 rectangle_count != 0 ? rectangle.right - rectangle.left : 0,
                 rectangle_count != 0 ? rectangle.bottom - rectangle.top : 0,
                 post_processing_promotion_logged_);
  }
  full_resolution_post_processing_ = false;
}

void DynamicResolution::restore_full_resolution(
    RE::BSGraphics::State *state, const bool restore_projection) noexcept {
  if (state == nullptr) {
    return;
  }
  if (restore_projection) {
    state_value<float>(state, kProjectionScaleXOffset) =
        original_projection_scale_x_;
    state_value<float>(state, kProjectionScaleYOffset) =
        original_projection_scale_y_;
  }
  state_value<float>(state, kDynamicWidthScaleOffset) = 1.0F;
  state_value<float>(state, kDynamicHeightScaleOffset) = 1.0F;
  state_value<float>(state, kDynamicPreviousWidthScaleOffset) = 1.0F;
  state_value<float>(state, kDynamicPreviousHeightScaleOffset) = 1.0F;
  state_value<std::int32_t>(state, kDynamicResolutionLockOffset) = 1;

  const auto &super_resolution = streamline::SuperResolution::instance();
  const auto output_width = super_resolution.output_width();
  const auto output_height = super_resolution.output_height();
  const auto previous_viewport_width = state->frameBufferViewport[0];
  const auto previous_viewport_height = state->frameBufferViewport[1];
  if (output_width != 0 && output_height != 0) {

    state->frameBufferViewport[0] = output_width;
    state->frameBufferViewport[1] = output_height;
  }

  if (auto *renderer = RE::BSGraphics::Renderer::GetSingleton();
      renderer != nullptr) {
    renderer->UpdateViewPort(0, 0, true);
  }
  if (auto *context = PresentationBridge::instance().d3d11_context();
      context != nullptr && output_width != 0 && output_height != 0) {
    const D3D11_VIEWPORT viewport{0.0F,
                                  0.0F,
                                  static_cast<float>(output_width),
                                  static_cast<float>(output_height),
                                  0.0F,
                                  1.0F};
    const D3D11_RECT scissor{0, 0, static_cast<LONG>(output_width),
                             static_cast<LONG>(output_height)};
    context->RSSetViewports(1, &viewport);
    context->RSSetScissorRects(1, &scissor);
  }
  update_camera_data();

  if (!full_resolution_restore_logged_) {
    full_resolution_restore_logged_ = true;
    logger::info("Full-resolution ratio, cached viewport {}x{} -> {}x{}, D3D11 "
                 "viewport/scissor, projection, and camera constants restored "
                 "after DLSS",
                 previous_viewport_width, previous_viewport_height,
                 output_width, output_height);
  }
}
}
