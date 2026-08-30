#include "streamline/FeatureSupport.hpp"

#include "streamline/StreamlineApi.hpp"

#include <SKSE/SKSE.h>

#include <dxgiformat.h>

#include <sl_dlss.h>
#include <sl_dlss_g.h>

#include <array>
#include <string>
#include <string_view>

namespace mfgdlss::streamline
{
namespace
{
namespace logger = SKSE::log;

const sl::ViewportHandle kMainViewport{0};

struct UpscalingMode
{
    sl::DLSSMode mode;
    std::string_view name;
};

constexpr std::array kUpscalingModes{
    UpscalingMode{sl::DLSSMode::eMaxQuality, "Quality"},
    UpscalingMode{sl::DLSSMode::eBalanced, "Balanced"},
    UpscalingMode{sl::DLSSMode::eMaxPerformance, "Performance"},
    UpscalingMode{sl::DLSSMode::eUltraPerformance, "Ultra Performance"}};

template <class Function>
[[nodiscard]] Function* load_feature_function(
    const sl::Feature feature,
    const char* name)
{
    void* function{};
    const auto result =
        Api::instance().get_feature_function(feature, name, function);
    if (result != sl::Result::eOk || function == nullptr) {
        logger::error(
            "Unable to load Streamline feature function {}: {}",
            name,
            static_cast<int>(result));
        return nullptr;
    }
    return reinterpret_cast<Function*>(function);
}

void apply_dlss_45_presets(sl::DLSSOptions& options) noexcept
{
    options.dlaaPreset = sl::DLSSPreset::ePresetK;
    options.qualityPreset = sl::DLSSPreset::ePresetK;
    options.balancedPreset = sl::DLSSPreset::ePresetK;
    options.performancePreset = sl::DLSSPreset::ePresetM;
    options.ultraPerformancePreset = sl::DLSSPreset::ePresetL;
}
}

FeatureSupport& FeatureSupport::instance() noexcept
{
    static FeatureSupport support;
    return support;
}

bool FeatureSupport::initialize(
    const std::uint32_t output_width,
    const std::uint32_t output_height)
{
    if (ready_) {
        return true;
    }
    if (!Api::instance().initialized() ||
        output_width == 0 ||
        output_height == 0) {
        return false;
    }

    auto* get_dlss_settings =
        load_feature_function<PFun_slDLSSGetOptimalSettings>(
            sl::kFeatureDLSS,
            "slDLSSGetOptimalSettings");
    auto* get_dlssg_state =
        load_feature_function<PFun_slDLSSGGetState>(
            sl::kFeatureDLSS_G,
            "slDLSSGGetState");
    if (get_dlss_settings == nullptr || get_dlssg_state == nullptr) {
        return false;
    }

    sl::DLSSGState frame_generation_state{};
    const auto frame_generation_result =
        get_dlssg_state(
            kMainViewport,
            frame_generation_state,
            nullptr);
    if (frame_generation_result != sl::Result::eOk) {
        logger::error(
            "slDLSSGGetState failed: {}",
            static_cast<int>(frame_generation_result));
        return false;
    }

    maximum_multiplier_ =
        frame_generation_state.numFramesToGenerateMax + 1;
    dynamic_mfg_supported_ =
        frame_generation_state.bIsDynamicMFGSupported == sl::Boolean::eTrue;

    estimated_vram_bytes_ = 0;
    refresh_vram_estimate(output_width, output_height);

    logger::info(
        "DLSS MFG capability: fixed 2x-{}x, dynamic={}, estimated VRAM at the "
        "maximum multiplier {}",
        maximum_multiplier_,
        dynamic_mfg_supported_,
        estimated_vram_bytes_ == 0ULL ?
            std::string{"was not reported by the runtime"} :
            std::to_string(estimated_vram_bytes_ / (1024ULL * 1024ULL)) +
                " MiB");

    for (const auto& mode : kUpscalingModes) {
        sl::DLSSOptions options{};
        options.mode = mode.mode;
        options.outputWidth = output_width;
        options.outputHeight = output_height;
        options.colorBuffersHDR = sl::Boolean::eTrue;
        apply_dlss_45_presets(options);

        sl::DLSSOptimalSettings settings{};
        const auto result = get_dlss_settings(options, settings);
        if (result != sl::Result::eOk) {
            logger::warn(
                "DLSS SR {} settings query failed: {}",
                mode.name,
                static_cast<int>(result));
            continue;
        }

        logger::info(
            "DLSS SR {} at {}x{}: render {}x{} (range {}x{}-{}x{})",
            mode.name,
            output_width,
            output_height,
            settings.optimalRenderWidth,
            settings.optimalRenderHeight,
            settings.renderWidthMin,
            settings.renderHeightMin,
            settings.renderWidthMax,
            settings.renderHeightMax);
    }

    ready_ = true;
    return true;
}

void FeatureSupport::refresh_vram_estimate(
    const std::uint32_t output_width,
    const std::uint32_t output_height) noexcept
{
    if (maximum_multiplier_ <= 1U || output_width == 0 || output_height == 0) {
        return;
    }
    auto* get_dlssg_state =
        load_feature_function<PFun_slDLSSGGetState>(
            sl::kFeatureDLSS_G,
            "slDLSSGGetState");
    if (get_dlssg_state == nullptr) {
        return;
    }

    sl::DLSSGOptions estimate_options{};
    estimate_options.mode = sl::DLSSGMode::eOn;
    estimate_options.numFramesToGenerate = maximum_multiplier_ - 1U;
    estimate_options.flags = sl::DLSSGFlags::eRequestVRAMEstimate;
    estimate_options.numBackBuffers = 3U;
    estimate_options.colorWidth = output_width;
    estimate_options.colorHeight = output_height;
    estimate_options.mvecDepthWidth = output_width;
    estimate_options.mvecDepthHeight = output_height;
    estimate_options.colorBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    estimate_options.mvecBufferFormat = DXGI_FORMAT_R16G16_FLOAT;
    estimate_options.depthBufferFormat = DXGI_FORMAT_R32_FLOAT;
    estimate_options.hudLessBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    estimate_options.uiBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    sl::DLSSGState estimate_state{};
    if (get_dlssg_state(kMainViewport, estimate_state, &estimate_options) !=
        sl::Result::eOk) {
        return;
    }
    if (estimate_state.estimatedVRAMUsageInBytes == estimated_vram_bytes_) {
        return;
    }
    estimated_vram_bytes_ = estimate_state.estimatedVRAMUsageInBytes;
    logger::info(
        "DLSS frame generation estimates {} MiB of VRAM at {}x{} and the "
        "maximum multiplier",
        estimated_vram_bytes_ / (1024ULL * 1024ULL),
        output_width,
        output_height);
}

void FeatureSupport::shutdown() noexcept
{
    ready_ = false;
    maximum_multiplier_ = 1;
    estimated_vram_bytes_ = 0;
    dynamic_mfg_supported_ = false;
}

bool FeatureSupport::ready() const noexcept
{
    return ready_;
}

std::uint32_t FeatureSupport::maximum_multiplier() const noexcept
{
    return maximum_multiplier_;
}

std::uint64_t FeatureSupport::estimated_vram_bytes() const noexcept
{
    return estimated_vram_bytes_;
}

bool FeatureSupport::dynamic_mfg_supported() const noexcept
{
    return dynamic_mfg_supported_;
}
}
