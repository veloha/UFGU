#include "render/RendererRuntime.hpp"

#include "config/Settings.hpp"
#include "providers/LowLatencyController.hpp"
#include "render/CameraData.hpp"
#include "render/D3D12Backend.hpp"
#include "render/DynamicResolution.hpp"
#include "render/FsrFrameGeneration.hpp"
#include "render/JitterOwnership.hpp"
#include "render/PresentationBridge.hpp"
#include "render/SamplerMipBias.hpp"
#include "render/SharedResources.hpp"
#include "render/XessFrameGeneration.hpp"
#include "render/StatusOverlay.hpp"
#include "render/UiCompositePass.hpp"
#include "render/UpscalingPass.hpp"
#include "streamline/FeatureSupport.hpp"
#include "streamline/FrameGeneration.hpp"
#include "streamline/FrameGenerationFramePolicy.hpp"
#include "streamline/FrameSubmission.hpp"
#include "streamline/SuperResolution.hpp"

#include <d3d11.h>
#include <dxgi.h>

#include <SKSE/SKSE.h>

#include <algorithm>
#include <chrono>
#include <string_view>

namespace mfgdlss::render
{
namespace
{
namespace logger = SKSE::log;
using TimingClock = std::chrono::steady_clock;

struct RuntimeStageTiming
{
    std::uint32_t samples{};
    double copy_total_ms{};
    double submit_total_ms{};
    double configure_total_ms{};
    double copy_max_ms{};
    double submit_max_ms{};
    double configure_max_ms{};
    TimingClock::time_point last_slow_report{};

    void record(
        const double copy_ms,
        const double submit_ms,
        const double configure_ms)
    {
        ++samples;
        copy_total_ms += copy_ms;
        submit_total_ms += submit_ms;
        configure_total_ms += configure_ms;
        copy_max_ms = (std::max)(copy_max_ms, copy_ms);
        submit_max_ms = (std::max)(submit_max_ms, submit_ms);
        configure_max_ms =
            (std::max)(configure_max_ms, configure_ms);

        const auto total_ms =
            copy_ms + submit_ms + configure_ms;
        const auto now = TimingClock::now();
        if (total_ms >= 8.0 &&
            (last_slow_report == TimingClock::time_point{} ||
             now - last_slow_report >= std::chrono::seconds(1))) {
            last_slow_report = now;
            logger::warn(
                "Slow MFG CPU submission frame: total={:.2f}ms "
                "(capture={:.2f}, tags={:.2f}, configure={:.2f})",
                total_ms,
                copy_ms,
                submit_ms,
                configure_ms);
        }

        if (samples == 300) {
            logger::info(
                "MFG CPU submission timing over 300 frames: "
                "capture={:.3f}ms avg/{:.3f}ms max, "
                "tags={:.3f}/{:.3f}, configure={:.3f}/{:.3f}",
                copy_total_ms / samples,
                copy_max_ms,
                submit_total_ms / samples,
                submit_max_ms,
                configure_total_ms / samples,
                configure_max_ms);
            samples = 0;
            copy_total_ms = 0.0;
            submit_total_ms = 0.0;
            configure_total_ms = 0.0;
            copy_max_ms = 0.0;
            submit_max_ms = 0.0;
            configure_max_ms = 0.0;
        }
    }
};

RuntimeStageTiming runtime_stage_timing;

struct PrologueTiming
{
    TimingClock::time_point last_report{};

