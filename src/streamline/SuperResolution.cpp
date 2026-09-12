#include "streamline/SuperResolution.hpp"

#include "providers/FsrUpscaler.hpp"
#include "providers/XessUpscaler.hpp"
#include "streamline/ProjectIdentity.hpp"
#include "streamline/StreamlineApi.hpp"

#include <d3d11.h>

#include <SKSE/SKSE.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <algorithm>
#include <filesystem>
#include <string_view>

namespace mfgdlss::streamline
{
namespace
{
namespace logger = SKSE::log;

#ifndef MFG_DLSS_STREAMLINE_APPLICATION_ID
#error MFG_DLSS_STREAMLINE_APPLICATION_ID must be supplied by CMake.
#endif

constexpr auto kApplicationId =
    static_cast<unsigned long long>(MFG_DLSS_STREAMLINE_APPLICATION_ID);
using mfgdlss::streamline::kProjectId;

[[nodiscard]] std::string_view mode_name(
    const config::UpscalingMode mode) noexcept
{
    switch (mode) {
    case config::UpscalingMode::off:
        return "Off";
    case config::UpscalingMode::dlaa:
        return "DLAA";
    case config::UpscalingMode::quality:
        return "Quality";
    case config::UpscalingMode::balanced:
        return "Balanced";
    case config::UpscalingMode::performance:
        return "Performance";
    case config::UpscalingMode::ultra_performance:
        return "UltraPerformance";
    }
    return "Unknown";
}

[[nodiscard]] NVSDK_NGX_PerfQuality_Value performance_quality(
    const config::UpscalingMode mode) noexcept
{
    switch (mode) {
    case config::UpscalingMode::dlaa:
        return NVSDK_NGX_PerfQuality_Value_DLAA;
    case config::UpscalingMode::quality:
        return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case config::UpscalingMode::balanced:
        return NVSDK_NGX_PerfQuality_Value_Balanced;
    case config::UpscalingMode::performance:
        return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case config::UpscalingMode::ultra_performance:
        return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    case config::UpscalingMode::off:
        return NVSDK_NGX_PerfQuality_Value_DLAA;
    }
    return NVSDK_NGX_PerfQuality_Value_DLAA;
}

[[nodiscard]] providers::QualityMode provider_quality(
    const config::UpscalingMode mode) noexcept
{
    switch (mode) {
    case config::UpscalingMode::dlaa:
        return providers::QualityMode::native_antialiasing;
    case config::UpscalingMode::quality:
        return providers::QualityMode::quality;
    case config::UpscalingMode::balanced:
        return providers::QualityMode::balanced;
    case config::UpscalingMode::performance:
        return providers::QualityMode::performance;
    case config::UpscalingMode::ultra_performance:
        return providers::QualityMode::ultra_performance;
    case config::UpscalingMode::off:
        return providers::QualityMode::off;
    }
    return providers::QualityMode::off;
}

[[nodiscard]] NVSDK_NGX_DLSS_Hint_Render_Preset recommended_preset(
    const config::UpscalingMode mode) noexcept
{
    switch (mode) {
    case config::UpscalingMode::performance:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_M;
    case config::UpscalingMode::ultra_performance:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_L;
    case config::UpscalingMode::off:
    case config::UpscalingMode::dlaa:
    case config::UpscalingMode::quality:
    case config::UpscalingMode::balanced:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
    }
    return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
}

[[nodiscard]] NVSDK_NGX_DLSS_Hint_Render_Preset selected_preset(
    const config::UpscalingMode mode) noexcept
{
    switch (config::Settings::instance().dlss_preset()) {
    case config::DlssPreset::recommended:
        return recommended_preset(mode);
    case config::DlssPreset::legacy_e:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_E;
    case config::DlssPreset::j:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_J;
    case config::DlssPreset::k:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
    case config::DlssPreset::l:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_L;
    case config::DlssPreset::m:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_M;
    }
    return recommended_preset(mode);
}

void set_presets(
    NVSDK_NGX_Parameter* parameters,
    const NVSDK_NGX_DLSS_Hint_Render_Preset preset)
{
    parameters->Set(
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
        static_cast<unsigned>(preset));
    parameters->Set(
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
        static_cast<unsigned>(preset));
    parameters->Set(
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
        static_cast<unsigned>(preset));
    parameters->Set(
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
        static_cast<unsigned>(preset));
    parameters->Set(
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
        static_cast<unsigned>(preset));
    parameters->Set(
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
        static_cast<unsigned>(preset));
}
}

SuperResolution& SuperResolution::instance() noexcept
{
    static SuperResolution super_resolution;
    return super_resolution;
}

bool SuperResolution::initialize(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const std::uint32_t output_width,
    const std::uint32_t output_height)
{
    const auto configured_provider =
        config::Settings::instance().upscaling_provider();
    if (ready_ && device_ == device && context_ == context &&
        output_width_ == output_width && output_height_ == output_height &&
        provider_ == configured_provider) {
        return true;
    }
    if (device == nullptr || context == nullptr ||
        output_width == 0 || output_height == 0) {
        return false;
    }
    shutdown();

    if (configured_provider != providers::Vendor::nvidia &&
        configured_provider != providers::Vendor::amd &&
        configured_provider != providers::Vendor::intel) {
        logger::error(
            "No concrete super-resolution provider was selected at startup");
        return false;
    }
    device_ = device;
    context_ = context;
    device_->AddRef();
    context_->AddRef();
    output_width_ = output_width;
    output_height_ = output_height;
    render_width_ = output_width;
    render_height_ = output_height;
    provider_ = configured_provider;
    mode_ = config::UpscalingMode::off;
    ready_ = true;
    if (configured_provider == providers::Vendor::nvidia &&
        !ensure_ngx_controller()) {
        shutdown();
        return false;
    }
    logger::info(
        "Provider-neutral super-resolution controller ready for {} at "
        "{}x{} output{}",
        providers::vendor_display_name(provider_),
        output_width_,
        output_height_,
        configured_provider == providers::Vendor::nvidia ?
            "; NGX ready" : "; NGX deferred until requested");
    return true;
}

bool SuperResolution::ensure_ngx_controller()
{
    if (ngx_initialized_ && capability_parameters_ != nullptr &&
        feature_parameters_ != nullptr) {
        return true;
    }
    if (!ready_ || device_ == nullptr || context_ == nullptr) {
        return false;
    }

    release_ngx();
    const auto runtime_path = Api::instance().runtime_directory().wstring();
    const wchar_t* runtime_paths[]{runtime_path.c_str()};
    NVSDK_NGX_FeatureCommonInfo feature_info{};
    feature_info.PathListInfo.Path = runtime_paths;
    feature_info.PathListInfo.Length = 1;

    auto data_path = std::filesystem::temp_directory_path() / L"UFGU-NGX";
    std::error_code error;
    std::filesystem::create_directories(data_path, error);

    NVSDK_NGX_Result init_result{};
    if constexpr (kApplicationId != 0) {
        init_result = NVSDK_NGX_D3D11_Init(
            kApplicationId,
            data_path.c_str(),
            device_,
            &feature_info,
            NVSDK_NGX_Version_API);
    } else {
        init_result = NVSDK_NGX_D3D11_Init_with_ProjectID(
            kProjectId.data(),
            NVSDK_NGX_ENGINE_TYPE_CUSTOM,
            runtime_engine_version().c_str(),
            data_path.c_str(),
            device_,
            &feature_info,
            NVSDK_NGX_Version_API);
    }
    if (NVSDK_NGX_FAILED(init_result)) {
        logger::error(
            "Native D3D11 NGX initialization failed: 0x{:08X}",
            static_cast<unsigned>(init_result));
        return false;
    }
    ngx_initialized_ = true;

    const auto parameter_result =
        NVSDK_NGX_D3D11_GetCapabilityParameters(&capability_parameters_);
    int available{};
    const auto availability_result =
        NVSDK_NGX_SUCCEED(parameter_result) &&
                capability_parameters_ != nullptr ?
            capability_parameters_->Get(
                NVSDK_NGX_Parameter_SuperSampling_Available,
                &available) :
            parameter_result;
    if (NVSDK_NGX_FAILED(availability_result) || available == 0) {
        logger::error(
            "Native D3D11 DLSS is unavailable: parameters=0x{:08X}, "
            "availability=0x{:08X}, available={}",
            static_cast<unsigned>(parameter_result),
            static_cast<unsigned>(availability_result),
            available);
        release_ngx();
        return false;
    }

    const auto allocation_result =
        NVSDK_NGX_D3D11_AllocateParameters(&feature_parameters_);
    if (NVSDK_NGX_FAILED(allocation_result) ||
        feature_parameters_ == nullptr) {
        logger::error(
            "Native D3D11 DLSS feature parameter allocation failed: "
            "0x{:08X}",
            static_cast<unsigned>(allocation_result));
        release_ngx();
        return false;
    }
    logger::info(
        "Native D3D11 NGX DLSS controller prepared lazily for {}x{} output",
        output_width_,
        output_height_);
    return true;
}

bool SuperResolution::reconfigure(
    const std::uint32_t output_width,
    const std::uint32_t output_height)
{
    if (!ready_ || output_width == 0 || output_height == 0) {
        return false;
    }
    if (output_width_ == output_width && output_height_ == output_height) {
        return true;
    }
    const auto previous_mode = mode_;
    release_feature();
    output_width_ = output_width;
    output_height_ = output_height;
    render_width_ = output_width;
    render_height_ = output_height;
    mode_ = config::UpscalingMode::off;
    return set_mode(previous_mode);
}

bool SuperResolution::optimal_render_resolution(
    const config::UpscalingMode mode,
    std::uint32_t& width,
    std::uint32_t& height) const
{
    width = 0;
    height = 0;
    if (!ready_ || output_width_ == 0 || output_height_ == 0) {
        return false;
    }
    return optimal_render_resolution_for(provider_, mode, width, height);
}

bool SuperResolution::optimal_render_resolution_for(
    const providers::Vendor provider,
    const config::UpscalingMode mode,
    std::uint32_t& width,
    std::uint32_t& height) const
{
    width = 0;
    height = 0;
    if (!ready_ || output_width_ == 0 || output_height_ == 0) {
        return false;
    }
    if (mode == config::UpscalingMode::off) {
        width = output_width_;
        height = output_height_;
        return true;
    }

    if (provider == providers::Vendor::amd) {
        return providers::FsrUpscaler::instance().query_render_resolution(
            provider_quality(mode),
            output_width_,
            output_height_,
            width,
            height);
    }
    if (provider == providers::Vendor::intel) {
        return providers::XessUpscaler::instance().query_render_resolution(
            provider_quality(mode),
            output_width_,
            output_height_,
            width,
            height);
    }
    if (provider != providers::Vendor::nvidia ||
        capability_parameters_ == nullptr) {
        return false;
    }
    if (mode == config::UpscalingMode::dlaa) {
        width = output_width_;
        height = output_height_;
        return true;
    }

    unsigned render_width{};
    unsigned render_height{};
    unsigned maximum_width{};
    unsigned maximum_height{};
    unsigned minimum_width{};
    unsigned minimum_height{};
    float recommended_sharpness{};
    const auto result = NGX_DLSS_GET_OPTIMAL_SETTINGS(
        capability_parameters_,
        output_width_,
        output_height_,
        performance_quality(mode),
        &render_width,
        &render_height,
        &maximum_width,
        &maximum_height,
        &minimum_width,
        &minimum_height,
        &recommended_sharpness);
    if (NVSDK_NGX_FAILED(result) ||
        render_width == 0 || render_height == 0) {
        logger::error(
            "Native D3D11 DLSS {} optimal resize query failed: 0x{:08X}",
            mode_name(mode),
            static_cast<unsigned>(result));
        return false;
    }

    width = render_width;
    height = render_height;
    return true;
}

bool SuperResolution::query_provider_render_extent(
    const providers::Vendor provider,
    const config::UpscalingMode mode,
    std::uint32_t& width,
    std::uint32_t& height)
{
    if (!ready_ || mode == config::UpscalingMode::off) {
        return false;
    }

    if (provider == providers::Vendor::nvidia && !ensure_ngx_controller()) {
        return false;
    }
    return optimal_render_resolution_for(provider, mode, width, height);
}

bool SuperResolution::prepare_provider_at_extent(
    const providers::Vendor provider,
    const config::UpscalingMode mode,
    const std::uint32_t render_width,
    const std::uint32_t render_height)
{
    if (!ready_ || mode == config::UpscalingMode::off ||
        render_width == 0 || render_height == 0 ||
        output_width_ == 0 || output_height_ == 0 ||
        render_width > output_width_ || render_height > output_height_) {
        return false;
    }

    if (mode == config::UpscalingMode::dlaa) {
        return render_width == output_width_ &&
               render_height == output_height_;
    }

    if (provider == providers::Vendor::amd) {
        return providers::FsrUpscaler::instance().supports_render_resolution(
            provider_quality(mode),
            output_width_,
            output_height_,
            render_width,
            render_height);
    }
    if (provider == providers::Vendor::intel) {
        return providers::XessUpscaler::instance().supports_render_resolution(
            provider_quality(mode),
            output_width_,
            output_height_,
            render_width,
            render_height);
    }
    if (provider != providers::Vendor::nvidia) {
        return false;
    }

    if (!ensure_ngx_controller() || capability_parameters_ == nullptr) {
        return false;
    }
    unsigned optimal_width{};
    unsigned optimal_height{};
    unsigned maximum_width{};
    unsigned maximum_height{};
    unsigned minimum_width{};
    unsigned minimum_height{};
    float recommended_sharpness{};
    const auto result = NGX_DLSS_GET_OPTIMAL_SETTINGS(
        capability_parameters_,
        output_width_,
        output_height_,
        performance_quality(mode),
        &optimal_width,
        &optimal_height,
        &maximum_width,
        &maximum_height,
        &minimum_width,
        &minimum_height,
        &recommended_sharpness);
    if (NVSDK_NGX_FAILED(result) || maximum_width == 0 ||
        maximum_height == 0) {
        logger::error(
            "DLSS {} render-extent range query failed: 0x{:08X}",
            mode_name(mode),
            static_cast<unsigned>(result));
        return false;
    }
    if (render_width < minimum_width || render_height < minimum_height ||
        render_width > maximum_width || render_height > maximum_height) {
        logger::info(
            "DLSS {} would refuse an explicit {}x{} input for a {}x{} output "
            "(it recommends {}x{}; supported range {}x{}-{}x{})",
            mode_name(mode),
            render_width,
            render_height,
            output_width_,
            output_height_,
            optimal_width,
            optimal_height,
            minimum_width,
            minimum_height,
            maximum_width,
            maximum_height);
        return false;
    }
    logger::info(
        "DLSS {} accepts an explicit {}x{} input for a {}x{} output (it "
        "recommends {}x{}; supported range {}x{}-{}x{})",
        mode_name(mode),
        render_width,
        render_height,
        output_width_,
        output_height_,
        optimal_width,
        optimal_height,
        minimum_width,
        minimum_height,
        maximum_width,
        maximum_height);
    return ensure_nvidia_feature(mode, render_width, render_height);
}

bool SuperResolution::prepare_provider(
    const providers::Vendor provider,
    const config::UpscalingMode mode,
    std::uint32_t& render_width,
    std::uint32_t& render_height)
{
    if (!ready_ || mode == config::UpscalingMode::off) {
        return false;
    }
    if (provider == providers::Vendor::nvidia && !ensure_ngx_controller()) {
        return false;
    }
    if (!optimal_render_resolution_for(
            provider, mode, render_width, render_height)) {
        return false;
    }
    return provider != providers::Vendor::nvidia ||
           ensure_nvidia_feature(mode, render_width, render_height);
}

bool SuperResolution::set_mode(const config::UpscalingMode mode)
{
    if (!ready_) {
        return false;
    }
    if (mode == config::UpscalingMode::off) {
        mode_ = config::UpscalingMode::off;
        render_width_ = output_width_;
        render_height_ = output_height_;
        logger::info(
            "{} super resolution disabled",
            providers::vendor_display_name(provider_));
        return true;
    }

    std::uint32_t render_width{};
    std::uint32_t render_height{};
    if (!prepare_provider(provider_, mode, render_width, render_height)) {
        logger::error(
            "{} {} render contract could not be prepared by its runtime",
            providers::vendor_display_name(provider_),
            mode_name(mode));
        return false;
    }
    return commit_prepared_provider(
        provider_, mode, render_width, render_height);
}

bool SuperResolution::ensure_nvidia_feature(
    const config::UpscalingMode mode,
    const std::uint32_t render_width,
    const std::uint32_t render_height)
{
    if (!ensure_ngx_controller()) {
        return false;
    }
    if (feature_ != nullptr && feature_mode_ == mode &&
        feature_render_width_ == render_width &&
        feature_render_height_ == render_height &&
        feature_output_width_ == output_width_ &&
        feature_output_height_ == output_height_ &&
        feature_color_input_ == color_input_) {
        return true;
    }
    release_feature();

    const auto quality = performance_quality(mode);
    unsigned maximum_width{};
    unsigned maximum_height{};
    unsigned minimum_width{};
    unsigned minimum_height{};
    unsigned runtime_render_width{};
    unsigned runtime_render_height{};
    float recommended_sharpness{};
    const auto optimal_result = NGX_DLSS_GET_OPTIMAL_SETTINGS(
        capability_parameters_, output_width_, output_height_, quality,
        &runtime_render_width, &runtime_render_height,
        &maximum_width, &maximum_height,
        &minimum_width, &minimum_height, &recommended_sharpness);
    if (NVSDK_NGX_FAILED(optimal_result) || maximum_width == 0 ||
        maximum_height == 0) {
        logger::error(
            "Native D3D11 DLSS {} shadow preparation could not read the "
            "runtime extent range for {}x{} output: result=0x{:08X}",
            mode_name(mode),
            output_width_,
            output_height_,
            static_cast<unsigned>(optimal_result));
        return false;
    }

    if (render_width < minimum_width || render_height < minimum_height ||
        render_width > maximum_width || render_height > maximum_height) {
        logger::error(
            "Native D3D11 DLSS {} shadow preparation refused {}x{} for {}x{} "
            "output: outside the supported range {}x{}-{}x{}",
            mode_name(mode),
            render_width,
            render_height,
            output_width_,
            output_height_,
            minimum_width,
            minimum_height,
            maximum_width,
            maximum_height);
        return false;
    }
    if (runtime_render_width != render_width ||
        runtime_render_height != render_height) {
        logger::info(
            "Native D3D11 DLSS {} is rendering at {}x{} rather than the {}x{} "
            "it recommends for {}x{} output, inside the supported range "
            "{}x{}-{}x{}. The active renderer contract chooses the extent.",
            mode_name(mode),
            render_width,
            render_height,
            runtime_render_width,
            runtime_render_height,
            output_width_,
            output_height_,
            minimum_width,
            minimum_height,
            maximum_width,
            maximum_height);
    }

    const auto preset = selected_preset(mode);
    set_presets(feature_parameters_, preset);
    feature_parameters_->Set(
        NVSDK_NGX_Parameter_CreationNodeMask,
        1U);
    feature_parameters_->Set(
        NVSDK_NGX_Parameter_VisibilityNodeMask,
        1U);
    feature_parameters_->Set(
        NVSDK_NGX_Parameter_FreeMemOnReleaseFeature,
        1U);
    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = render_width;
    create.Feature.InHeight = render_height;
    create.Feature.InTargetWidth = output_width_;
    create.Feature.InTargetHeight = output_height_;
    create.Feature.InPerfQualityValue = quality;

    const bool auto_exposure =
        color_input_ == DlssColorInput::linear_hdr;
    create.InFeatureCreateFlags = auto_exposure ?
        NVSDK_NGX_DLSS_Feature_Flags_AutoExposure :
        NVSDK_NGX_DLSS_Feature_Flags_None;
    if (color_input_ == DlssColorInput::linear_hdr) {
        create.InFeatureCreateFlags |=
            NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    }

    create.InEnableOutputSubrects = false;
    if (mode != config::UpscalingMode::dlaa) {
        create.InFeatureCreateFlags |=
            NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    }

    const auto create_result = NGX_D3D11_CREATE_DLSS_EXT(
        context_,
        &feature_,
        feature_parameters_,
        &create);
    if (NVSDK_NGX_FAILED(create_result) || feature_ == nullptr) {
        logger::error(
            "Native D3D11 DLSS {} feature creation failed: 0x{:08X}",
            mode_name(mode),
            static_cast<unsigned>(create_result));
        release_feature();
        return false;
    }

    unsigned contract_width{};
    unsigned contract_height{};
    unsigned contract_output_width{};
    unsigned contract_output_height{};
    int output_subrects{};
    int feature_create_flags{};
    const auto width_result = feature_parameters_->Get(
        NVSDK_NGX_Parameter_Width,
        &contract_width);
    const auto height_result = feature_parameters_->Get(
        NVSDK_NGX_Parameter_Height,
        &contract_height);
    const auto output_width_result = feature_parameters_->Get(
        NVSDK_NGX_Parameter_OutWidth,
        &contract_output_width);
    const auto output_height_result = feature_parameters_->Get(
        NVSDK_NGX_Parameter_OutHeight,
        &contract_output_height);
    const auto output_subrect_result = feature_parameters_->Get(
        NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects,
        &output_subrects);
    const auto feature_flags_result = feature_parameters_->Get(
        NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
        &feature_create_flags);
    const bool contract_valid =
        NVSDK_NGX_SUCCEED(width_result) &&
        NVSDK_NGX_SUCCEED(height_result) &&
        NVSDK_NGX_SUCCEED(output_width_result) &&
        NVSDK_NGX_SUCCEED(output_height_result) &&
        NVSDK_NGX_SUCCEED(output_subrect_result) &&
        NVSDK_NGX_SUCCEED(feature_flags_result) &&
        contract_width == render_width &&
        contract_height == render_height &&
        contract_output_width == output_width_ &&
        contract_output_height == output_height_ &&
        output_subrects == 0 &&
        feature_create_flags == create.InFeatureCreateFlags;
    logger::info(
        "Native D3D11 DLSS feature contract: {}x{} -> {}x{}, "
        "output-subrects={}, create-flags=0x{:X}, auto-exposure={}, "
        "verified={}",
        contract_width,
        contract_height,
        contract_output_width,
        contract_output_height,
        output_subrects,
        static_cast<unsigned>(feature_create_flags),
        auto_exposure,
        contract_valid);
    if (!contract_valid) {
        logger::error(
            "Native D3D11 DLSS rejected the requested feature dimensions "
            "or output-subrect policy");
        release_feature();
        return false;
    }

    feature_mode_ = mode;
    feature_render_width_ = render_width;
    feature_render_height_ = render_height;
    feature_output_width_ = output_width_;
    feature_output_height_ = output_height_;
    feature_color_input_ = color_input_;
    logger::info(
        "Native D3D11 DLSS {} configured: {}x{} -> {}x{}, preset={}, "
        "input={}, auto-exposure={}, range={}x{}-{}x{}",
        mode_name(mode),
        render_width,
        render_height,
        output_width_,
        output_height_,
        static_cast<unsigned>(preset),
        color_input_ == DlssColorInput::linear_hdr ? "linear-HDR" : "display-LDR",
        auto_exposure,
        minimum_width,
        minimum_height,
        maximum_width,
        maximum_height);
    return true;
}

bool SuperResolution::commit_prepared_provider(
    const providers::Vendor provider,
    const config::UpscalingMode mode,
    const std::uint32_t render_width,
    const std::uint32_t render_height) noexcept
{
    if (!ready_ || mode == config::UpscalingMode::off ||
        render_width == 0 || render_height == 0 ||
        (provider != providers::Vendor::nvidia &&
         provider != providers::Vendor::amd &&
         provider != providers::Vendor::intel) ||
        (provider == providers::Vendor::nvidia &&
         (feature_ == nullptr || feature_mode_ != mode ||
          feature_render_width_ != render_width ||
          feature_render_height_ != render_height))) {
        return false;
    }
    provider_ = provider;
    mode_ = mode;
    render_width_ = render_width;
    render_height_ = render_height;
    logger::info(
        "Active upscaling contract committed atomically: {} {} {}x{} -> "
        "{}x{}",
        providers::vendor_display_name(provider_),
        mode_name(mode_),
        render_width_,
        render_height_,
        output_width_,
        output_height_);
    return true;
}

void SuperResolution::set_color_input(
    const DlssColorInput input) noexcept
{
    color_input_ = input;
}

bool SuperResolution::evaluate(
    ID3D11Resource* color,
    ID3D11Resource* output,
    ID3D11Resource* depth,
    ID3D11Resource* motion_vectors,
    ID3D11Resource* bias_current_color,
    ID3D11Resource* transparency,
    const float jitter_x,
    const float jitter_y,
    const bool reset)
{
    if (!ready_ || feature_ == nullptr ||
        color == nullptr || output == nullptr ||
        depth == nullptr || motion_vectors == nullptr) {
        return false;
    }

    NVSDK_NGX_D3D11_DLSS_Eval_Params evaluation{};
    evaluation.Feature.pInColor = color;
    evaluation.Feature.pInOutput = output;
    evaluation.Feature.InSharpness = 0.0F;
    evaluation.pInDepth = depth;
    evaluation.pInMotionVectors = motion_vectors;
    evaluation.pInBiasCurrentColorMask = bias_current_color;
    evaluation.pInTransparencyMask = transparency;

    evaluation.InJitterOffsetX = jitter_x;
    evaluation.InJitterOffsetY = jitter_y;
    evaluation.InRenderSubrectDimensions = {
        feature_render_width_,
        feature_render_height_};
    evaluation.InOutputSubrectBase = {0, 0};
    evaluation.InReset = reset ? 1 : 0;

    evaluation.InMVScaleX = static_cast<float>(feature_render_width_);
    evaluation.InMVScaleY = static_cast<float>(feature_render_height_);
    evaluation.InPreExposure = 1.0F;
    evaluation.InExposureScale = 1.0F;

    const auto result = NGX_D3D11_EVALUATE_DLSS_EXT(
        context_,
        feature_,
        feature_parameters_,
        &evaluation);
    if (NVSDK_NGX_FAILED(result)) {
        logger::error(
            "Native D3D11 DLSS evaluation failed: 0x{:08X}",
            static_cast<unsigned>(result));
        return false;
    }
    return true;
}

void SuperResolution::release_feature() noexcept
{
    if (feature_ != nullptr) {
        static_cast<void>(NVSDK_NGX_D3D11_ReleaseFeature(feature_));
        feature_ = nullptr;
    }
    feature_mode_ = config::UpscalingMode::off;
    feature_render_width_ = 0;
    feature_render_height_ = 0;
    feature_output_width_ = 0;
    feature_output_height_ = 0;
}

void SuperResolution::release_ngx() noexcept
{
    release_feature();
    if (feature_parameters_ != nullptr) {
        static_cast<void>(
            NVSDK_NGX_D3D11_DestroyParameters(feature_parameters_));
        feature_parameters_ = nullptr;
    }
    if (capability_parameters_ != nullptr) {
        static_cast<void>(NVSDK_NGX_D3D11_DestroyParameters(
            capability_parameters_));
        capability_parameters_ = nullptr;
    }
    if (ngx_initialized_) {
        static_cast<void>(NVSDK_NGX_D3D11_Shutdown1(device_));
        ngx_initialized_ = false;
    }
}

void SuperResolution::shutdown() noexcept
{
    release_ngx();
    if (context_ != nullptr) {
        context_->Release();
        context_ = nullptr;
    }
    if (device_ != nullptr) {
        device_->Release();
        device_ = nullptr;
    }
    render_width_ = 0;
    render_height_ = 0;
    output_width_ = 0;
    output_height_ = 0;
    provider_ = providers::Vendor::none;
    mode_ = config::UpscalingMode::off;
    ready_ = false;
}

bool SuperResolution::ready() const noexcept
{
    return ready_;
}

bool SuperResolution::enabled() const noexcept
{
    return ready_ && mode_ != config::UpscalingMode::off &&
           (provider_ != providers::Vendor::nvidia || feature_ != nullptr);
}

providers::Vendor SuperResolution::provider() const noexcept
{
    return provider_;
}

config::UpscalingMode SuperResolution::mode() const noexcept
{
    return mode_;
}

std::uint32_t SuperResolution::render_width() const noexcept
{
    return render_width_;
}

std::uint32_t SuperResolution::render_height() const noexcept
{
    return render_height_;
}

std::uint32_t SuperResolution::output_width() const noexcept
{
    return output_width_;
}

std::uint32_t SuperResolution::output_height() const noexcept
{
    return output_height_;
}

float SuperResolution::render_scale() const noexcept
{
    if (!enabled() || output_width_ == 0 || output_height_ == 0) {
        return 1.0F;
    }
    return (std::max)(
        static_cast<float>(render_width_) /
            static_cast<float>(output_width_),
        static_cast<float>(render_height_) /
            static_cast<float>(output_height_));
}
}
