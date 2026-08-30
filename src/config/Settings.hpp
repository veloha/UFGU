#pragma once

#include "providers/ProviderTypes.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>

namespace mfgdlss::config
{
enum class UpscalingMode : std::uint32_t
{
    off,
    dlaa,
    quality,
    balanced,
    performance,
    ultra_performance
};

enum class ReflexMode : std::uint32_t
{
    off,
    low_latency,
    low_latency_boost
};

enum class DlssPreset : std::uint32_t
{
    recommended,
    legacy_e,
    j,
    k,
    l,
    m
};

enum class DlssTemporalInputs : std::uint32_t
{
    standard,
    no_hints,
    raw_motion,
    raw_motion_no_hints
};

enum class TemporalJitter : std::uint32_t
{
    halton,
    disabled
};

enum class SurfaceModel : std::uint32_t
{
    complete_frame,
    sub_rect
};

enum class VendorFrameGeneration : std::uint32_t
{
    off,
    intel,
    amd
};

enum class MultiplierMode : std::uint32_t
{
    dynamic,
    fixed
};

enum class JitterFold : std::uint32_t
{
    engine,
    nop_gate
};

enum class MipBiasMode : std::uint32_t
{
    off,
    automatic
};

enum class JitterSource : std::uint32_t
{
    requested,
    measured
};

enum class OverlayFps : std::uint32_t
{
    both,
    output,
    rendered
};

enum class MenuPresentation : std::uint32_t
{
    full,
    bar,
    dock,
    drawer,
    card,
    corner
};

inline constexpr std::uint32_t kMenuPresentationCount = 6U;

enum class FsrColorSpace : std::uint32_t
{
    non_linear_srgb,
    linear
};

enum class FsrDepthOverride : std::uint32_t
{
    measured,
    standard_finite,
    standard_infinite,
    inverted_finite,
    inverted_infinite
};

enum class DepthInvertedOverride : std::uint32_t
{
    measured,
    force_inverted,
    force_standard
};

enum class MotionDilation : std::uint32_t
{
    standard,
    near_fade,
    off
};

class Settings final
{
public:
    [[nodiscard]] static Settings& instance() noexcept;

    [[nodiscard]] bool load();
    [[nodiscard]] bool loaded() const noexcept;
    [[nodiscard]] bool reload_if_changed();
    [[nodiscard]] bool frame_generation_enabled() const noexcept;
    [[nodiscard]] bool set_frame_generation_enabled(bool enabled);
    [[nodiscard]] bool set_frame_generation_multiplier(
        std::uint32_t multiplier);
    [[nodiscard]] bool set_multiplier_mode(MultiplierMode mode);
    [[nodiscard]] bool set_upscaling_provider(providers::Vendor provider);

    [[nodiscard]] bool commit_upscaling_selection(
        providers::Vendor provider,
        UpscalingMode mode);

    [[nodiscard]] bool stage_upscaling_for_restart(
        providers::Vendor provider,
        UpscalingMode mode);
    [[nodiscard]] bool set_upscaling_mode(UpscalingMode mode);
    [[nodiscard]] bool set_reflex_mode(ReflexMode mode);
    [[nodiscard]] bool set_base_frame_limit(std::uint32_t frame_limit);
    [[nodiscard]] bool set_frame_limit(std::uint32_t frame_limit);
    [[nodiscard]] bool set_sharpness(float sharpness);
    [[nodiscard]] bool set_overlay_enabled(bool enabled);
    [[nodiscard]] bool set_overlay_fps_display(OverlayFps display);
    [[nodiscard]] bool set_menu_presentation(MenuPresentation presentation);
    [[nodiscard]] bool set_menu_sounds_enabled(bool enabled);
    [[nodiscard]] std::uint32_t frame_generation_multiplier() const noexcept;

    [[nodiscard]] MultiplierMode multiplier_mode() const noexcept;
    [[nodiscard]] providers::Vendor upscaling_provider() const noexcept;
    [[nodiscard]] UpscalingMode upscaling_mode() const noexcept;
    [[nodiscard]] ReflexMode reflex_mode() const noexcept;
    [[nodiscard]] std::uint32_t base_frame_limit() const noexcept;
    [[nodiscard]] std::uint32_t frame_limit() const noexcept;
    [[nodiscard]] DlssPreset dlss_preset() const noexcept;
    [[nodiscard]] DlssTemporalInputs dlss_temporal_inputs() const noexcept;
    [[nodiscard]] TemporalJitter temporal_jitter() const noexcept;

