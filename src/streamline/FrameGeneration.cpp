#include "render/ContractSnapshot.hpp"

#include "streamline/FrameGeneration.hpp"

#include "config/Settings.hpp"
#include "render/DynamicResolution.hpp"
#include "render/MultiplierProjection.hpp"
#include "render/FsrFrameGeneration.hpp"
#include "render/SharedResources.hpp"
#include "render/XessFrameGeneration.hpp"
#include "render/UpscalingPass.hpp"
#include "streamline/FeatureSupport.hpp"
#include "streamline/FrameSubmission.hpp"
#include "streamline/StreamlineApi.hpp"
#include "streamline/SuperResolution.hpp"

#include <dxgiformat.h>

#include <SKSE/SKSE.h>

#include <sl_dlss_g.h>
#include <sl_reflex.h>

#include <algorithm>
#include <string>

namespace mfgdlss::streamline
{
namespace
{
namespace logger = SKSE::log;

const sl::ViewportHandle kMainViewport{0};
constexpr std::uint32_t kInitialStateQueryInterval = 1;
constexpr std::uint32_t kVerifiedStateQueryInterval = 1;
constexpr std::uint32_t kFailureLimit = 120;
constexpr std::uint32_t kWaitingLogInterval = 120;

[[nodiscard]] bool reduced_temporal_extent_verified() noexcept
{
    const auto& super_resolution = SuperResolution::instance();
    const auto reduced_mode =
        super_resolution.enabled() &&
        (super_resolution.render_width() <
             super_resolution.output_width() ||
         super_resolution.render_height() <
             super_resolution.output_height());
    return !reduced_mode ||
           (render::DynamicResolution::instance()
                .scene_viewport_verified() &&
            render::UpscalingPass::instance()
                .evaluation_verified());
}

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

[[nodiscard]] sl::DLSSGOptions make_options(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t back_buffer_count,
    const std::uint32_t multiplier,
    const render::CadenceDecision& cadence,
    const bool ui_recomposition,
    const bool retain_resources_when_off,
    const std::uint32_t temporal_width = 0U,
    const std::uint32_t temporal_height = 0U) noexcept
{
    sl::DLSSGOptions options{};

    const auto dynamic =
        multiplier > 1 &&
        cadence.cadence != render::GenerationCadence::fixed_multiplier;
    options.mode =
        multiplier > 1 ?
            (dynamic ? sl::DLSSGMode::eDynamic : sl::DLSSGMode::eOn) :
            sl::DLSSGMode::eOff;

    options.numFramesToGenerate =
        multiplier > 1 ? multiplier - 1 : 1;

    options.dynamicTargetFrameRate = dynamic ? cadence.dynamic_target_fps : 0.0F;
    options.numBackBuffers = back_buffer_count;
    options.mvecDepthWidth = temporal_width != 0U ? temporal_width : width;
    options.mvecDepthHeight = temporal_height != 0U ? temporal_height : height;
    options.colorWidth = width;
    options.colorHeight = height;

    const auto& resources = render::SharedResources::instance();
    const auto color_format =
        resources.color_format() != 0 ?
            resources.color_format() :
            DXGI_FORMAT_R8G8B8A8_UNORM;
    options.colorBufferFormat = color_format;
    options.mvecBufferFormat = DXGI_FORMAT_R16G16_FLOAT;
    options.depthBufferFormat = DXGI_FORMAT_R32_FLOAT;
    options.hudLessBufferFormat = color_format;
    if (ui_recomposition) {
        options.uiBufferFormat =
            resources.ui_format() != 0 ?
                resources.ui_format() :
                DXGI_FORMAT_R8G8B8A8_UNORM;

    }
    if (retain_resources_when_off) {

        options.flags |= sl::DLSSGFlags::eRetainResourcesWhenOff;
    }
    if (multiplier > 1U &&
        config::Settings::instance().show_only_interpolated_frames()) {

        options.flags |= sl::DLSSGFlags::eShowOnlyInterpolatedFrame;
    }
    options.queueParallelismMode =
        sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
    options.enableUserInterfaceRecomposition =
        ui_recomposition ?
            sl::Boolean::eTrue :
            sl::Boolean::eFalse;
    {
        static render::ContractSnapshot dlssg{"DLSS-G options"};
        dlssg.set("mode", std::string{
            options.mode == sl::DLSSGMode::eOff ? "off" :
            options.mode == sl::DLSSGMode::eDynamic ? "dynamic" : "on"});
        dlssg.set("multiplier", multiplier);
        dlssg.set("numFramesToGenerate", options.numFramesToGenerate);
        dlssg.set("numBackBuffers", options.numBackBuffers);
        dlssg.set("color", std::to_string(options.colorWidth) + "x" +
            std::to_string(options.colorHeight));
        dlssg.set("mvecDepth", std::to_string(options.mvecDepthWidth) + "x" +
            std::to_string(options.mvecDepthHeight));
        dlssg.set("color-format", static_cast<std::uint32_t>(
            options.colorBufferFormat));
        dlssg.set("dynamic-target-fps", static_cast<std::uint32_t>(
            options.dynamicTargetFrameRate));
        dlssg.set("ui-recomposition", ui_recomposition);
        dlssg.set("retain-when-off", retain_resources_when_off);
        dlssg.set(
            "show-only-interpolated",
            (static_cast<std::uint32_t>(options.flags) &
             static_cast<std::uint32_t>(
                 sl::DLSSGFlags::eShowOnlyInterpolatedFrame)) != 0U);
        dlssg.publish();
    }
    return options;
}

[[nodiscard]] sl::ReflexMode to_streamline_mode(
    const config::ReflexMode mode) noexcept
{
    switch (mode) {
    case config::ReflexMode::off:
        return sl::ReflexMode::eOff;
    case config::ReflexMode::low_latency:
        return sl::ReflexMode::eLowLatency;
    case config::ReflexMode::low_latency_boost:
        return sl::ReflexMode::eLowLatencyWithBoost;
    }
    return sl::ReflexMode::eLowLatency;
}

[[nodiscard]] const char* reflex_mode_name(
    const config::ReflexMode mode) noexcept
{
    switch (mode) {
    case config::ReflexMode::off:
        return "Off";
    case config::ReflexMode::low_latency:
        return "On";
    case config::ReflexMode::low_latency_boost:
        return "Boost";
    }
    return "On";
}
}

FrameGeneration& FrameGeneration::instance() noexcept
{
    static FrameGeneration frame_generation;
    return frame_generation;
}

bool FrameGeneration::initialize(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t back_buffer_count)
{
    if (ready_) {
        return true;
    }
    if (!FeatureSupport::instance().ready() ||
        width == 0 ||
        height == 0 ||
        back_buffer_count < 2) {
        return false;
    }

    set_reflex_options_ =
        load_feature_function<PFun_slReflexSetOptions>(
            sl::kFeatureReflex,
            "slReflexSetOptions");
    get_reflex_state_ =
        load_feature_function<PFun_slReflexGetState>(
            sl::kFeatureReflex,
            "slReflexGetState");
    set_dlssg_options_ =
        load_feature_function<PFun_slDLSSGSetOptions>(
            sl::kFeatureDLSS_G,
            "slDLSSGSetOptions");
    get_dlssg_state_ =
        load_feature_function<PFun_slDLSSGGetState>(
            sl::kFeatureDLSS_G,
            "slDLSSGGetState");
    if (set_reflex_options_ == nullptr ||
        get_reflex_state_ == nullptr ||
        set_dlssg_options_ == nullptr ||
        get_dlssg_state_ == nullptr) {
        shutdown();
        return false;
    }

    sl::ReflexState reflex_state{};
    const auto reflex_state_result =
        reinterpret_cast<PFun_slReflexGetState*>(get_reflex_state_)(
            reflex_state);
    if (reflex_state_result != sl::Result::eOk ||
        !reflex_state.lowLatencyAvailable) {
        logger::error(
            "NVIDIA Reflex is unavailable at runtime: result={}, available={}",
            static_cast<int>(reflex_state_result),
            reflex_state.lowLatencyAvailable);
        shutdown();
        return false;
    }
    if (!FrameSubmission::instance().initialize_timing(width, height)) {
        logger::error("Reflex/PCL frame timing initialization failed");
        shutdown();
        return false;
    }

    width_ = width;
    height_ = height;
    back_buffer_count_ = back_buffer_count;
    maximum_multiplier_ =
        (std::max)(FeatureSupport::instance().maximum_multiplier(), 2U);
    output_target_fps_ = config::Settings::instance().frame_limit();
    pacing_decision_ =
        render::decide_pacing(output_target_fps_, display_refresh_hz_);
    frame_limit_ = pacing_decision_.presentation_cap_fps;
    multiplier_tracker_.reset();
    ready_ = true;
    if (!set_reflex_mode(
            config::Settings::instance().reflex_mode())) {
        shutdown();
        return false;
    }
    logger::info(
        "NVIDIA Reflex {} mode active; DLSS-G controller ready for 2x-{}x, "
        "dynamic={} (Dynamic Multi Frame Generation needs an RTX 50 series "
        "and GeForce driver 595.41 or newer)",
        reflex_mode_name(reflex_mode_),
        maximum_multiplier_,
        FeatureSupport::instance().dynamic_mfg_supported());
    return true;
}

bool FrameGeneration::set_reflex_mode(
    const config::ReflexMode mode)
{
    if (!ready_ || set_reflex_options_ == nullptr) {
        return false;
    }

    if (reflex_configured_ && reflex_mode_ == mode &&
        reflex_submitted_valid_ &&
        reflex_submitted_ == effective_reflex_mode(mode)) {

        FrameSubmission::instance().set_reflex_sleep_enabled(true);
        return true;
    }

    if (!apply_reflex_options(mode, frame_limit_, reflex_limit_divisor())) {
        return false;
    }
    reflex_mode_ = mode;
    reflex_configured_ = true;
    return true;
}

config::ReflexMode FrameGeneration::effective_reflex_mode(
    const config::ReflexMode selected) const noexcept
{

    const auto vendor_viable =
        (render::XessFrameGeneration::selected() &&
         render::XessFrameGeneration::instance().state() !=
             render::XessGenerationState::failed) ||
        (render::FsrFrameGeneration::selected() &&
         render::FsrFrameGeneration::instance().state() !=
             render::FsrGenerationState::failed);
    if (vendor_viable) {
        return config::ReflexMode::off;
    }
    const auto generation_wanted =
        multiplier_ > 1U ||
        recovery_policy_.desired_multiplier() > 1U;
    if (!generation_wanted || selected != config::ReflexMode::off) {
        return selected;
    }
    return config::ReflexMode::low_latency;
}

std::uint32_t FrameGeneration::reflex_limit_divisor() const noexcept
{
    if (multiplier_ < 2U) {
        return 1U;
    }
    if (cadence_decision_.cadence !=
        render::GenerationCadence::fixed_multiplier) {
        return 1U;
    }
    return multiplier_;
}

bool FrameGeneration::apply_reflex_options(
    const config::ReflexMode mode,
    const std::uint32_t presentation_frame_limit,
    const std::uint32_t generated_multiplier)
{
    if (!ready_ || set_reflex_options_ == nullptr) {
        return false;
    }

    const auto submitted = effective_reflex_mode(mode);
    if (submitted != mode && !reflex_raise_logged_) {
        reflex_raise_logged_ = true;
        logger::warn(
            "NVIDIA Reflex was selected as {}, but DLSS Frame Generation "
            "requires Reflex to be active (sl_dlss_g.h: "
            "eFailReflexNotDetectedAtRuntime). Submitting {} to Streamline "
            "while generation is enabled; the selection is preserved and "
            "applies in full when Frame Generation is turned off.",
            reflex_mode_name(mode),
            reflex_mode_name(submitted));
    }
    const auto divisor = generated_multiplier < 1U ? 1U : generated_multiplier;
    const auto application_frame_limit = render::reflex_limit_preserving_base(
        presentation_frame_limit,
        config::Settings::instance().base_frame_limit(),
        divisor);

    sl::ReflexOptions options{};
    options.mode = to_streamline_mode(submitted);
    options.frameLimitUs =
        application_frame_limit == 0U ?
            0U :
            static_cast<std::uint32_t>(
                (1'000'000ULL + application_frame_limit / 2U) /
                application_frame_limit);
    const auto result =
        reinterpret_cast<PFun_slReflexSetOptions*>(
            set_reflex_options_)(options);
    if (result != sl::Result::eOk) {
        logger::error(
            "slReflexSetOptions for {} failed: {}",
            reflex_mode_name(mode),
            static_cast<int>(result));
        return false;
    }

    FrameSubmission::instance().set_reflex_sleep_enabled(true);
    reflex_submitted_ = submitted;
    reflex_submitted_valid_ = true;
    reflex_effective_limit_ = application_frame_limit;
    {
        static std::uint32_t warned_limit = 0U;
        static std::uint32_t warned_refresh = 0U;
        const auto matches_refresh =
            display_refresh_hz_ != 0U &&
            application_frame_limit == display_refresh_hz_;
        if (matches_refresh &&
            (warned_limit != application_frame_limit ||
             warned_refresh != display_refresh_hz_)) {
            warned_limit = application_frame_limit;
            warned_refresh = display_refresh_hz_;
            logger::warn(
                "OUTPUT RATE EQUALS DISPLAY REFRESH at {} FPS into {} "
                "Hz ({}x generation, base cap {}). If the NVIDIA Control "
                "Panel is forcing V-Sync off, the frame tears, and a tear "
                "drifts at the difference between output and refresh. At "
                "zero difference it stops drifting and sits as a NEAR "
                "STATIONARY BAND, usually read as smearing or a woven "
                "pattern in foliage rather than as tearing. CONFIRMED ON "
                "HARDWARE at tick 304: 240 into 240 Hz showed it at 6x, 4x "
                "and 3x alike, while the same 6x at 270, 360 and 480 was "
                "clean. Output is base cap times multiplier whenever a base "
                "cap is set, so a higher output cap does not move it. If you "
                "see it, either move the base cap so the output is not {} "
                "Hz, or set Vertical sync to Use the 3D application setting "
                "in the NVIDIA Control Panel so the V-Sync this plugin "
                "already requests actually engages",
                application_frame_limit,
                display_refresh_hz_,
                divisor,
                config::Settings::instance().base_frame_limit(),
                display_refresh_hz_);
        }
    }
    logger::info(
        "NVIDIA Reflex: selected={}, submitted={}, output cap {} raised to a "
        "submitted target of {} because {}x generation is active and Streamline "
        "divides this limit by the presented-frame count itself. MEASURED "
        "2026-08-21 in Riverwood: submitting the raw 240 cap at 6x paced the "
        "base at 40 FPS, spacing rendered frames 25ms apart and stretching "
        "five interpolations across the gap, which was the reported ghosting. "
        "Submitting base times multiplier held the base at 60 with 358 "
        "displayed and the ghosting disappeared. 4x never showed the fault "
        "because 240 already equals its base times multiplier.",
        reflex_mode_name(mode),
        reflex_mode_name(submitted),
        presentation_frame_limit == 0U ?
            std::string{"uncapped"} :
            std::to_string(presentation_frame_limit) + " FPS",
        application_frame_limit == 0U ?
            std::string{"uncapped"} :
            std::to_string(application_frame_limit) + " FPS",
        divisor);
    return true;
}

bool FrameGeneration::set_output_target_fps(
    const std::uint32_t output_fps)
{
    if (!ready_ || set_reflex_options_ == nullptr) {
        return false;
    }
    const auto decision =
        render::decide_pacing(output_fps, display_refresh_hz_);
    const auto wanted_effective_limit = render::reflex_limit_preserving_base(
        decision.presentation_cap_fps,
        config::Settings::instance().base_frame_limit(),
        reflex_limit_divisor());
    if (reflex_configured_ &&
        frame_limit_ == decision.presentation_cap_fps &&
        reflex_effective_limit_ == wanted_effective_limit) {
        output_target_fps_ = output_fps;
        pacing_decision_ = decision;
        return true;
    }
    if (!apply_reflex_options(
            reflex_mode_,
            decision.presentation_cap_fps,
            reflex_limit_divisor())) {
        return false;
    }
    output_target_fps_ = output_fps;
    frame_limit_ = decision.presentation_cap_fps;
    pacing_decision_ = decision;
    reflex_configured_ = true;
    logger::info(
        "Pacing: requested output {} -> final Streamline presentation "
        "limit {} ({})",
        output_fps == 0 ?
            std::string{"uncapped"} :
            std::to_string(output_fps) + " FPS",
        frame_limit_ == 0 ?
            std::string{"uncapped"} :
            std::to_string(frame_limit_) + " FPS",
        render::describe(decision.reason));
    return true;
}

void FrameGeneration::note_display_refresh(
    const std::uint32_t refresh_hz) noexcept
{
    if (display_refresh_hz_ == refresh_hz) {
        return;
    }
    display_refresh_hz_ = refresh_hz;

    pacing_refresh_dirty_ = true;
    logger::info(
        "Display refresh reported as {} Hz; pacing will be re-derived at the "
        "next frame boundary (Output FPS Off means paced to this rate, not "
        "unpaced)",
        refresh_hz);
}

bool FrameGeneration::enable(const std::uint32_t multiplier)
{

    if (ready_ && pacing_refresh_dirty_) {
        pacing_refresh_dirty_ = false;
        static_cast<void>(set_output_target_fps(output_target_fps_));
    }

    const auto xess_viable = render::XessFrameGeneration::selected() &&
        render::XessFrameGeneration::instance().state() !=
            render::XessGenerationState::failed;
    const auto fsr_viable = render::FsrFrameGeneration::selected() &&
        render::FsrFrameGeneration::instance().state() !=
            render::FsrGenerationState::failed;
    if (xess_viable || fsr_viable) {
        if (!vendor_generator_logged_) {
            vendor_generator_logged_ = true;
            logger::info(
                "DLSS-G is not being configured: VendorFrameGeneration "
                "selects {}, which owns presentation instead",
                xess_viable ?
                    "Intel XeSS-FG" : "AMD FidelityFX frame generation");
        }
        return false;
    }
    if ((render::XessFrameGeneration::selected() ||
         render::FsrFrameGeneration::selected()) &&
        !vendor_fallback_logged_) {
        vendor_fallback_logged_ = true;

        const std::string& reason =
            render::XessFrameGeneration::selected() ?
                render::XessFrameGeneration::instance().detail() :
                render::FsrFrameGeneration::instance().detail();

        if (ready_) {
            logger::warn(
                "The selected vendor frame generator could not install, so "
                "DLSS-G is being configured instead rather than leaving this "
                "session with no frame generation. Reason: {}",
                reason);
        } else {
            logger::error(
                "The selected vendor frame generator could not install, and "
                "DLSS-G cannot stand in for it: selecting a vendor generator "
                "disables Streamline's presentation proxy, which DLSS-G "
                "requires. This session has no frame generation. Set "
                "[FrameGeneration] VendorFrameGeneration=Off to use DLSS-G. "
                "Reason: {}",
                reason);
        }
    }
    if (!ready_ || multiplier < 2) {
        return false;
    }
    if (multiplier > maximum_multiplier_) {
        if (refused_multiplier_ != multiplier) {
            refused_multiplier_ = multiplier;
            logger::warn(
                "DLSS frame generation refused {}x because this GPU and driver "
                "report a ceiling of {}x. Frame generation stays off until a "
                "supported multiplier is selected.",
                multiplier,
                maximum_multiplier_);
        }
        return false;
    }
    refused_multiplier_ = 0U;

    recovery_policy_.remember_multiplier(multiplier);
    if (recovery_policy_.suspended()) {

        return true;
    }
    const auto ui_recomposition =
        render::SharedResources::instance()
            .streamline_ui_recomposition_available();
    const auto& super_resolution = SuperResolution::instance();
    const auto temporal_width =
        super_resolution.enabled() &&
                reduced_temporal_extent_verified() ?
            super_resolution.render_width() :
            width_;
    const auto temporal_height =
        super_resolution.enabled() &&
                reduced_temporal_extent_verified() ?
            super_resolution.render_height() :
            height_;
    const auto& settings = config::Settings::instance();
    const auto cadence = render::decide_cadence(
        settings.multiplier_mode() == config::MultiplierMode::dynamic,
        FeatureSupport::instance().dynamic_mfg_supported(),
        multiplier,
        frame_limit_);
    if (cadence.dynamic_unavailable && !dynamic_unavailable_logged_) {
        dynamic_unavailable_logged_ = true;

        logger::warn(
            "[FrameGeneration] MultiplierMode=Dynamic was configured, but this "
            "runtime reports Dynamic Multi Frame Generation unsupported "
            "(DLSSGState::bIsDynamicMFGSupported is false). NVIDIA lists the "
            "causes as: multi frame generation unsupported, GeForce driver "
            "below 595.41, or Vulkan. Falling back to a fixed {}x; the "
            "selection is kept and takes effect if the runtime later reports "
            "support.",
            multiplier);
    }
    if (multiplier_ == multiplier &&
        ui_recomposition_enabled_ == ui_recomposition &&
        temporal_width_ == temporal_width &&
        temporal_height_ == temporal_height &&
        cadence_decision_.cadence == cadence.cadence &&
        cadence_decision_.dynamic_target_fps == cadence.dynamic_target_fps) {
        return true;
    }
    const auto active_contract_changed = multiplier_ > 1U;

    const auto options = make_options(
        width_,
        height_,
        back_buffer_count_,
        multiplier,
        cadence,
        ui_recomposition,
        true,
        temporal_width,
        temporal_height);
    const auto result =
        reinterpret_cast<PFun_slDLSSGSetOptions*>(set_dlssg_options_)(
            kMainViewport,
            options);
    if (result != sl::Result::eOk) {
        logger::error(
            "slDLSSGSetOptions for {}x failed: {}",
            multiplier,
            static_cast<int>(result));
        return false;
    }

    if (!apply_reflex_options(
            config::Settings::instance().reflex_mode(),
            frame_limit_,
            cadence.cadence == render::GenerationCadence::fixed_multiplier ?
                multiplier :
                1U)) {
        const auto disabled = make_options(
            width_,
            height_,
            back_buffer_count_,
            1,
            render::CadenceDecision{},
            ui_recomposition,
            true,
            temporal_width,
            temporal_height);
        const auto disable_result =
            reinterpret_cast<PFun_slDLSSGSetOptions*>(
                set_dlssg_options_)(kMainViewport, disabled);
        if (disable_result != sl::Result::eOk) {
            logger::error(
                "DLSS-G rollback after Reflex failure failed: {}",
                static_cast<int>(disable_result));
        }
        multiplier_ = 1;
        temporal_width_ = 0;
        temporal_height_ = 0;
        ui_recomposition_enabled_ = ui_recomposition;
        resources_retained_ = true;
        cadence_decision_ = render::CadenceDecision{};
        return false;
    }
    reflex_mode_ = config::Settings::instance().reflex_mode();
    reflex_configured_ = true;

    multiplier_ = multiplier;
    temporal_width_ = temporal_width;
    temporal_height_ = temporal_height;
    ui_recomposition_enabled_ = ui_recomposition;
    resources_retained_ = false;
    presents_since_state_query_ = 0;
    unsuccessful_state_queries_ = 0;
    waiting_present_count_ = 0;
    actual_presents_ = 1;
    generated_frames_verified_ = false;
    cadence_decision_ = cadence;

    if (active_contract_changed) {
        FrameSubmission::instance().request_history_reset();
    }
    logger::info(
        "DLSS Multi Frame Generation configured: {}, cadence={}, target={}, "
        "temporal-input={}x{}, UI recomposition={}, fullscreen-menu "
        "detection=manual",
        cadence_decision_.cadence ==
                render::GenerationCadence::fixed_multiplier ?
            std::string{"fixed "} + std::to_string(multiplier_) + "x (" +
                std::to_string(multiplier_ - 1) + " generated frames)" :
            std::string{"the runtime chooses the multiplier; the configured "} +
                std::to_string(multiplier_) +
                "x is IGNORED while dynamic is active",
        render::describe(cadence_decision_.cadence),
        cadence_decision_.cadence ==
                render::GenerationCadence::fixed_multiplier ?
            std::string{"n/a"} :
            (cadence_decision_.dynamic_target_fps == 0.0F ?
                 std::string{"auto-detected display refresh"} :
                 std::to_string(
                     static_cast<std::uint32_t>(
                         cadence_decision_.dynamic_target_fps)) + " FPS"),
        temporal_width_,
        temporal_height_,
        ui_recomposition_enabled_);
    return true;
}

bool FrameGeneration::suspend(const char* const reason)
{
    suspension_reason_ = reason != nullptr ? reason : "an unrecorded reason";
    return suspend_impl(
        true,
        FrameGenerationSuspensionCause::unhealthy_frame);
}

bool FrameGeneration::suspend_for_gate(
    const std::uint32_t desired_multiplier,
    const FrameGenerationGateReason reason)
{
    if (desired_multiplier >= 2U &&
        desired_multiplier <= maximum_multiplier_) {
        recovery_policy_.remember_multiplier(desired_multiplier);
    }
    gate_reason_ = reason;
    return suspend_impl(
        true,
        FrameGenerationSuspensionCause::deterministic_gate);
}

bool FrameGeneration::resume_from_gate(
    const std::uint32_t desired_multiplier)
{
    if (!ready_ || !recovery_policy_.deterministic_gate_active()) {
        return false;
    }
    if (desired_multiplier >= 2U &&
        desired_multiplier <= maximum_multiplier_) {
        recovery_policy_.remember_multiplier(desired_multiplier);
    }

    const auto multiplier = recovery_policy_.desired_multiplier();
    const auto reason = gate_reason_;
    recovery_policy_.mark_active();
    gate_grace_frames_ = 0U;
    if (multiplier < 2U || !enable(multiplier)) {
        static_cast<void>(recovery_policy_.enter_unhealthy_frame());
        logger::warn(
            "DLSS-G could not resume at {}x after the {} gate; guarded "
            "bad-frame recovery is now active",
            multiplier,
            gate_reason_name(reason));
        return false;
    }

    logger::info(
        "DLSS-G resumed immediately at {}x on the first complete gameplay "
        "frame after the {} gate",
        multiplier,
        gate_reason_name(reason));
    return true;
}

bool FrameGeneration::gate_suspended() const noexcept
{
    return recovery_policy_.deterministic_gate_active();
}

bool FrameGeneration::generation_suspended() const noexcept
{
    return recovery_policy_.suspended();
}

std::uint64_t FrameGeneration::suspended_frames() const noexcept
{
    return recovery_policy_.suspended() ? suspended_frames_ : 0ULL;
}

const char* FrameGeneration::suspension_cause_name() const noexcept
{
    switch (recovery_policy_.cause()) {
    case FrameGenerationSuspensionCause::deterministic_gate:
        return gate_reason_name(gate_reason_);
    case FrameGenerationSuspensionCause::unhealthy_frame:
        return suspension_reason_;
    case FrameGenerationSuspensionCause::none:
    default:
        return "not suspended";
    }
}

bool FrameGeneration::suspend_for_reset(const char* const reason)
{
    suspension_reason_ =
        reason != nullptr ? reason : "an unrecorded renderer reset";
    return suspend_impl(
        false,
        FrameGenerationSuspensionCause::unhealthy_frame);
}

const char* FrameGeneration::gate_reason_name(
    const FrameGenerationGateReason reason) noexcept
{
    switch (reason) {
    case FrameGenerationGateReason::plugin_menu:
        return "plugin menu";
    case FrameGenerationGateReason::game_paused:
        return "Skyrim pause";
    case FrameGenerationGateReason::plugin_menu_and_game_paused:
        return "plugin menu and Skyrim pause";
    }
    return "deterministic menu/pause";
}

bool FrameGeneration::suspend_impl(
    const bool retain_resources,
    const FrameGenerationSuspensionCause cause)
{
    if (!ready_) {
        return true;
    }
    constexpr std::uint32_t kGateGraceFrames{30U};
    const auto previous_cause = recovery_policy_.cause();
    const auto was_suspended = recovery_policy_.suspended();
    auto held_by_grace = false;
    if (cause == FrameGenerationSuspensionCause::deterministic_gate) {
        static_cast<void>(recovery_policy_.enter_deterministic_gate());
        gate_grace_frames_ = 0U;
    } else if (
        previous_cause !=
            FrameGenerationSuspensionCause::unhealthy_frame &&
        gate_grace_frames_ < kGateGraceFrames) {
        ++gate_grace_frames_;
        held_by_grace = true;
        static_cast<void>(recovery_policy_.enter_deterministic_gate());
        if (gate_grace_frames_ == 1U) {
            logger::info(
                "A gameplay frame was not complete while generation was NOT "
                "already in guarded suspension. This is the ORDINARY warm-up "
                "state after a save load, a menu, a pause or a history reset, "
                "when the upscaling pipeline has not yet produced a pre-UI "
                "full-resolution presentation. It is being held in the "
                "resumable gate for up to {} such frames instead of "
                "escalating to guarded suspension, so generation resumes on "
                "the next complete frame instead of serving 60 to 900 frames. "
                "MEASURED 2026-08-29: save loads cost about 5 seconds of lost "
                "generation each through this path",
                kGateGraceFrames);
        }
    } else {
        static_cast<void>(recovery_policy_.enter_unhealthy_frame());
    }

    if (was_suspended) {
        report_persistent_suspension(cause);
        if (cause == FrameGenerationSuspensionCause::unhealthy_frame &&
            previous_cause !=
                FrameGenerationSuspensionCause::unhealthy_frame &&
            !held_by_grace) {
            logger::warn(
                "DLSS-G deterministic gate escalated to guarded suspension "
                "because the next gameplay frame was STILL invalid after the "
                "grace window. This is now a real fault rather than warm-up");
        }

        if (!retain_resources && resources_retained_) {
            const auto options = make_options(
                width_,
                height_,
                back_buffer_count_,
                1,
                render::CadenceDecision{},
                false,
                false,
                temporal_width_ != 0U ? temporal_width_ : width_,
                temporal_height_ != 0U ? temporal_height_ : height_);
            const auto result =
                reinterpret_cast<PFun_slDLSSGSetOptions*>(
                    set_dlssg_options_)(kMainViewport, options);
            if (result != sl::Result::eOk) {
                logger::error(
                    "Unable to release retained DLSS-G resources before "
                    "swap-chain mutation: {}",
                    static_cast<int>(result));
                return false;
            }
            resources_retained_ = false;
            ui_recomposition_enabled_ = false;
            logger::info(
                "Retained DLSS-G resources released before renderer reset");
        }
        return true;
    }
    suspended_frames_ = 1ULL;
    const auto previous_multiplier = multiplier_;
    if (multiplier_ <= 1) {
        const auto reportable = effective_desired_multiplier();
        if (cause == FrameGenerationSuspensionCause::deterministic_gate) {
            logger::info(
                "DLSS-G gated by {} before its first active frame; the "
                "configured {}x will resume on the first complete gameplay "
                "frame",
                gate_reason_name(gate_reason_),
                reportable);
        } else {
            logger::warn(
                "DLSS-G guarded suspension armed before generation became "
                "active because {}; {} healthy gameplay frames are required "
                "before the configured {}x is requested",
                suspension_reason_,
                FrameGenerationRecoveryPolicy::kGuardedRecoveryFrames,
                reportable);
        }
        return true;
    }

    const auto options = make_options(
        width_,
        height_,
        back_buffer_count_,
        1,
        render::CadenceDecision{},
        retain_resources ? ui_recomposition_enabled_ : false,
        retain_resources,
        temporal_width_ != 0U ? temporal_width_ : width_,
        temporal_height_ != 0U ? temporal_height_ : height_);
    const auto result =
        reinterpret_cast<PFun_slDLSSGSetOptions*>(set_dlssg_options_)(
            kMainViewport,
            options);
    if (result != sl::Result::eOk) {
        logger::error(
            "Unable to suspend DLSS-G (retain-resources={}): {}",
            retain_resources,
            static_cast<int>(result));

        recovery_policy_.mark_active();
        gate_grace_frames_ = 0U;
        return false;
    }

    if (cause == FrameGenerationSuspensionCause::deterministic_gate) {
        logger::info(
            "DLSS-G gated by {} (previously {}x, configured {}x, resources "
            "retained); it will resume on the first complete gameplay frame",
            gate_reason_name(gate_reason_),
            previous_multiplier,
            recovery_policy_.desired_multiplier());
    } else if (retain_resources) {
        logger::warn(
            "DLSS-G suspended after an invalid frame (previously {}x, "
            "resources retained); guarded recovery requires {} consecutive "
            "complete gameplay frames",
            previous_multiplier,
            FrameGenerationRecoveryPolicy::kGuardedRecoveryFrames);
    } else {
        logger::info(
            "DLSS-G suspended for a renderer reset or long-term disable "
            "(previously {}x, resources released)",
            previous_multiplier);
    }
    if (!apply_reflex_options(reflex_mode_, frame_limit_, 1U)) {
        logger::error(
            "Unable to restore 1x Reflex pacing after suspending DLSS-G");
    }
    multiplier_ = 1;
    temporal_width_ = 0;
    temporal_height_ = 0;
    presents_since_state_query_ = 0;
    unsuccessful_state_queries_ = 0;
    waiting_present_count_ = 0;
    actual_presents_ = 1;
    if (!retain_resources) {
        ui_recomposition_enabled_ = false;
    }
    resources_retained_ = retain_resources;
    generated_frames_verified_ = false;
    return true;
}

void FrameGeneration::report_persistent_suspension(
    const FrameGenerationSuspensionCause requested)
{
    static_cast<void>(requested);
    ++suspended_frames_;
    const auto gated =
        recovery_policy_.cause() ==
        FrameGenerationSuspensionCause::deterministic_gate;
    const auto due = gated ?
        (suspended_frames_ == 6000ULL ||
         suspended_frames_ % 60000ULL == 0ULL) :
        (suspended_frames_ == 60ULL ||
         suspended_frames_ == 600ULL ||
         suspended_frames_ == 6000ULL ||
         suspended_frames_ % 60000ULL == 0ULL);
    if (!due) {
        return;
    }
    if (gated) {
        logger::info(
            "Frame generation has been held by a deterministic gate for {} "
            "consecutive frames: {}. This is normal while a menu is open or "
            "the game is paused; the configured {}x resumes on the first "
            "complete gameplay frame",
            suspended_frames_,
            gate_reason_name(gate_reason_),
            effective_desired_multiplier());
        return;
    }
    logger::warn(
        "Frame generation has been suspended for {} consecutive frames by an "
        "unhealthy presentation frame: {}. The configured {}x is not running",
        suspended_frames_,
        suspension_reason_,
        effective_desired_multiplier());
}

std::uint32_t FrameGeneration::effective_desired_multiplier() const noexcept
{
    const auto remembered = recovery_policy_.desired_multiplier();
    if (remembered >= 2U) {
        return remembered;
    }
    return config::Settings::instance().frame_generation_multiplier();
}

void FrameGeneration::report_idle_generation()
{
    if (multiplier_ >= 2U || recovery_policy_.suspended()) {
        idle_frames_ = 0ULL;
        return;
    }
    const auto configured = effective_desired_multiplier();
    if (configured < 2U) {
        idle_frames_ = 0ULL;
        return;
    }
    ++idle_frames_;
    if (idle_frames_ != 600ULL && idle_frames_ % 60000ULL != 0ULL) {
        return;
    }
    logger::warn(
        "Frame generation is configured at {}x but has not been active for "
        "{} healthy frames, and nothing is suspending it now",
        configured,
        idle_frames_);
}

void FrameGeneration::note_presentation_healthy()
{
    if (!ready_) {
        return;
    }
    if (!recovery_policy_.guarded_failure_active()) {
        report_idle_generation();
        return;
    }
    idle_frames_ = 0ULL;

    if (!recovery_policy_.note_healthy_frame()) {
        return;
    }

    logger::info(
        "Presentation healthy for {} consecutive frames; DLSS-G recovery "
        "is eligible and the configured {}x will be requested at the next "
        "frame boundary",
        FrameGenerationRecoveryPolicy::kGuardedRecoveryFrames,
        effective_desired_multiplier());
}

bool FrameGeneration::resume_if_eligible()
{
    if (!ready_ ||
        !recovery_policy_.guarded_recovery_eligible()) {
        return false;
    }
    const auto desired_multiplier = effective_desired_multiplier();
    const auto was_forced = recovery_policy_.recovery_was_forced();
    recovery_policy_.mark_active();
    gate_grace_frames_ = 0U;
    if (desired_multiplier < 2U) {
        return false;
    }
    if (!enable(desired_multiplier)) {
        logger::warn(
            "DLSS-G could not be restored at {}x; it stays suspended until the "
            "next healthy run",
            desired_multiplier);
        static_cast<void>(recovery_policy_.enter_unhealthy_frame());
        return false;
    }
    if (was_forced) {
        logger::info(
            "DLSS-G restored at the configured {}x by the FORCED RECOVERY "
            "WATCHDOG, without ever completing a clean {} frame run. This is "
            "the path that used to be a permanent death: repeated HUD "
            "coverage rejections reset the consecutive counter faster than it "
            "could fill, so generation never came back until an alt-tab "
            "rebuilt the device state",
            desired_multiplier,
            FrameGenerationRecoveryPolicy::kGuardedRecoveryFrames);
    } else {
        logger::info(
            "DLSS-G restored at the configured {}x after a clean {} frame "
            "healthy presentation run, which is the ordinary recovery path "
            "rather than the watchdog",
            desired_multiplier,
            FrameGenerationRecoveryPolicy::kGuardedRecoveryFrames);
    }
    return true;
}

bool FrameGeneration::reconfigure(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t back_buffer_count) noexcept
{
    if (!ready_ ||
        enabled() ||
        width == 0 ||
        height == 0 ||
        back_buffer_count < 2) {
        return false;
    }

    if (resources_retained_) {
        const auto options = make_options(
            width_,
            height_,
            back_buffer_count_,
            1,
            render::CadenceDecision{},
            false,
            false,
            temporal_width_ != 0U ? temporal_width_ : width_,
            temporal_height_ != 0U ? temporal_height_ : height_);
        const auto result =
            reinterpret_cast<PFun_slDLSSGSetOptions*>(
                set_dlssg_options_)(kMainViewport, options);
        if (result != sl::Result::eOk) {
            logger::error(
                "Unable to release retained DLSS-G resources before "
                "controller reconfiguration: {}",
                static_cast<int>(result));
            return false;
        }
        resources_retained_ = false;
        logger::info(
            "Retained DLSS-G resources released before controller "
            "reconfiguration");
    }

    width_ = width;
    height_ = height;
    back_buffer_count_ = back_buffer_count;
    temporal_width_ = 0;
    temporal_height_ = 0;
    presents_since_state_query_ = 0;
    unsuccessful_state_queries_ = 0;
    waiting_present_count_ = 0;
    actual_presents_ = 1;
    ui_recomposition_enabled_ = false;
    generated_frames_verified_ = false;
    logger::info(
        "DLSS-G controller reconfigured for {}x{}, buffers={}; "
        "the configured multiplier will resume after resources rebuild",
        width_,
        height_,
        back_buffer_count_);
    return true;
}

void FrameGeneration::after_present()
{
    if (!enabled()) {
        return;
    }

    ++presents_since_state_query_;
    const auto interval = generated_frames_verified_ ?
                              kVerifiedStateQueryInterval :
                              kInitialStateQueryInterval;
    if (presents_since_state_query_ < interval) {
        return;
    }
    presents_since_state_query_ = 0;

    const auto options = make_options(
        width_,
        height_,
        back_buffer_count_,
        multiplier_,
        cadence_decision_,
        ui_recomposition_enabled_,
        true,
        temporal_width_ != 0U ? temporal_width_ : width_,
        temporal_height_ != 0U ? temporal_height_ : height_);
    sl::DLSSGState state{};
    const auto result =
        reinterpret_cast<PFun_slDLSSGGetState*>(get_dlssg_state_)(
            kMainViewport,
            state,
            &options);
    if (result != sl::Result::eOk) {
        actual_presents_ = 0;
        if (++unsuccessful_state_queries_ == 1) {
            logger::error(
                "slDLSSGGetState after Present failed: {}",
                static_cast<int>(result));
        }
        return;
    }

    const auto status = static_cast<std::uint32_t>(state.status);
    if (status != static_cast<std::uint32_t>(sl::DLSSGStatus::eOk)) {
        actual_presents_ = 0;
        if (++unsuccessful_state_queries_ == 1 ||
            unsuccessful_state_queries_ == kFailureLimit) {
            logger::error(
                "DLSS-G runtime status=0x{:X}, frames-presented={}, failure-count={}",
                status,
                state.numFramesActuallyPresented,
                unsuccessful_state_queries_);
        }
        return;
    }

    unsuccessful_state_queries_ = 0;
    actual_presents_ = state.numFramesActuallyPresented;
    presented_frames_total_ += state.numFramesActuallyPresented;

    if (multiplier_tracker_.observe(actual_presents_)) {
        logger::info(
            "Measured Streamline presentation multiplier changed to {}x "
            "(telemetry only; output cap remains {} FPS)",
            multiplier_tracker_.stable(),
            frame_limit_ == 0 ?
                std::string{"uncapped"} : std::to_string(frame_limit_));
    }
    if (state.numFramesActuallyPresented > 1) {
        if (!generated_frames_verified_) {
            generated_frames_verified_ = true;
            logger::info(
                "DLSS-G verified: {} frames actually presented for a rendered frame at requested {}x",
                state.numFramesActuallyPresented,
                multiplier_);
        }
    } else if (!generated_frames_verified_) {
        ++waiting_present_count_;
        if (waiting_present_count_ == 1 ||
            waiting_present_count_ % kWaitingLogInterval == 0) {
            logger::info(
                "DLSS-G is waiting for an active foreground presentation (presented={})",
                state.numFramesActuallyPresented);
        }
    }
}

void FrameGeneration::shutdown() noexcept
{
    if (set_dlssg_options_ != nullptr &&
        (multiplier_ > 1 || resources_retained_)) {
        const auto options = make_options(
            width_,
            height_,
            back_buffer_count_,
            1,
            render::CadenceDecision{},
            false,
            false,
            temporal_width_ != 0U ? temporal_width_ : width_,
            temporal_height_ != 0U ? temporal_height_ : height_);
        static_cast<void>(
            reinterpret_cast<PFun_slDLSSGSetOptions*>(
                set_dlssg_options_)(kMainViewport, options));
    }
    if (set_reflex_options_ != nullptr) {
        sl::ReflexOptions options{};
        options.mode = sl::ReflexMode::eOff;
        static_cast<void>(
            reinterpret_cast<PFun_slReflexSetOptions*>(
                set_reflex_options_)(options));
    }

    set_reflex_options_ = nullptr;
    get_reflex_state_ = nullptr;
    set_dlssg_options_ = nullptr;
    get_dlssg_state_ = nullptr;
    width_ = 0;
    height_ = 0;
    back_buffer_count_ = 0;
    maximum_multiplier_ = 1;
    multiplier_ = 1;
    temporal_width_ = 0;
    temporal_height_ = 0;
    presents_since_state_query_ = 0;
    unsuccessful_state_queries_ = 0;
    waiting_present_count_ = 0;
    actual_presents_ = 1;
    frame_limit_ = 0;
    output_target_fps_ = 0;
    display_refresh_hz_ = 0;
    suspended_frames_ = 0ULL;
    idle_frames_ = 0ULL;
    refused_multiplier_ = 0U;
    multiplier_tracker_.reset();
    pacing_decision_ = render::PacingDecision{};
    cadence_decision_ = render::CadenceDecision{};
    pacing_refresh_dirty_ = false;
    ready_ = false;
    ui_recomposition_enabled_ = false;
    resources_retained_ = false;
    generated_frames_verified_ = false;

    recovery_policy_.reset();
    reflex_configured_ = false;
    reflex_raise_logged_ = false;
    vendor_generator_logged_ = false;
    vendor_fallback_logged_ = false;
    dynamic_unavailable_logged_ = false;
}

bool FrameGeneration::ready() const noexcept
{
    return ready_;
}

bool FrameGeneration::enabled() const noexcept
{
    return ready_ && multiplier_ > 1;
}

std::uint32_t FrameGeneration::multiplier() const noexcept
{
    return multiplier_;
}

std::uint32_t FrameGeneration::actual_presents() const noexcept
{
    return actual_presents_;
}

std::uint64_t FrameGeneration::presented_frames_total() const noexcept
{
    return presented_frames_total_;
}

std::uint32_t FrameGeneration::output_target_fps() const noexcept
{
    return output_target_fps_;
}

std::uint32_t FrameGeneration::applied_render_cap_fps() const noexcept
{
    return frame_limit_;
}

std::uint32_t FrameGeneration::effective_output_target_fps() const noexcept
{
    return pacing_decision_.presentation_cap_fps;
}

std::uint32_t FrameGeneration::measured_multiplier() const noexcept
{
    return multiplier_tracker_.stable();
}

std::uint32_t FrameGeneration::display_refresh_hz() const noexcept
{
    return display_refresh_hz_;
}

std::uint32_t FrameGeneration::generation_transitions() const noexcept
{
    return multiplier_tracker_.transitions();
}

render::PacingReason FrameGeneration::pacing_reason() const noexcept
{
    return pacing_decision_.reason;
}

bool FrameGeneration::pacing_target_achievable() const noexcept
{
    return pacing_decision_.target_achievable;
}

bool FrameGeneration::vrr_headroom_applied() const noexcept
{
    return false;
}

render::GenerationCadence FrameGeneration::cadence() const noexcept
{
    return cadence_decision_.cadence;
}

bool FrameGeneration::verified() const noexcept
{
    return generated_frames_verified_;
}
}