    void record(
        const double settings_ms,
        const double gate_ms,
        const char* const exit_point)
    {
        const auto total_ms = settings_ms + gate_ms;
        if (total_ms < 8.0) {
            return;
        }
        const auto now = TimingClock::now();
        if (last_report != TimingClock::time_point{} &&
            now - last_report < std::chrono::seconds(1)) {
            return;
        }
        last_report = now;
        logger::warn(
            "Slow frame-generation prologue: total={:.2f}ms "
            "(settings={:.2f}, pause/suspend gate={:.2f}), exited at {}",
            total_ms,
            settings_ms,
            gate_ms,
            exit_point);
    }
};

PrologueTiming prologue_timing;

BoundedLogBudget frame_generation_stage_budget{8U};

std::uint64_t frame_generation_frames_total{};
std::uint64_t frame_generation_frames_enabled{};
std::uint32_t frame_generation_last_failed_stage{};
std::uint64_t gate_stuck_frames{};
std::uint64_t frame_generation_frames_suspended{};

[[nodiscard]] double milliseconds_between(
    const TimingClock::time_point begin,
    const TimingClock::time_point end) noexcept
{
    return std::chrono::duration<double, std::milli>(
               end - begin)
        .count();
}

[[nodiscard]] constexpr std::string_view adapter_name(
    const RendererAdapter adapter) noexcept
{
    return adapter == RendererAdapter::enb ? "ENB" : "vanilla";
}
}

RendererRuntime& RendererRuntime::instance() noexcept
{
    static RendererRuntime runtime;
    return runtime;
}

bool RendererRuntime::initialize(
    const RendererAdapter adapter,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    IDXGISwapChain* swap_chain,
    const std::uint32_t width,
    const std::uint32_t height)
{
    if (ready_) {
        return adapter_ == adapter;
    }
    if (device == nullptr ||
        context == nullptr ||
        width == 0 ||
        height == 0 ||
        !D3D12Backend::instance().initialize(device)) {
        return false;
    }
    const auto fail = [this]() {
        shutdown();
        return false;
    };

    DXGI_SWAP_CHAIN_DESC description{};
    auto back_buffer_count = 3U;
    if (swap_chain != nullptr &&
        SUCCEEDED(swap_chain->GetDesc(&description))) {
        back_buffer_count = (std::max)(description.BufferCount, 2U);
    }

    if (!streamline::SuperResolution::instance().initialize(
            device,
            context,
            width,
            height) ||
        !streamline::SuperResolution::instance().set_mode(
            config::Settings::instance().upscaling_mode())) {
        logger::error(
            "{} renderer could not initialize the selected "
            "super-resolution provider",
            adapter_name(adapter));
        return fail();
    }

    streamline_features_ready_ =
        D3D12Backend::instance().streamline_proxy_active() &&
        streamline::FeatureSupport::instance().initialize(width, height) &&
        streamline::FrameGeneration::instance().initialize(
            width,
            height,
            back_buffer_count);
    if (!streamline_features_ready_) {
        streamline::FrameGeneration::instance().shutdown();
        streamline::FrameSubmission::instance().shutdown();
        streamline::FeatureSupport::instance().shutdown();
        logger::info(
            "{} renderer is running without NVIDIA DLSS-G/Reflex; "
            "the selected super-resolution provider remains active",
            adapter_name(adapter));
        if (D3D12Backend::instance().streamline_proxy_active() &&
            streamline::FrameSubmission::instance().initialize_timing(
                width, height)) {
            logger::info(
                "PC latency markers are running even though the DLSS stack is "
                "absent. NVIDIA documents PCL Stats as supported on every GPU, "
                "vendor and driver version, so latency measurement no longer "
                "depends on Reflex or DLSS-G being present on this card");
        }
    }
    if (!CameraData::instance().install(context)) {
        logger::error(
            "{} renderer could not initialize native camera capture",
            adapter_name(adapter));
        return fail();
    }
    if (!DynamicResolution::instance().attach_context(context)) {
        logger::error(
            "{} renderer could not install scene-viewport verification",
            adapter_name(adapter));
        return fail();
    }

    providers::LowLatencyController::instance().initialize(device);
    install_sampler_mip_bias(device);

    adapter_ = adapter;
    ready_ = true;
    logger::info(
        "Universal upscaling runtime initialized through the {} renderer "
        "adapter; Streamline presentation={}",
        adapter_name(adapter_),
        streamline_features_ready_);
    return true;
}

void RendererRuntime::begin_frame()
{
    if (!ready_) {
        return;
    }
    if (streamline_features_ready_) {
        if (!streamline::FrameSubmission::instance().begin_frame()) {

            return;
        }
    } else {

        if (native_frame_open_) {
            return;
        }
        native_frame_open_ = true;
        static_cast<void>(
            streamline::FrameSubmission::instance().begin_frame());
    }
    XessFrameGeneration::instance().begin_latency_frame();
    providers::LowLatencyController::instance().refresh_mode_from_settings();
    providers::LowLatencyController::instance().update_before_input_sampling(
        config::Settings::instance().frame_limit());
    SharedResources::instance().begin_frame();
    UpscalingPass::instance().begin_frame();
}

void RendererRuntime::end_frame()
{
    if (!ready_) {
        return;
    }
    native_frame_open_ = false;
    XessFrameGeneration::instance().add_latency_marker(
        XessFrameGeneration::LatencyMarker::simulation_end);
    XessFrameGeneration::instance().add_latency_marker(
        XessFrameGeneration::LatencyMarker::render_submit_start);

    const auto prologue_begin = TimingClock::now();
    if (config::Settings::instance().reload_if_changed()) {
        auto& super_resolution =
            streamline::SuperResolution::instance();
        const auto configured_mode =
            config::Settings::instance().upscaling_mode();
        if (PresentationBridge::instance().
                uses_virtual_render_surface() &&
            configured_mode != super_resolution.mode()) {
            logger::warn(
                "DLSS profile changed while the complete-frame render "
                "surface is active; the new profile will apply after the "
                "next game launch");
        } else {
            UpscalingPass::instance().reset_history();
            streamline::FrameSubmission::instance().request_history_reset();
            static_cast<void>(
                super_resolution.set_mode(configured_mode));
        }
        if (XessFrameGeneration::selected() &&
            XessFrameGeneration::instance().owns_presentation()) {
            XessFrameGeneration::instance().reset_history();
            logger::info(
                "XeSS-FG interpolation history reset because settings that "
                "shape its inputs were changed while the game is running");
        } else if (
            FsrFrameGeneration::selected() &&
            FsrFrameGeneration::instance().owns_presentation()) {
            FsrFrameGeneration::instance().reset_history();
            logger::info(
                "FidelityFX frame generation interpolation history reset "
                "because settings that shape its inputs were changed while "
                "the game is running");
        }
        if (streamline_features_ready_) {
            static_cast<void>(
                streamline::FrameGeneration::instance().set_reflex_mode(
                    config::Settings::instance().reflex_mode()));
            static_cast<void>(
                streamline::FrameGeneration::instance().set_output_target_fps(
                    config::Settings::instance().frame_limit()));
        }
    }
    const auto settings_end = TimingClock::now();
    const auto settings_ms =
        milliseconds_between(prologue_begin, settings_end);
    const auto note_prologue = [settings_ms, settings_end](
                                   const char* const exit_point) {
        prologue_timing.record(
            settings_ms,
            milliseconds_between(settings_end, TimingClock::now()),
            exit_point);
    };
    if (!streamline_features_ready_) {

        note_prologue("no Streamline features");
        return;
    }
    auto& settings = config::Settings::instance();
    auto& frame_generation =
        streamline::FrameGeneration::instance();
    auto* ui = RE::UI::GetSingleton();
    const auto paused =
        ui != nullptr && ui->GameIsPaused();
    const auto plugin_menu_open = StatusOverlay::instance().menu_open();
    if (!settings.frame_generation_enabled()) {

        static_cast<void>(
            streamline::FrameSubmission::instance().submit_inactive_tags());
        static_cast<void>(frame_generation.suspend_for_reset(
            "frame generation is switched off in the settings"));
        note_prologue("frame generation disabled");
        return;
    }
    if (plugin_menu_open || paused) {
        if (paused && ui != nullptr) {
            std::string pausing;
            for (const auto& open_menu : ui->menuStack) {
                if (open_menu == nullptr || !open_menu->PausesGame()) {
                    continue;
                }
                const char* name = "<unnamed>";
                for (const auto& entry : ui->menuMap) {
                    if (entry.second.menu.get() == open_menu.get()) {
                        name = entry.first.c_str();
                        break;
                    }
                }
                if (!pausing.empty()) {
                    pausing += ", ";
                }
                pausing += name;
            }
            static std::string last_pausing;
            if (pausing != last_pausing) {
                last_pausing = pausing;
                logger::info(
                    "Skyrim pause gate raised by OPEN menu(s) on the stack: {}. numPausesGame "
                    "counts {}. If this names NO menu while the game reports "
                    "paused, RE::UI::GameIsPaused() is true with nothing "
                    "actually open, and gating frame generation on it is "
                    "wrong. veloha reports the dropout happens on being HIT "
                    "with no menu opened, so this line is the one that "
                    "identifies what pauses on a hit",
                    pausing.empty() ? "NONE OPEN" : pausing.c_str(),
                    ui->numPausesGame);
            }
        }

        static_cast<void>(
            streamline::FrameSubmission::instance().submit_inactive_tags());
        const auto gate_reason =
            plugin_menu_open && paused ?
                streamline::FrameGenerationGateReason::
                    plugin_menu_and_game_paused :
                (plugin_menu_open ?
                     streamline::FrameGenerationGateReason::plugin_menu :
                     streamline::FrameGenerationGateReason::game_paused);
        static_cast<void>(frame_generation.suspend_for_gate(
            settings.frame_generation_multiplier(),
            gate_reason));
        note_prologue("menu open or game paused");
        return;
    }
    note_prologue("continued to capture");
    const auto copy_begin = TimingClock::now();
    const auto copied = SharedResources::instance().copy_frame();
    const auto copy_end = TimingClock::now();
    const auto submitted =
        copied &&
        streamline::FrameSubmission::instance().submit();
    const auto submit_end = TimingClock::now();
    const auto gate_resume_pending =
        submitted && frame_generation.gate_suspended();
    auto gate_resumed = false;
    if (submitted) {
        if (gate_resume_pending) {

            gate_resumed = frame_generation.resume_from_gate(
                settings.frame_generation_multiplier());
        } else {

            frame_generation.note_presentation_healthy();

            static_cast<void>(frame_generation.resume_if_eligible());
        }
    }
    const auto configured =
        submitted &&
        (gate_resume_pending ?
             gate_resumed :
             frame_generation.enable(
                 settings.frame_generation_multiplier()));
    const auto configure_end = TimingClock::now();
    runtime_stage_timing.record(
        milliseconds_between(copy_begin, copy_end),
        milliseconds_between(copy_end, submit_end),
        milliseconds_between(submit_end, configure_end));

    const auto stage =
        (copied ? 0x1U : 0U) |
        (submitted ? 0x2U : 0U) |
        (configured ? 0x4U : 0U);
    const auto incomplete_action =
        streamline::incomplete_frame_action(copied, submitted, configured);
    if (!configured && frame_generation_stage_budget.admit(stage)) {
        logger::warn(
            "Frame generation produced no generated frame: copy_frame={}, "
            "submit={}, enable={}. The first false is the cause; "
            "everything after it never ran. This frame receives explicit "
            "null inputs; the configured DLSS-G mode {}.",
            copied,
            submitted,
            configured,
            incomplete_action ==
                    streamline::IncompleteFrameAction::
                        publish_inactive_tags_keep_mode ?
                "remains armed for the next complete gameplay frame" :
                "will be suspended because runtime configuration failed");
    }

    if (frame_generation.gate_suspended()) {
        ++gate_stuck_frames;
        if (gate_stuck_frames == 120ULL ||
            gate_stuck_frames == 1200ULL ||
            gate_stuck_frames % 12000ULL == 0ULL) {
            logger::warn(
                "THE DETERMINISTIC GATE IS STUCK. It has stayed raised for {} "
                "consecutive frames on which Skyrim was NOT paused and no "
                "plugin menu was open, so the condition that raised it is "
                "long gone. The gate releases only when copy_frame AND submit "
                "both succeed on the same frame, and this frame reported "
                "copy_frame={}, submit={}, enable={}. Whichever of those is "
                "false is the reason frame generation is dead and will stay "
                "dead; an alt-tab appears to fix it because it forces a fresh "
                "pause and resume, not because it rebuilds anything. Search "
                "upward for an MFG submission line for the specific cause",
                gate_stuck_frames,
                copied,
                submitted,
                configured);
        }
    } else {
        gate_stuck_frames = 0ULL;
    }

    ++frame_generation_frames_total;
    if (configured) {
        if (frame_generation.generation_suspended()) {
            ++frame_generation_frames_suspended;
        } else {
            ++frame_generation_frames_enabled;
        }
    } else {
        frame_generation_last_failed_stage = stage;
    }
    if (frame_generation_frames_total % 600U == 0U) {

        auto& fg = streamline::FrameGeneration::instance();
        const auto* const presents_note = fg.actual_presents() > 1U ?
            "actual-presents is above 1, so the runtime is both configured "
            "and generating." :
            !fg.verified() ?
            "actual-presents is 1 and nothing has ever been verified, so the "
            "runtime is configured and generating nothing." :
            "actual-presents has fallen back to 1 after generating earlier, "
            "so generation is configured and currently producing nothing.";
        logger::info(
            "Frame generation census over {} frames: {} ACTUALLY generated, "
            "{} were SUSPENDED while enable() still reported success, and {} "
            "failed outright. Current suspension cause: {}, for {} "
            "consecutive frames. Before 2026-08-21 the suspended column was "
            "counted as enabled, because enable() returns true while the "
            "recovery policy is suspended, so this census reported healthy "
            "generation during a total dropout. Trust the suspended column "
            "over any other line in this log. Last failing stage: "
            "copy_frame={}, submit={}, "
            "enable={}. DLSS-G reports: requested={}x ({}), measured={}x, "
            "actual-presents={}, ever-verified={}, presented-total={}. {} "
            "Under a dynamic cadence "
            "measured is EXPECTED to sit below requested and to move, because "
            "requested is a ceiling rather than a count.",
            frame_generation_frames_total,
            frame_generation_frames_enabled,
            frame_generation_frames_suspended,
            frame_generation_frames_total - frame_generation_frames_enabled -
                frame_generation_frames_suspended,
            fg.suspension_cause_name(),
            fg.suspended_frames(),
            (frame_generation_last_failed_stage & 0x1U) != 0U,
            (frame_generation_last_failed_stage & 0x2U) != 0U,
            (frame_generation_last_failed_stage & 0x4U) != 0U,
            fg.multiplier(),
            describe(fg.cadence()),
            fg.measured_multiplier(),
            fg.actual_presents(),
            fg.verified(),
            fg.presented_frames_total(),
            presents_note);
    }
    if (configured) {
        return;
    }

    static_cast<void>(
        streamline::FrameSubmission::instance().submit_inactive_tags());
    if (incomplete_action ==
        streamline::IncompleteFrameAction::
            publish_inactive_tags_keep_mode) {
        return;
    }

    static_cast<void>(frame_generation.suspend(
        "this frame did not complete a full Streamline submission"));
}

void RendererRuntime::pre_reset() noexcept
{
    native_frame_open_ = false;
    static_cast<void>(
        streamline::FrameGeneration::instance().suspend_for_reset(
            "the renderer is resetting its devices"));
    if (XessFrameGeneration::selected() &&
        XessFrameGeneration::instance().owns_presentation()) {
        XessFrameGeneration::instance().set_enabled(false);
        vendor_generation_suspended_ = true;
    } else if (
        FsrFrameGeneration::selected() &&
        FsrFrameGeneration::instance().owns_presentation()) {
        FsrFrameGeneration::instance().set_enabled(false);
        vendor_generation_suspended_ = true;
    }
    if (vendor_generation_suspended_) {
        logger::info(
            "Vendor frame generation suspended for a device reset before the "
            "shared textures it holds are destroyed");
    }
    streamline::FrameSubmission::instance().request_history_reset();
    streamline::FrameSubmission::instance().abandon_frame();
    UpscalingPass::instance().shutdown();
    SharedResources::instance().shutdown();
}

void RendererRuntime::post_reset() noexcept
{
    if (!vendor_generation_suspended_) {
        return;
    }
    vendor_generation_suspended_ = false;
    if (XessFrameGeneration::selected()) {
        XessFrameGeneration::instance().reset_history();
        XessFrameGeneration::instance().set_enabled(true);
    } else if (FsrFrameGeneration::selected()) {
        FsrFrameGeneration::instance().reset_history();
        FsrFrameGeneration::instance().set_enabled(true);
    }
    logger::info(
        "Vendor frame generation resumed after the device reset; "
        "interpolation history was reset");
}

bool RendererRuntime::resize(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t back_buffer_count)
{
    if (!ready_ ||
        width == 0 ||
        height == 0 ||
        back_buffer_count < 2) {
        return false;
    }

    auto& frame_generation =
        streamline::FrameGeneration::instance();
    auto& frame_submission =
        streamline::FrameSubmission::instance();
    auto& super_resolution =
        streamline::SuperResolution::instance();
    const auto presentation_features_reconfigured =
        !streamline_features_ready_ ||
        (frame_generation.reconfigure(
             width,
             height,
             back_buffer_count) &&
         frame_submission.initialize_timing(width, height));
    if (!presentation_features_reconfigured ||
        !super_resolution.reconfigure(width, height)) {
        logger::error(
            "{} renderer feature controllers failed to reconfigure for "
            "{}x{}, buffers={}",
            adapter_name(adapter_),
            width,
            height,
            back_buffer_count);
        return false;
    }

    if (streamline_features_ready_) {
        streamline::FeatureSupport::instance().refresh_vram_estimate(
            width, height);
    }

    logger::info(
        "{} renderer feature controllers reconfigured for {}x{}, buffers={}",
        adapter_name(adapter_),
        width,
        height,
        back_buffer_count);
    if (adapter_ == RendererAdapter::vanilla) {
        begin_frame();
    }
    return true;
}

void RendererRuntime::reset_all_history(const char* const reason) noexcept
{
    if (!ready_) {
        return;
    }
    UpscalingPass::instance().reset_history();
    UiCompositePass::instance().reset_capture_reports();
    streamline::FrameSubmission::instance().request_history_reset();
    const char* generator = "none";
    if (XessFrameGeneration::selected() &&
        XessFrameGeneration::instance().owns_presentation()) {
        XessFrameGeneration::instance().reset_history();
        generator = "XeSS-FG";
    } else if (
        FsrFrameGeneration::selected() &&
        FsrFrameGeneration::instance().owns_presentation()) {
        FsrFrameGeneration::instance().reset_history();
        generator = "FidelityFX frame generation";
    }
    logger::info(
        "Upscaling and frame generation history reset because {}. Vendor "
        "generator reset: {}",
        reason == nullptr ? "the renderer asked for it" : reason,
        generator);
}

void RendererRuntime::shutdown() noexcept
{
    providers::LowLatencyController::instance().shutdown();
    CameraData::instance().shutdown();
    UpscalingPass::instance().shutdown();
    streamline::SuperResolution::instance().shutdown();
    streamline::FrameGeneration::instance().shutdown();
    streamline::FrameSubmission::instance().shutdown();
    SharedResources::instance().shutdown();
    streamline::FeatureSupport::instance().shutdown();
    streamline_features_ready_ = false;
    native_frame_open_ = false;
    ready_ = false;
}

bool RendererRuntime::ready() const noexcept
{
    return ready_;
}

bool RendererRuntime::streamline_features_ready() const noexcept
{
    return ready_ && streamline_features_ready_;
}

bool RendererRuntime::uses(const RendererAdapter adapter) const noexcept
{
    return ready_ && adapter_ == adapter;
}
}