    [[nodiscard]] SurfaceModel surface_model() const noexcept;

    [[nodiscard]] JitterFold jitter_fold() const noexcept;
    [[nodiscard]] MipBiasMode mip_bias_mode() const noexcept;

    [[nodiscard]] VendorFrameGeneration vendor_frame_generation() const noexcept;
    [[nodiscard]] float sharpness() const noexcept;

    [[nodiscard]] float motion_scale_x() const noexcept;
    [[nodiscard]] float motion_scale_y() const noexcept;

    [[nodiscard]] JitterSource jitter_source() const noexcept;
    [[nodiscard]] FsrColorSpace fsr_color_space() const noexcept;

    [[nodiscard]] FsrDepthOverride fsr_depth_override() const noexcept;
    [[nodiscard]] DepthInvertedOverride
        depth_inverted_override() const noexcept;

    [[nodiscard]] MotionDilation motion_dilation() const noexcept;
    [[nodiscard]] float depth_object_separation() const noexcept;

    [[nodiscard]] bool allow_tearing() const noexcept;
    [[nodiscard]] std::uint32_t back_buffer_count() const noexcept;
    [[nodiscard]] bool overlay_enabled() const noexcept;
    [[nodiscard]] bool d3d12_debug_layer() const noexcept;
    [[nodiscard]] bool experimental_features() const noexcept;
    [[nodiscard]] bool show_only_interpolated_frames() const noexcept;

    [[nodiscard]] OverlayFps overlay_fps_display() const noexcept;
    [[nodiscard]] MenuPresentation menu_presentation() const noexcept;
    [[nodiscard]] bool menu_sounds_enabled() const noexcept;
    [[nodiscard]] float menu_sound_volume() const noexcept;

    [[nodiscard]] std::uint32_t debug_view_index() const noexcept;
    [[nodiscard]] std::uint32_t menu_key() const noexcept;

private:
    std::filesystem::path path_;
    std::filesystem::file_time_type last_write_time_{};

    std::chrono::steady_clock::time_point last_change_check_{};
    bool loaded_{};
    bool frame_generation_enabled_{false};
    std::uint32_t frame_generation_multiplier_{2};
    MultiplierMode multiplier_mode_{MultiplierMode::fixed};
    providers::Vendor upscaling_provider_{providers::Vendor::nvidia};
    UpscalingMode upscaling_mode_{UpscalingMode::off};
    ReflexMode reflex_mode_{ReflexMode::off};
    std::uint32_t base_frame_limit_{};
    bool show_only_interpolated_frames_{};
    std::uint32_t frame_limit_{};
    DlssPreset dlss_preset_{DlssPreset::recommended};
    DlssTemporalInputs dlss_temporal_inputs_{
        DlssTemporalInputs::standard};
    TemporalJitter temporal_jitter_{TemporalJitter::halton};
    SurfaceModel surface_model_{SurfaceModel::complete_frame};
    bool surface_model_latched_{};
    JitterFold jitter_fold_{JitterFold::nop_gate};
    MipBiasMode mip_bias_mode_{MipBiasMode::off};
    bool jitter_fold_latched_{};
    VendorFrameGeneration vendor_frame_generation_{VendorFrameGeneration::off};
    bool vendor_frame_generation_latched_{};
    bool surface_model_change_logged_{};
    bool jitter_fold_change_logged_{};
    bool vendor_frame_generation_change_logged_{};
    float sharpness_{};
    float motion_scale_x_{1.0F};
    float motion_scale_y_{1.0F};
    JitterSource jitter_source_{JitterSource::requested};
    FsrColorSpace fsr_color_space_{FsrColorSpace::non_linear_srgb};
    FsrDepthOverride fsr_depth_override_{FsrDepthOverride::measured};
    DepthInvertedOverride depth_inverted_override_{
        DepthInvertedOverride::measured};
    MotionDilation motion_dilation_{MotionDilation::standard};
    float depth_object_separation_{40.0F};
    bool allow_tearing_{};
    std::uint32_t back_buffer_count_{};
    bool overlay_enabled_{true};
    bool d3d12_debug_layer_{};
    bool experimental_features_{};
    OverlayFps overlay_fps_display_{OverlayFps::both};
    MenuPresentation menu_presentation_{MenuPresentation::full};
    bool menu_sounds_enabled_{true};
    float menu_sound_volume_{0.6F};
    std::uint32_t debug_view_index_{};
    std::uint32_t menu_key_{0x22};
};
}
