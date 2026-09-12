#include "config/Settings.hpp"
#include "Version.hpp"
#include "enb/EnbApi.hpp"
#include "render/D3D12Backend.hpp"
#include "render/DynamicResolution.hpp"
#include "render/PresentationBridge.hpp"
#include "render/RendererDiagnostics.hpp"
#include "render/RuntimeCompatibility.hpp"
#include "render/RuntimeCompatibilitySkse.hpp"
#include "render/RendererRuntime.hpp"
#include "render/StatusOverlay.hpp"
#include "render/UpscalingPass.hpp"
#include "streamline/StreamlineApi.hpp"

#include <dxgi.h>

#include <SKSE/SKSE.h>

#include <spdlog/sinks/basic_file_sink.h>

#include <atomic>
#include <array>
#include <chrono>
#include <string>
#include <utility>
#include <cstring>
#include <cstdio>
#include <memory>

namespace
{
namespace logger = SKSE::log;
std::atomic_uint32_t diagnostic_frames_remaining{};

void schedule_post_frame_diagnostics() noexcept
{
    diagnostic_frames_remaining.store(3, std::memory_order_relaxed);
}

void run_scheduled_diagnostics()
{
    auto remaining = diagnostic_frames_remaining.load(std::memory_order_relaxed);
    while (remaining != 0 &&
           !diagnostic_frames_remaining.compare_exchange_weak(
               remaining,
               remaining - 1,
               std::memory_order_relaxed)) {
    }

    if (remaining == 1) {
        logger::info("Post-frame renderer diagnostics");
        mfgdlss::render::log_renderer_diagnostics(true);
    }
}

bool enb_teardown_seen = false;

std::array<std::atomic_uint64_t, 9> enb_callback_counts{};
std::atomic<std::chrono::steady_clock::time_point>
    enb_census_last{std::chrono::steady_clock::time_point{}};

[[nodiscard]] const char* enb_callback_name(
    const mfgdlss::enb::Callback callback) noexcept
{
    switch (callback) {
    case mfgdlss::enb::Callback::end_frame: return "end_frame";
    case mfgdlss::enb::Callback::begin_frame: return "begin_frame";
    case mfgdlss::enb::Callback::pre_save: return "pre_save";
    case mfgdlss::enb::Callback::post_load: return "post_load";
    case mfgdlss::enb::Callback::initialize: return "initialize";
    case mfgdlss::enb::Callback::exit: return "exit";
    case mfgdlss::enb::Callback::pre_reset: return "pre_reset";
    case mfgdlss::enb::Callback::post_reset: return "post_reset";
    }
    return "unknown";
}

void note_enb_callback(const mfgdlss::enb::Callback callback)
{
    const auto index = static_cast<std::size_t>(callback);
    if (index < enb_callback_counts.size()) {
        enb_callback_counts[index].fetch_add(
            1, std::memory_order_relaxed);
    }
    if (callback != mfgdlss::enb::Callback::end_frame) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto previous =
        enb_census_last.load(std::memory_order_relaxed);
    if (previous != std::chrono::steady_clock::time_point{} &&
        now - previous < std::chrono::seconds(60)) {
        return;
    }
    enb_census_last.store(now, std::memory_order_relaxed);
    std::string tally;
    for (std::size_t entry = 1; entry < enb_callback_counts.size();
         ++entry) {
        if (!tally.empty()) {
            tally += ", ";
        }
        tally += enb_callback_name(
            static_cast<mfgdlss::enb::Callback>(entry));
        tally += "=";
        tally += std::to_string(
            enb_callback_counts[entry].load(
                std::memory_order_relaxed));
    }
    logger::info(
        "ENB callback census: {}. This answers which callbacks ENB "
        "ACTUALLY fires in this build, which has never been established. "
        "A zero against exit, pre_reset or post_reset means UFGU's "
        "teardown and device-rebuild paths have never once executed, so "
        "any claim about their behaviour is untested. An ENB RELOAD "
        "should raise exit then initialize; watch these counts across a "
        "reload to capture the path",
        tally);
}

void __stdcall on_enb_callback(const mfgdlss::enb::Callback callback)
{
    note_enb_callback(callback);
    switch (callback) {
    case mfgdlss::enb::Callback::begin_frame:
        mfgdlss::render::RendererRuntime::instance().begin_frame();
        break;
    case mfgdlss::enb::Callback::end_frame:

        run_scheduled_diagnostics();
        break;
    case mfgdlss::enb::Callback::initialize: {
        logger::info("ENB initialized");
        if (enb_teardown_seen) {
            logger::info(
                "ENB initialized again after it had already shut down, so the "
                "Streamline interposer API released by that shutdown is being "
                "re-created before anything that depends on it");
            if (!mfgdlss::streamline::Api::instance().initialize()) {
                logger::error(
                    "The Streamline interposer API could not be re-created "
                    "after an ENB restart. NVIDIA frame generation and NVIDIA "
                    "upscaling will stay unavailable for the rest of this "
                    "session even if they worked before the restart; native "
                    "FSR and XeSS are unaffected. Restarting the game is the "
                    "only recovery");
            }
        }
        mfgdlss::render::log_renderer_diagnostics(false);
        const auto* info = mfgdlss::enb::Api::instance().render_info();
        const auto& presentation =
            mfgdlss::render::PresentationBridge::instance();
        const auto output_width =
            presentation.output_width() != 0 ?
                presentation.output_width() :
                info != nullptr ? info->width : 0;
        const auto output_height =
            presentation.output_height() != 0 ?
                presentation.output_height() :
                info != nullptr ? info->height : 0;
        if (info == nullptr ||
            !mfgdlss::render::RendererRuntime::instance().initialize(
                mfgdlss::render::RendererAdapter::enb,
                static_cast<ID3D11Device*>(info->d3d11_device),
                static_cast<ID3D11DeviceContext*>(info->d3d11_context),
                static_cast<IDXGISwapChain*>(info->dxgi_swap_chain),
                output_width,
                output_height)) {
            logger::error(
                "Universal upscaling/frame-generation runtime "
                "initialization failed");
        }
        schedule_post_frame_diagnostics();
        break;
    }
    case mfgdlss::enb::Callback::pre_reset:
        logger::info("ENB renderer reset starting");
        mfgdlss::render::RendererRuntime::instance().pre_reset();
        break;
    case mfgdlss::enb::Callback::post_reset:
        logger::info("ENB renderer reset complete");
        mfgdlss::render::RendererRuntime::instance().post_reset();
        mfgdlss::render::log_renderer_diagnostics(false);
        schedule_post_frame_diagnostics();
        break;
    case mfgdlss::enb::Callback::exit:
        logger::info("ENB shutting down");
        enb_teardown_seen = true;
        mfgdlss::render::DynamicResolution::instance().shutdown();
        mfgdlss::render::RendererRuntime::instance().shutdown();
        mfgdlss::render::PresentationBridge::instance().shutdown();
        mfgdlss::render::D3D12Backend::instance().shutdown();
        mfgdlss::streamline::Api::instance().shutdown();
        break;
    default:
        break;
    }
}

void connect_enb()
{
    const auto* const profile = mfgdlss::render::active_runtime_profile();
    if (profile != nullptr) {
        const auto& hooks = profile->hooks;
        const std::array<const mfgdlss::render::CodeSignature*, 6>
            signatures{
                &hooks.resolution_call_signature,
                &hooks.jitter_call_signature,
                &hooks.main_draw_call_signature,
                &hooks.post_processing_call_signature,
                &hooks.jitter_update_patch_signature,
                &hooks.camera_state_patch_signature};
        constexpr std::size_t kDirectCallSignatures = 4U;
        std::size_t approved = 0U;
        std::size_t with_target = 0U;
        for (std::size_t index = 0; index < signatures.size(); ++index) {
            approved += signatures[index]->approved ? 1U : 0U;
            if (index < kDirectCallSignatures) {
                with_target +=
                    signatures[index]->expected_target_rva != 0U ? 1U : 0U;
            }
        }
        const auto complete =
            approved == signatures.size() &&
            with_target == kDirectCallSignatures;
        if (complete) {
            logger::info(
                "Runtime profile: {}. All {} hook signatures are approved "
                "and all {} direct calls record an expected target address. "
                "Each hook is still checked against the running executable "
                "before it installs",
                profile->name,
                signatures.size(),
                kDirectCallSignatures);
        } else {
            logger::warn(
                "Runtime profile: {}. THIS PROFILE IS INCOMPLETE: {} of {} "
                "hook signatures are approved and {} of {} direct calls "
                "record an expected target address. Hooks whose signature "
                "cannot be verified are refused at install, so features "
                "that depend on them are unavailable on this runtime and "
                "their absence is a gap in UFGU's profile rather than a "
                "fault in the game",
                profile->name,
                approved,
                signatures.size(),
                with_target,
                kDirectCallSignatures);
        }
        if (!complete) {
            const auto module_base =
                REL::Module::get().base();
            const std::array<std::pair<const char*, std::pair<
                std::uint64_t, std::ptrdiff_t>>, 6> sites{{
                {"resolution_call",
                 {profile->address_ids.dynamic_resolution_update,
                  hooks.resolution_call}},
                {"jitter_call",
                 {profile->address_ids.temporal_jitter_update,
                  hooks.jitter_call}},
                {"main_draw_call",
                 {profile->address_ids.main_world_draw,
                  hooks.main_draw_call}},
                {"post_processing_call",
                 {profile->address_ids.post_processing,
                  hooks.post_processing_call}},
                {"jitter_update_patch",
                 {profile->address_ids.jitter_update_gate,
                  hooks.jitter_update_patch}},
                {"camera_state_patch",
                 {profile->address_ids.camera_state_build,
                  hooks.camera_state_patch}}}};
            for (std::size_t entry = 0; entry < sites.size(); ++entry) {
                const auto* const label = sites[entry].first;
                const auto id = sites[entry].second.first;
                const auto call_offset = sites[entry].second.second;
                const auto* const signature = signatures[entry];
                const auto function =
                    REL::ID(static_cast<std::uint64_t>(id)).address();
                if (function == 0U) {
                    logger::warn(
                        "Signature capture {}: address library id {} resolved "
                        "to zero on this runtime",
                        label,
                        id);
                    continue;
                }
                const auto site = function + call_offset;
                const auto window_start = static_cast<std::uintptr_t>(
                    static_cast<std::intptr_t>(site) +
                    signature->relative_offset);
                const auto length = signature->length != 0U ?
                    signature->length : std::size_t{24};
                std::string hex;
                auto readable = true;
                for (std::size_t index = 0; index < length; ++index) {
                    const auto* const at =
                        reinterpret_cast<const std::uint8_t*>(
                            window_start + index);
                    if (IsBadReadPtr(at, 1) != 0) {
                        readable = false;
                        break;
                    }
                    char pair[6]{};
                    std::snprintf(pair, sizeof(pair), "0x%02X,", *at);
                    hex += pair;
                }
                if (!readable) {
                    logger::warn(
                        "Signature capture {}: memory at the hook site is not "
                        "readable",
                        label);
                    continue;
                }
                std::string target_note;
                const auto* const opcode =
                    reinterpret_cast<const std::uint8_t*>(site);
                if (IsBadReadPtr(opcode, 5) == 0 && *opcode == 0xE8U) {
                    std::int32_t relative = 0;
                    std::memcpy(&relative, opcode + 1, sizeof(relative));
                    const auto target = site + 5U +
                        static_cast<std::uintptr_t>(
                            static_cast<std::intptr_t>(relative));
                    char note[96]{};
                    std::snprintf(
                        note,
                        sizeof(note),
                        " expected_target_rva = 0x%llX",
                        static_cast<unsigned long long>(
                            target - module_base));
                    target_note = note;
                }
                logger::warn(
                    "SIGNATURE CAPTURE {}: function rva 0x{:X}, site rva "
                    "0x{:X}, relative_offset {}, length {}, bytes = {}{}. "
                    "These are the values this runtime's profile needs, read "
                    "from the running process where Steam's encryption of "
                    ".text has already been removed",
                    label,
                    function - module_base,
                    site - module_base,
                    signature->relative_offset,
                    length,
                    hex,
                    target_note);
            }
        }
    } else {
        logger::error(
            "This Skyrim build has no runtime profile. Only 1.5.97 and "
            "1.6.1170 are supported, so the hooks that need engine addresses "
            "will not install and upscaling and frame generation will stay "
            "unavailable");
    }
    if (!mfgdlss::config::Settings::instance().load()) {
        logger::warn(
            "Settings could not be read; validated defaults will be used");
    }
    if (!mfgdlss::streamline::Api::instance().initialize()) {
        logger::info(
            "NVIDIA Streamline is unavailable; native FSR/XeSS "
            "super-resolution can still initialize");
    }
    if (!mfgdlss::render::install_presentation_bootstrap()) {
        logger::error("Presentation bridge bootstrap installation failed");
    }

    auto& api = mfgdlss::enb::Api::instance();
    if (!api.connect()) {
        logger::info(
            "ENB is not active; the vanilla renderer adapter will initialize "
            "from Skyrim's D3D11 swap chain");
        return;
    }

    logger::info("Connected to ENB {} with SDK {}", api.enb_version(), api.sdk_version());
    if (!api.set_callback(on_enb_callback)) {
        logger::error("Unable to register the ENB callback");
    }
}

void on_skse_message(SKSE::MessagingInterface::Message* message)
{
    if (message == nullptr) {
        return;
    }
    if (message->type == SKSE::MessagingInterface::kPostLoadGame ||
        message->type == SKSE::MessagingInterface::kNewGame) {

        mfgdlss::render::RendererRuntime::instance().reset_all_history(
            message->type == SKSE::MessagingInterface::kNewGame ?
                "a new game was started" :
                "a save was loaded");
        return;
    }
    if (message->type == SKSE::MessagingInterface::kPostLoad) {
        connect_enb();
    } else if (message->type == SKSE::MessagingInterface::kDataLoaded) {
        const auto dynamic_hooks =
            mfgdlss::render::DynamicResolution::instance().install();
        const auto upscaling_hooks = dynamic_hooks &&
            mfgdlss::render::UpscalingPass::instance().install();
        const auto overlay_input =
            mfgdlss::render::StatusOverlay::instance().install_input();
        if (!dynamic_hooks || !upscaling_hooks) {
            logger::error(
                "Render hook installation was not complete: dynamic "
                "resolution and viewport hooks={}, upscaling and pre-UI "
                "capture hooks={}. Neither upscaling nor frame generation "
                "can run without both. The usual cause is an unsupported "
                "game version, which is reported separately above this line",
                dynamic_hooks,
                upscaling_hooks);
        }
        if (!overlay_input) {
            logger::error("Native settings mouse input integration failed");
        }
    }
}

void initialize_logging()
{
    auto directory = logger::log_directory();
    if (!directory) {
        SKSE::stl::report_and_fail("SKSE log directory is unavailable");
    }

    *directory /= "UFGU.log";
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(directory->string(), true);
    auto log = std::make_shared<spdlog::logger>("UFGU", std::move(sink));
    log->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    log->set_level(spdlog::level::info);
    log->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(log));
}
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    initialize_logging();
    SKSE::Init(skse);

    logger::info("UFGU {} loading", mfgdlss::kProjectVersion);
    const auto* messaging = SKSE::GetMessagingInterface();
    if (messaging == nullptr || !messaging->RegisterListener(on_skse_message)) {
        logger::critical("Unable to register the SKSE messaging listener");
        return false;
    }

    logger::info("Native plugin shell loaded; waiting for Skyrim's renderer");
    return true;
}
