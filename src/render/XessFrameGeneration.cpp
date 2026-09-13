#include "render/XessFrameGeneration.hpp"

#include "config/Settings.hpp"
#include "providers/RuntimeLoader.hpp"
#include "render/CameraData.hpp"
#include "render/D3D12Backend.hpp"
#include "render/PresentationBridge.hpp"
#include "render/SharedResources.hpp"
#include "streamline/SuperResolution.hpp"

#include <Windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <SKSE/SKSE.h>

#include <algorithm>
#include <array>

#if __has_include(<xess_fg/xefg_swapchain_d3d12.h>)
#    define MFG_DLSS_HAVE_XEFG_HEADERS 1

#    pragma warning(push)
#    pragma warning(disable : 4820 4365 4514 5219)
#    include <xell/xell.h>
#    include <xell/xell_d3d12.h>
#    include <xess_fg/xefg_swapchain.h>
#    include <xess_fg/xefg_swapchain_d3d12.h>
#    pragma warning(pop)
#else
#    define MFG_DLSS_HAVE_XEFG_HEADERS 0
#endif

namespace mfgdlss::render
{
namespace
{
namespace logger = SKSE::log;
using Microsoft::WRL::ComPtr;

constexpr auto kIntelSubdirectory = "UFGU/Intel";

#if MFG_DLSS_HAVE_XEFG_HEADERS
using PfnXefgGetVersion = xefg_swapchain_result_t(*)(
    xefg_swapchain_version_t*);
using PfnXefgGetProperties = xefg_swapchain_result_t(*)(
    xefg_swapchain_handle_t, xefg_swapchain_properties_t*);
using PfnXefgCreateContext = xefg_swapchain_result_t(*)(
    ID3D12Device*, xefg_swapchain_handle_t*);
using PfnXefgInitFromSwapChain = xefg_swapchain_result_t(*)(
    xefg_swapchain_handle_t,
    ID3D12CommandQueue*,
    const xefg_swapchain_d3d12_init_params_t*);
using PfnXefgGetSwapChainPtr = xefg_swapchain_result_t(*)(
    xefg_swapchain_handle_t, REFIID, void**);
using PfnXefgTagFrameResource = xefg_swapchain_result_t(*)(
    xefg_swapchain_handle_t,
    ID3D12CommandList*,
    uint32_t,
    const xefg_swapchain_d3d12_resource_data_t*);
using PfnXefgBuildPipelines = xefg_swapchain_result_t(*)(
    xefg_swapchain_handle_t, ID3D12PipelineLibrary*, uint8_t, uint32_t);
using PfnXefgTagFrameConstants = xefg_swapchain_result_t(*)(
    xefg_swapchain_handle_t,
    uint32_t,
    const xefg_swapchain_frame_constant_data_t*);
using PfnXefgSetPresentId = xefg_swapchain_result_t(*)(
    xefg_swapchain_handle_t, uint32_t);
using PfnXefgSetEnabled = xefg_swapchain_result_t(*)(
    xefg_swapchain_handle_t, uint32_t);
using PfnXefgSetNumInterpolatedFrames = xefg_swapchain_result_t(*)(
    xefg_swapchain_handle_t, uint32_t);
using PfnXefgSetUiCompositionState = xefg_swapchain_result_t(*)(
    xefg_swapchain_handle_t, xefg_swapchain_ui_composition_state_t);
using PfnXefgSetLatencyReduction = xefg_swapchain_result_t(*)(
    xefg_swapchain_handle_t, void*);
using PfnXefgGetLastPresentStatus = xefg_swapchain_result_t(*)(
    xefg_swapchain_handle_t, xefg_swapchain_present_status_t*);
using PfnXefgDestroy = xefg_swapchain_result_t(*)(xefg_swapchain_handle_t);

using PfnXellCreateContext = xell_result_t(*)(
    ID3D12Device*, xell_context_handle_t*);
using PfnXellDestroyContext = xell_result_t(*)(xell_context_handle_t);
using PfnXefgSetLoggingCallback = xefg_swapchain_result_t(*)(
    xefg_swapchain_handle_t,
    xefg_swapchain_logging_level_t,
    xefg_swapchain_app_log_callback_t,
    void*);
using PfnXellGetFramesReports = xell_result_t(*)(
    xell_context_handle_t,
    xell_frame_report_t*);
using PfnXellSetLoggingCallback = xell_result_t(*)(
    xell_context_handle_t,
    xell_logging_level_t,
    xell_app_log_callback_t);
using PfnXellSetSleepMode = xell_result_t(*)(
    xell_context_handle_t, const xell_sleep_params_t*);
using PfnXellSleep = xell_result_t(*)(xell_context_handle_t, std::uint32_t);
using PfnXellAddMarkerData = xell_result_t(*)(
    xell_context_handle_t, std::uint32_t, xell_latency_marker_type_t);

[[nodiscard]] std::string xefg_result_text(const xefg_swapchain_result_t result)
{
    switch (result) {
    case XEFG_SWAPCHAIN_RESULT_SUCCESS: return "success";
    case XEFG_SWAPCHAIN_RESULT_WARNING_OLD_DRIVER:
        return "warning: old driver";
    case XEFG_SWAPCHAIN_RESULT_WARNING_TOO_FEW_FRAMES:
        return "warning: too few frames";
    case XEFG_SWAPCHAIN_RESULT_WARNING_FRAMES_ID_MISMATCH:
        return "warning: frame id mismatch";
    case XEFG_SWAPCHAIN_RESULT_WARNING_MISSING_PRESENT_STATUS:
        return "warning: missing present status";
    case XEFG_SWAPCHAIN_RESULT_WARNING_RESOURCE_SIZES_MISMATCH:
        return "warning: resource sizes mismatch";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE:
        return "unsupported device";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DRIVER:
        return "unsupported driver";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNINITIALIZED: return "uninitialized";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT:
        return "invalid argument";
    case XEFG_SWAPCHAIN_RESULT_ERROR_DEVICE_OUT_OF_MEMORY:
        return "device out of memory";
    case XEFG_SWAPCHAIN_RESULT_ERROR_DEVICE: return "device error";
    case XEFG_SWAPCHAIN_RESULT_ERROR_NOT_IMPLEMENTED: return "not implemented";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_CONTEXT: return "invalid context";
    case XEFG_SWAPCHAIN_RESULT_ERROR_OPERATION_IN_PROGRESS:
        return "operation in progress";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED: return "unsupported";
    case XEFG_SWAPCHAIN_RESULT_ERROR_CANT_LOAD_LIBRARY:
        return "cannot load library";
    case XEFG_SWAPCHAIN_RESULT_ERROR_MISMATCH_INPUT_RESOURCES:
        return "input resources do not match each other";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INCORRECT_OUTPUT_RESOURCES:
        return "incorrect output resources";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INCORRECT_INPUT_RESOURCES:
        return "incorrect input resources";
    case XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_UNSUPPORTED:
        return "latency reduction unsupported";
    case XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_FUNCTION_MISSING:
        return "latency reduction function missing";
    case XEFG_SWAPCHAIN_RESULT_ERROR_HRESULT_FAILURE: return "HRESULT failure";
    case XEFG_SWAPCHAIN_RESULT_ERROR_DXGI_INVALID_CALL:
        return "DXGI invalid call";
    case XEFG_SWAPCHAIN_RESULT_ERROR_POINTER_STILL_IN_USE:
        return "a swap chain pointer is still in use";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_DESCRIPTOR_HEAP:
        return "invalid descriptor heap";
    case XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER:
        return "wrong call order";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNKNOWN:
    default:
        break;
    }
    return "unknown error " + std::to_string(static_cast<int>(result));
}

void xefg_log_bridge(
    const char* const message,
    const xefg_swapchain_logging_level_t level,
    void* const)
{
    if (message == nullptr) {
        return;
    }
    std::string text{message};
    while (!text.empty() &&
           (text.back() == '\n' || text.back() == '\r' ||
            text.back() == '\t' || text.back() == ' ')) {
        text.pop_back();
    }
    if (text.empty()) {
        return;
    }
    if (level == XEFG_SWAPCHAIN_LOGGING_LEVEL_ERROR) {
        logger::error("[XeSS-FG] {}", text);
        return;
    }
    if (level == XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING) {
        logger::warn("[XeSS-FG] {}", text);
        return;
    }
    logger::info("[XeSS-FG] {}", text);
}

void xell_log_bridge(
    const char* const message,
    const xell_logging_level_t level)
{
    if (message == nullptr) {
        return;
    }
    std::string text{message};
    while (!text.empty() &&
           (text.back() == '\n' || text.back() == '\r' ||
            text.back() == '\t' || text.back() == ' ')) {
        text.pop_back();
    }
    if (text.empty()) {
        return;
    }
    if (level == XELL_LOGGING_LEVEL_ERROR) {
        logger::error("[XeLL] {}", text);
        return;
    }
    if (level == XELL_LOGGING_LEVEL_WARNING) {
        logger::warn("[XeLL] {}", text);
        return;
    }
    logger::info("[XeLL] {}", text);
}

[[nodiscard]] bool xefg_ok(const xefg_swapchain_result_t result) noexcept
{
    return static_cast<int>(result) >= 0;
}
#endif
}

struct XessFrameGeneration::State
{
    XessGenerationState status{XessGenerationState::inactive};
    std::string version;
    std::string detail;
    std::uint32_t maximum_interpolated_frames{};
    std::uint32_t interpolated_frames{};
    std::uint32_t presented_total{};
    std::uint32_t applied_output_cap{0xFFFFFFFFU};
    bool applied_low_latency{true};
    std::uint64_t estimated_vram_bytes{};
    std::uint32_t last_tagged_frame_id{};
    bool frame_id_stall_logged{};
    bool low_latency_forced_logged{};
    bool enabled{true};
    bool reset_pending{};
    bool reinstall_required{};
    bool installed_depth_reversed{};
    DWORD owner_thread_id{};

    std::string waiting_reason_logged;
    bool tag_warning_logged{};
    bool xell_report_warning_logged{};
    std::uint32_t stale_temporal_inputs_frames{};
    bool first_submission_logged{};
    bool status_warning_logged{};

    std::uint32_t census_frames{};
    std::uint32_t census_generated{};
    std::uint32_t census_last_enabled{};

    std::uint32_t submission_frames{};
    std::uint32_t submission_ok{};
    std::string submission_last_failure;

    void note_submission(const bool submitted)
    {
        ++submission_frames;
        if (submitted) {
            ++submission_ok;
        } else if (!detail.empty()) {
            submission_last_failure = detail;
        }
        if (submission_frames < 600U) {
            return;
        }
        logger::info(
            "XeSS-FG input submission over {} frames: {} tagged and submitted "
            "successfully, {} failed.{}",
            submission_frames,
            submission_ok,
            submission_frames - submission_ok,
            submission_last_failure.empty() ?
                std::string{} :
                " Most recent submission failure: " + submission_last_failure);
        submission_frames = 0U;
        submission_ok = 0U;
        submission_last_failure.clear();
    }

#if MFG_DLSS_HAVE_XEFG_HEADERS
    providers::RuntimeLoader fg_loader;
    providers::RuntimeLoader xell_loader;

    PfnXefgGetVersion get_version{};
    PfnXefgGetProperties get_properties{};
    PfnXefgCreateContext create_context{};
    PfnXefgInitFromSwapChain init_from_swap_chain{};
    PfnXefgGetSwapChainPtr get_swap_chain{};
    PfnXefgTagFrameResource tag_resource{};
    PfnXefgBuildPipelines build_pipelines{};
    PfnXefgTagFrameConstants tag_constants{};
    PfnXefgSetPresentId set_present_id{};
    PfnXefgSetEnabled set_enabled_fn{};
    PfnXefgSetNumInterpolatedFrames set_num_interpolated_frames{};
    PfnXefgSetUiCompositionState set_ui_composition{};
    PfnXefgSetLatencyReduction set_latency_reduction{};
    PfnXefgGetLastPresentStatus get_present_status{};
    PfnXefgDestroy destroy_fn{};

    PfnXellCreateContext xell_create{};
    PfnXellDestroyContext xell_destroy{};
    PfnXellSetSleepMode xell_set_sleep_mode{};
    PfnXellSleep xell_sleep{};
    PfnXellAddMarkerData xell_add_marker{};
    std::uint32_t latency_frame_id{};
    bool xell_sleep_absent_logged{};
    PfnXefgSetLoggingCallback set_logging_callback{};
    PfnXellSetLoggingCallback xell_set_logging_callback{};
    PfnXellGetFramesReports xell_get_frames_reports{};

    xefg_swapchain_handle_t handle{};
    xell_context_handle_t xell_context{};
    ComPtr<IDXGISwapChain> adopted_swap_chain{};

    xefg_swapchain_result_t census_last_result{XEFG_SWAPCHAIN_RESULT_SUCCESS};

    void refresh_memory_usage() noexcept
    {
        if (handle == nullptr || get_properties == nullptr) {
            return;
        }
        xefg_swapchain_properties_t properties{};
        if (!xefg_ok(get_properties(handle, &properties))) {
            return;
        }
        const auto total = properties.tempBufferHeapSize +
            properties.tempTextureHeapSize + properties.constantBufferSize;
        if (total == estimated_vram_bytes) {
            return;
        }
        estimated_vram_bytes = total;
        logger::info(
            "XeSS-FG now uses {} MB of GPU memory (buffer heap {} MB, texture "
            "heap {} MB, constants {} KB)",
            total / (1024ULL * 1024ULL),
            properties.tempBufferHeapSize / (1024ULL * 1024ULL),
            properties.tempTextureHeapSize / (1024ULL * 1024ULL),
            properties.constantBufferSize / 1024ULL);
    }

    [[nodiscard]] bool apply_sleep_parameters(
        const std::uint32_t output_cap,
        const bool requested_low_latency)
    {
        if (xell_context == nullptr || xell_set_sleep_mode == nullptr) {
            return false;
        }
        const auto low_latency = requested_low_latency || handle != nullptr;
        if (low_latency != requested_low_latency &&
            !low_latency_forced_logged) {

            low_latency_forced_logged = true;
            logger::info(
                "Latency reduction is set to Off, but XeSS-FG is installed "
                "and Intel documents that frame generation is disabled if "
                "XeLL is not enabled. XeLL is therefore being kept on for as "
                "long as Intel frame generation is running. Turning frame "
                "generation off releases it");
        }
        if (output_cap == applied_output_cap &&
            low_latency == applied_low_latency) {
            return true;
        }
        xell_sleep_params_t sleep{};
        sleep.minimumIntervalUs =
            output_cap == 0U ? 0U : 1000000U / output_cap;
        sleep.bLowLatencyMode = low_latency ? 1U : 0U;
        sleep.bLowLatencyBoost = 0U;
        const auto result = xell_set_sleep_mode(xell_context, &sleep);
        if (result != XELL_RESULT_SUCCESS) {
            logger::warn(
                "xellSetSleepMode could not apply latency reduction {} with a "
                "final-output cap of {}: {}",
                low_latency,
                output_cap,
                static_cast<int>(result));
            return false;
        }
        applied_output_cap = output_cap;
        applied_low_latency = low_latency;
        logger::info(
            "XeLL latency reduction is now {}, final-output cap {}",
            low_latency ? "on" : "off",
            output_cap == 0U ? std::string{"uncapped"}
                             : std::to_string(output_cap) + " FPS");
        return true;
    }

    [[nodiscard]] bool apply_interpolated_frames(
        const std::uint32_t requested_frames)
    {
        const auto ceiling = maximum_interpolated_frames >= 1U ?
            maximum_interpolated_frames : 1U;
        const auto clamped = std::clamp(requested_frames, 1U, ceiling);
        if (clamped == interpolated_frames) {
            return true;
        }
        if (handle == nullptr) {
            return false;
        }
        if (set_num_interpolated_frames == nullptr) {
            reinstall_required = true;
            logger::info(
                "This XeSS-FG runtime does not export "
                "xefgSwapChainSetNumInterpolatedFrames, so the interpolated "
                "frame count is fixed when the swap chain is initialised. "
                "Reinitialising to apply {} interpolated frame(s).",
                clamped);
            return false;
        }
        const auto result = set_num_interpolated_frames(handle, clamped);
        if (!xefg_ok(result)) {
            if (result == XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT) {
                logger::error(
                    "xefgSwapChainSetNumInterpolatedFrames({}) rejected the "
                    "count; the runtime ceiling is {}",
                    clamped,
                    maximum_interpolated_frames);
            } else {
                reinstall_required = true;
                logger::error(
                    "xefgSwapChainSetNumInterpolatedFrames({}): {}. XeSS-FG "
                    "disables frame generation on this class of error and the "
                    "swap chain must be reinitialised to recover.",
                    clamped,
                    xefg_result_text(result));
            }
            return false;
        }
        interpolated_frames = clamped;
        reset_pending = true;
        logger::info(
            "XeSS-FG now produces {} interpolated frame(s) per rendered frame "
            "(runtime ceiling {})",
            clamped,
            maximum_interpolated_frames);
        return true;
    }

    [[nodiscard]] bool resolve_exports()
    {
        get_version =
            fg_loader.function<PfnXefgGetVersion>("xefgSwapChainGetVersion");
        get_properties = fg_loader.function<PfnXefgGetProperties>(
            "xefgSwapChainGetProperties");
        create_context = fg_loader.function<PfnXefgCreateContext>(
            "xefgSwapChainD3D12CreateContext");
        init_from_swap_chain = fg_loader.function<PfnXefgInitFromSwapChain>(
            "xefgSwapChainD3D12InitFromSwapChain");
        get_swap_chain = fg_loader.function<PfnXefgGetSwapChainPtr>(
            "xefgSwapChainD3D12GetSwapChainPtr");
        tag_resource = fg_loader.function<PfnXefgTagFrameResource>(
            "xefgSwapChainD3D12TagFrameResource");
        build_pipelines = fg_loader.function<PfnXefgBuildPipelines>(
            "xefgSwapChainD3D12BuildPipelines");
        tag_constants = fg_loader.function<PfnXefgTagFrameConstants>(
            "xefgSwapChainTagFrameConstants");
        set_present_id = fg_loader.function<PfnXefgSetPresentId>(
            "xefgSwapChainSetPresentId");
        set_enabled_fn = fg_loader.function<PfnXefgSetEnabled>(
            "xefgSwapChainSetEnabled");
        set_num_interpolated_frames =
            fg_loader.function<PfnXefgSetNumInterpolatedFrames>(
                "xefgSwapChainSetNumInterpolatedFrames");
        set_ui_composition = fg_loader.function<PfnXefgSetUiCompositionState>(
            "xefgSwapChainSetUiCompositionState");
        set_latency_reduction = fg_loader.function<PfnXefgSetLatencyReduction>(
            "xefgSwapChainSetLatencyReduction");
        set_logging_callback = fg_loader.function<PfnXefgSetLoggingCallback>(
            "xefgSwapChainSetLoggingCallback");
        get_present_status = fg_loader.function<PfnXefgGetLastPresentStatus>(
            "xefgSwapChainGetLastPresentStatus");
        destroy_fn =
            fg_loader.function<PfnXefgDestroy>("xefgSwapChainDestroy");
        return get_version != nullptr && get_properties != nullptr &&
               create_context != nullptr && init_from_swap_chain != nullptr &&
               get_swap_chain != nullptr && tag_resource != nullptr &&
               build_pipelines != nullptr && tag_constants != nullptr &&
               set_present_id != nullptr && set_enabled_fn != nullptr &&
               set_ui_composition != nullptr &&
               set_latency_reduction != nullptr &&
               get_present_status != nullptr && destroy_fn != nullptr;
    }

    [[nodiscard]] bool resolve_xell_exports()
    {
        xell_create = xell_loader.function<PfnXellCreateContext>(
            "xellD3D12CreateContext");
        xell_destroy =
            xell_loader.function<PfnXellDestroyContext>("xellDestroyContext");
        xell_set_sleep_mode =
            xell_loader.function<PfnXellSetSleepMode>("xellSetSleepMode");
        xell_sleep = xell_loader.function<PfnXellSleep>("xellSleep");
        xell_add_marker =
            xell_loader.function<PfnXellAddMarkerData>("xellAddMarkerData");
        xell_set_logging_callback =
            xell_loader.function<PfnXellSetLoggingCallback>(
                "xellSetLoggingCallback");
        xell_get_frames_reports =
            xell_loader.function<PfnXellGetFramesReports>(
                "xellGetFramesReports");
        return xell_create != nullptr && xell_destroy != nullptr &&
               xell_set_sleep_mode != nullptr;
    }
#endif
};

XessFrameGeneration::XessFrameGeneration() :
    state_{std::make_unique<State>()}
{
}

XessFrameGeneration::~XessFrameGeneration()
{

    static_cast<void>(state_.release());
}

XessFrameGeneration& XessFrameGeneration::instance() noexcept
{
    static XessFrameGeneration generation;
    return generation;
}

bool XessFrameGeneration::selected() noexcept
{
    return config::Settings::instance().vendor_frame_generation() ==
        config::VendorFrameGeneration::intel;
}

bool XessFrameGeneration::install_if_ready(
    const std::uint32_t interpolated_frames)
{
#if !MFG_DLSS_HAVE_XEFG_HEADERS
    static_cast<void>(interpolated_frames);
    state_->status = XessGenerationState::inactive;
    state_->detail = "built without the Intel XeSS frame generation headers";
    return false;
#else
    if (state_->status == XessGenerationState::installed) {
        const auto live_depth = CameraData::instance().depth_contract();
        if (live_depth.determinate() &&
            live_depth.reversed() != state_->installed_depth_reversed) {
            logger::warn(
                "The depth convention changed from {} to {} after XeSS-FG was "
                "installed. The depth flag is fixed when the swap chain is "
                "initialised, so it is being reinitialised rather than left "
                "interpreting depth backwards.",
                state_->installed_depth_reversed ? "inverted" : "standard",
                live_depth.reversed() ? "inverted" : "standard");
            shutdown();
        } else if (
            state_->apply_interpolated_frames(interpolated_frames) ||
            !state_->reinstall_required) {
            return true;
        }
        state_->reinstall_required = false;
        logger::warn(
            "The interpolated frame count could not be changed on the live "
            "swap chain, so it is being reinitialised to apply the new count");
        shutdown();
    }
    if (state_->status == XessGenerationState::failed) {

        return false;
    }
    if (!selected()) {
        state_->status = XessGenerationState::inactive;
        return false;
    }

    const auto fail = [this](std::string reason) {
        state_->status = XessGenerationState::failed;
        state_->detail = std::move(reason);
        logger::error("XeSS-FG installation failed: {}", state_->detail);
        return false;
    };
    const auto wait = [this](std::string reason) {
        state_->status = XessGenerationState::waiting;
        state_->detail = std::move(reason);
        if (state_->waiting_reason_logged != state_->detail) {
            state_->waiting_reason_logged = state_->detail;
            logger::info(
                "XeSS-FG is selected and waiting to install: {}",
                state_->detail);
        }
        return false;
    };

    if (!state_->fg_loader.loaded() &&
        !state_->fg_loader.load(
            kIntelSubdirectory,
            L"libxess_fg.dll",
            true,
            L"Intel Corporation")) {
        return fail("libxess_fg.dll: " + state_->fg_loader.failure());
    }
    if (!state_->resolve_exports()) {
        return fail("libxess_fg.dll is missing one or more required exports");
    }
    if (!state_->xell_loader.loaded() &&
        !state_->xell_loader.load(
            kIntelSubdirectory,
            L"libxell.dll",
            true,
            L"Intel Corporation")) {
        return fail("libxell.dll: " + state_->xell_loader.failure());
    }
    if (!state_->resolve_xell_exports()) {
        return fail("libxell.dll is missing one or more required exports");
    }

    if (!probe_capability()) {
        return fail(
            "the runtime would not report a supported interpolated-frame "
            "count");
    }

    if (!streamline::SuperResolution::instance().enabled()) {
        return wait(
            "upscaling is set to Off, so Skyrim's motion vectors and depth are "
            "never prepared for a frame generator; select an upscaling mode "
            "and frame generation will install");
    }

    const auto depth = CameraData::instance().depth_contract();
    if (!depth.determinate()) {
        return wait("the depth convention has not been measured yet");
    }

    auto& resources = SharedResources::instance();
    if (!resources.ready() ||
        !resources.streamline_ui_recomposition_available()) {

        auto& bridge = PresentationBridge::instance();
        return wait(
            bridge.ready() && !bridge.uses_virtual_render_surface() ?
                "no verified HUD-less/UI separation yet, and the game is "
                "rendering at the output extent, so it must come from "
                "extraction rather than the UI redirect" :
                "no verified HUD-less/UI separation yet");
    }

    auto& backend = D3D12Backend::instance();
    auto* const device = static_cast<ID3D12Device*>(backend.native_device());
    auto* const queue =
        static_cast<ID3D12CommandQueue*>(backend.command_queue());
    auto* const application_chain = static_cast<IDXGISwapChain*>(
        PresentationBridge::instance().presentation_swap_chain());
    if (!backend.ready() || device == nullptr || queue == nullptr) {
        return wait("the D3D12 device or graphics queue is not ready");
    }
    if (application_chain == nullptr) {
        return wait("the presentation bridge has no swap chain to adopt");
    }

    const auto install_ceiling = state_->maximum_interpolated_frames >= 1U ?
        state_->maximum_interpolated_frames : 1U;
    const auto requested =
        std::clamp(interpolated_frames, 1U, install_ceiling);

    xefg_swapchain_result_t result = XEFG_SWAPCHAIN_RESULT_SUCCESS;
    if (state_->handle == nullptr) {
        result = state_->create_context(device, &state_->handle);
        if (!xefg_ok(result) || state_->handle == nullptr) {
            return fail(
                "xefgSwapChainD3D12CreateContext: " +
                xefg_result_text(result));
        }
    }

    const std::uint32_t init_flags = depth.reversed() ?
        static_cast<std::uint32_t>(XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH) :
        static_cast<std::uint32_t>(XEFG_SWAPCHAIN_INIT_FLAG_NONE);

    if (state_->set_logging_callback != nullptr) {
        static_cast<void>(state_->set_logging_callback(
            state_->handle,
            XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING,
            &xefg_log_bridge,
            nullptr));
    }

    result = state_->build_pipelines(state_->handle, nullptr, 1U, init_flags);
    if (xefg_ok(result) && result != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        logger::warn(
            "XeSS-FG pipeline build returned a warning rather than plain "
            "success: {}",
            xefg_result_text(result));
    }
    if (!xefg_ok(result)) {
        shutdown();
        return fail(
            "xefgSwapChainD3D12BuildPipelines: " + xefg_result_text(result));
    }

    const auto xell_result =
        state_->xell_create(device, &state_->xell_context);
    if (xell_result == XELL_RESULT_SUCCESS &&
        state_->xell_context != nullptr &&
        state_->xell_set_logging_callback != nullptr) {

        static_cast<void>(state_->xell_set_logging_callback(
            state_->xell_context,
            XELL_LOGGING_LEVEL_WARNING,
            &xell_log_bridge));
    }
    if (xell_result != XELL_RESULT_SUCCESS ||
        state_->xell_context == nullptr) {
        shutdown();
        return fail(
            "xellD3D12CreateContext failed: " +
            std::to_string(static_cast<int>(xell_result)));
    }

    result = state_->set_latency_reduction(
        state_->handle, state_->xell_context);
    if (!xefg_ok(result)) {
        shutdown();
        return fail(
            "xefgSwapChainSetLatencyReduction: " + xefg_result_text(result));
    }

    {
        const auto output_cap = config::Settings::instance().frame_limit();
        xell_sleep_params_t sleep{};
        sleep.minimumIntervalUs = output_cap == 0U ?
            0U : 1000000U / output_cap;
        sleep.bLowLatencyMode = 1U;
        sleep.bLowLatencyBoost = 0U;
        const auto sleep_result =
            state_->xell_set_sleep_mode(state_->xell_context, &sleep);
        if (sleep_result != XELL_RESULT_SUCCESS) {

            logger::warn(
                "xellSetSleepMode failed ({}); XeSS-FG requires XeLL to be "
                "enabled before initialization and may now refuse to install",
                static_cast<int>(sleep_result));
        } else {
            logger::info(
                "XeLL latency reduction enabled, final-output cap {}",
                output_cap == 0U ? "uncapped" :
                    std::to_string(output_cap) + " FPS");
        }
    }

    auto* const handed_over = static_cast<IDXGISwapChain*>(
        PresentationBridge::instance().begin_presentation_handoff());
    if (handed_over == nullptr) {
        return fail("the presentation bridge would not give up its swap chain");
    }

    const auto fail_after_handoff = [&fail](std::string reason) {
        if (!PresentationBridge::instance().abort_presentation_handoff()) {
            logger::error(
                "Presentation could not be restored after the failed handoff");
        }
        return fail(std::move(reason));
    };

    xefg_swapchain_d3d12_init_params_t params{};
    params.pApplicationSwapChain = handed_over;
    params.initFlags = init_flags;
    params.maxInterpolatedFrames =
        state_->set_num_interpolated_frames != nullptr
            ? XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES
            : requested;
    params.creationNodeMask = 1U;
    params.visibleNodeMask = 1U;

    params.uiMode = XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS_UITEXTURE;

    result = state_->init_from_swap_chain(state_->handle, queue, &params);
    if (!xefg_ok(result)) {
        shutdown();
        return fail_after_handoff(
            "xefgSwapChainD3D12InitFromSwapChain: " + xefg_result_text(result));
    }
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        logger::warn(
            "XeSS-FG initialised with a warning rather than plain success: "
            "{}. Intel treats this as usable, so frame generation continues, "
            "but the warning is the runtime telling you something about this "
            "machine that it will not repeat. An old driver is the usual one "
            "and updating the Intel graphics driver is the usual fix",
            xefg_result_text(result));
    }

    void* generated_chain{};
    result = state_->get_swap_chain(
        state_->handle, __uuidof(IDXGISwapChain), &generated_chain);
    if (!xefg_ok(result) || generated_chain == nullptr) {
        shutdown();
        return fail_after_handoff(
            "xefgSwapChainD3D12GetSwapChainPtr: " + xefg_result_text(result));
    }

    state_->adopted_swap_chain.Attach(
        static_cast<IDXGISwapChain*>(generated_chain));

    if (!PresentationBridge::instance().adopt_presentation_swap_chain(
            state_->adopted_swap_chain.Get())) {
        shutdown();
        return fail_after_handoff(
            "the presentation bridge refused the generated swap chain");
    }

    result = state_->set_ui_composition(
        state_->handle, XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED);
    if (!xefg_ok(result)) {
        shutdown();
        return fail(
            "xefgSwapChainSetUiCompositionState: " + xefg_result_text(result));
    }
    logger::info(
        "XeSS-FG UI composition enabled in BACKBUFFER_HUDLESS_UITEXTURE mode; "
        "the HUD is composited from this plugin's premultiplied UI layer "
        "rather than interpolated with the scene, and anything else drawn "
        "over the scene before Present is extracted from the back buffer so "
        "generated frames keep it too");

    if (state_->set_num_interpolated_frames != nullptr) {
        state_->interpolated_frames = state_->maximum_interpolated_frames;
        if (!state_->apply_interpolated_frames(requested)) {
            shutdown();
            return fail("xefgSwapChainSetNumInterpolatedFrames could not "
                        "select the requested interpolated frame count");
        }
    }

    result = state_->set_enabled_fn(
        state_->handle, state_->enabled ? 1U : 0U);
    if (!xefg_ok(result)) {
        shutdown();
        return fail("xefgSwapChainSetEnabled: " + xefg_result_text(result));
    }

    state_->interpolated_frames = requested;
    state_->installed_depth_reversed = depth.reversed();
    state_->owner_thread_id = GetCurrentThreadId();
    state_->latency_frame_id = 0U;
    state_->last_tagged_frame_id = 0U;
    state_->frame_id_stall_logged = false;
    state_->low_latency_forced_logged = false;
    state_->reset_pending = true;
    state_->status = XessGenerationState::installed;
    state_->detail.clear();
    logger::info(
        "{} installed over this plugin's D3D12 swap chain: {} interpolated "
        "frame(s) (runtime ceiling {}), depth={}, XeLL attached. Skyrim and "
        "ENB continue to present through SwapChainProxy and observe no change.",
        state_->version.empty() ? "XeSS-FG" : state_->version,
        requested,
        state_->maximum_interpolated_frames,
        depth.reversed() ? "inverted" : "standard");
    return true;
#endif
}

bool XessFrameGeneration::submit_frame(
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    const float frame_time_milliseconds,
    const bool reset_history_requested)
{
#if !MFG_DLSS_HAVE_XEFG_HEADERS
    static_cast<void>(render_width);
    static_cast<void>(render_height);
    static_cast<void>(output_width);
    static_cast<void>(output_height);
    static_cast<void>(frame_time_milliseconds);
    static_cast<void>(reset_history_requested);
    return false;
#else
    if (state_->status != XessGenerationState::installed ||
        state_->handle == nullptr || !state_->enabled) {
        return false;
    }

    bool submitted = false;
    const struct Census {
        State& state;
        const bool& submitted;
        ~Census() { state.note_submission(submitted); }
    } census{*state_, submitted};

    auto& resources = SharedResources::instance();
    if (!resources.streamline_ui_recomposition_available() ||
        !resources.frame_generation_hudless_captured()) {

        state_->detail = !resources.streamline_ui_recomposition_available() ?
            "the UI layer could not be separated from the frame, so there is "
            "no HUD-less colour to interpolate" :
            "no HUD-less colour was captured for this frame";
        return false;
    }

    if (!resources.temporal_inputs_prepared()) {
        ++state_->stale_temporal_inputs_frames;
        const auto count = state_->stale_temporal_inputs_frames;
        if (count == 1U || count == 600U || count == 6000U ||
            count % 60000U == 0U) {
            logger::warn(
                "{} has interpolated with depth and motion vectors that were "
                "not prepared for the current frame on {} frame(s) so far, "
                "because upscaling evaluation did not run on those frames. "
                "Those textures still hold the previous frame's data, which "
                "is a ghosting risk",
                "XeSS-FG",
                count);
        }
    }

    auto* const hudless =
        static_cast<ID3D12Resource*>(resources.frame_generation_hudless());
    auto* const depth = static_cast<ID3D12Resource*>(resources.depth());

    auto* const motion =
        static_cast<ID3D12Resource*>(resources.vendor_motion_vectors());
    auto* const ui = static_cast<ID3D12Resource*>(resources.ui_color_alpha());
    if (hudless == nullptr || depth == nullptr || motion == nullptr ||
        ui == nullptr) {
        state_->detail = std::string{"a shared input is missing:"} +
            (hudless == nullptr ? " HUD-less colour" : "") +
            (depth == nullptr ? " depth" : "") +
            (motion == nullptr ? " motion vectors" : "") +
            (ui == nullptr ? " UI colour" : "");
        return false;
    }
    resources.log_vendor_input_provenance("XeSS-FG");

    float view[16]{};
    float projection[16]{};
    if (!CameraData::instance().frame_matrices(view, projection)) {
        state_->detail =
            "the camera view and projection matrices were not available this "
            "frame";
        return false;
    }

    if (state_->latency_frame_id == state_->last_tagged_frame_id) {
        ++state_->latency_frame_id;
        if (!state_->frame_id_stall_logged) {
            state_->frame_id_stall_logged = true;
            logger::warn(
                "The shared XeLL and XeSS-FG frame counter did not advance "
                "between two presents, which means the renderer frame start "
                "did not run. Advancing it here so XeSS-FG never sees a "
                "repeated present id, but the XeLL markers for this frame "
                "will be attributed to the previous one");
        }
    }
    state_->last_tagged_frame_id = state_->latency_frame_id;
    auto result =
        state_->set_present_id(state_->handle, state_->latency_frame_id);
    if (!xefg_ok(result)) {
        state_->detail =
            "xefgSwapChainSetPresentId: " + xefg_result_text(result);
        return false;
    }

    struct TagRequest
    {
        xefg_swapchain_resource_type_t type;
        ID3D12Resource* resource;
        std::uint32_t width;
        std::uint32_t height;
    };

    const std::array<TagRequest, 4> requests{{
        {XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, hudless, output_width,
         output_height},
        {XEFG_SWAPCHAIN_RES_UI, ui, output_width, output_height},
        {XEFG_SWAPCHAIN_RES_DEPTH, depth, render_width, render_height},
        {XEFG_SWAPCHAIN_RES_MOTION_VECTOR, motion, render_width,
         render_height},
    }};
    for (const auto& request : requests) {
        xefg_swapchain_d3d12_resource_data_t data{};
        data.type = request.type;

        data.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
        data.resourceBase = {0U, 0U};
        data.resourceSize = {request.width, request.height};
        data.pResource = request.resource;
        data.incomingState = D3D12_RESOURCE_STATE_COMMON;
        result = state_->tag_resource(
            state_->handle, nullptr, state_->latency_frame_id, &data);
        if (!xefg_ok(result)) {
            state_->detail = "xefgSwapChainD3D12TagFrameResource: " +
                xefg_result_text(result);
            return false;
        }
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS &&
            !state_->tag_warning_logged) {
            state_->tag_warning_logged = true;
            logger::warn(
                "XeSS-FG accepted a tagged resource with a warning: {}",
                xefg_result_text(result));
        }
    }

    xefg_swapchain_frame_constant_data_t constants{};
    std::copy(
        std::begin(view), std::end(view), std::begin(constants.viewMatrix));
    std::copy(
        std::begin(projection),
        std::end(projection),
        std::begin(constants.projectionMatrix));

    const auto jitter = CameraData::instance().frame_jitter();
    constants.jitterOffsetX = jitter.pixels_x;
    constants.jitterOffsetY = jitter.pixels_y;

    constants.motionVectorScaleX = static_cast<float>(render_width);
    constants.motionVectorScaleY = static_cast<float>(render_height);
    constants.resetHistory =
        (reset_history_requested || state_->reset_pending) ? 1U : 0U;

    constants.frameRenderTime = frame_time_milliseconds;

    result = state_->tag_constants(
        state_->handle, state_->latency_frame_id, &constants);
    if (!xefg_ok(result)) {
        state_->detail =
            "xefgSwapChainTagFrameConstants: " + xefg_result_text(result);
        return false;
    }
    state_->reset_pending = false;

    if (!state_->first_submission_logged) {
        state_->first_submission_logged = true;
        logger::info(
            "XeSS-FG accepted its first tagged frame: hudless/UI {}x{}, "
            "depth/motion {}x{}, {} interpolated frame(s)",
            output_width,
            output_height,
            render_width,
            render_height,
            state_->interpolated_frames);
    }

    xefg_swapchain_present_status_t status{};
    const auto status_result =
        state_->get_present_status(state_->handle, &status);
    if (xefg_ok(status_result)) {
        state_->presented_total = status.framesPresented;
        ++state_->census_frames;
        if (status.framesPresented > 1U) {
            ++state_->census_generated;
        }
        state_->census_last_result = status.frameGenResult;
        state_->census_last_enabled = status.isFrameGenEnabled;
        if (state_->census_frames >= 600U) {
            state_->refresh_memory_usage();
            logger::info(
                "XeSS-FG census over {} frames: {} presented more than one "
                "frame (interpolation happening), {} presented exactly one "
                "(no interpolation). Runtime reports frame generation "
                "enabled={}, last interpolation result={}, "
                "frames-presented-last={}. Runtime is configured for {} "
                "interpolated frame(s) ({}x) against a ceiling of {}; the "
                "settings ask for {}x{}.{}",
                state_->census_frames,
                state_->census_generated,
                state_->census_frames - state_->census_generated,
                state_->census_last_enabled,
                xefg_result_text(state_->census_last_result),
                status.framesPresented,
                state_->interpolated_frames,
                state_->interpolated_frames + 1U,
                state_->maximum_interpolated_frames,
                config::Settings::instance().frame_generation_multiplier(),
                state_->interpolated_frames + 1U ==
                        config::Settings::instance()
                            .frame_generation_multiplier() ?
                    std::string{} :
                    std::string{", so the requested multiplier is not what is "
                                "running"},
                state_->census_frames == state_->census_generated ?
                    std::string{} :
                    std::string{" Frames that presented exactly one are not "
                                "necessarily a fault: XeSS-FG disables "
                                "interpolation on a detected scene change, and "
                                "its threshold is left at Intel's own default "
                                "of 0.7 on a scale where higher means more "
                                "sensitive and therefore more readily "
                                "disabled. The runtime reports no distinct "
                                "result code for that case, so a high count "
                                "here during ordinary gameplay makes the "
                                "threshold the first thing to suspect."});
            if (state_->xell_get_frames_reports != nullptr &&
                state_->xell_context != nullptr) {

                std::array<xell_frame_report_t, 64U> reports{};
                if (state_->xell_get_frames_reports(
                        state_->xell_context, reports.data()) ==
                    XELL_RESULT_SUCCESS) {

                    std::uint32_t filled = 0U;
                    std::uint64_t total_ns = 0ULL;
                    std::uint64_t worst_ns = 0ULL;
                    for (const auto& report : reports) {
                        if (report.m_frame_id == 0U ||
                            report.m_present_end_ts <= report.m_sim_start_ts) {
                            continue;
                        }
                        const auto span =
                            report.m_present_end_ts - report.m_sim_start_ts;
                        ++filled;
                        total_ns += span;
                        worst_ns = (std::max)(worst_ns, span);
                    }
                    if (filled == 0U) {
                        if (!state_->xell_report_warning_logged) {
                            state_->xell_report_warning_logged = true;
                            logger::warn(
                                "XeLL returned no usable frame reports. The "
                                "latency runtime is created and handed to the "
                                "frame generation swap chain, but nothing here "
                                "shows it measuring frames, so treat Intel low "
                                "latency as unproven on this machine");
                        }
                    } else {
                        logger::info(
                            "XeLL frame reports: {} of 64 usable, mean "
                            "simulation-start to present-end {:.2f}ms, worst "
                            "{:.2f}ms. This is the runtime measuring itself, "
                            "so it also confirms XeLL is actually running "
                            "rather than merely created",
                            filled,
                            static_cast<double>(total_ns / filled) / 1.0e6,
                            static_cast<double>(worst_ns) / 1.0e6);
                    }
                }
            }
            state_->census_frames = 0U;
            state_->census_generated = 0U;
        }
    } else if (!state_->status_warning_logged) {
        state_->status_warning_logged = true;
        logger::warn(
            "xefgSwapChainGetLastPresentStatus failed ({}); whether frames are "
            "being interpolated cannot be confirmed this session",
            xefg_result_text(status_result));
    }
    state_->detail.clear();
    submitted = true;
    return true;
#endif
}

void XessFrameGeneration::set_enabled(const bool enabled) noexcept
{
    state_->enabled = enabled;
    if (!enabled) {
        state_->presented_total = 0U;
    }
#if MFG_DLSS_HAVE_XEFG_HEADERS

    if (state_->status == XessGenerationState::installed &&
        state_->handle != nullptr && state_->set_enabled_fn != nullptr) {
        const auto result =
            state_->set_enabled_fn(state_->handle, enabled ? 1U : 0U);
        if (!xefg_ok(result)) {
            logger::error(
                "xefgSwapChainSetEnabled({}): {}",
                enabled,
                xefg_result_text(result));
        }
    }

    if (enabled) {
        state_->reset_pending = true;
    }
#endif
}

void XessFrameGeneration::set_output_target_fps(
    const std::uint32_t frames_per_second) noexcept
{
#if MFG_DLSS_HAVE_XEFG_HEADERS
    static_cast<void>(state_->apply_sleep_parameters(
        frames_per_second,
        config::Settings::instance().reflex_mode() !=
            config::ReflexMode::off));
#else
    static_cast<void>(frames_per_second);
#endif
}

void XessFrameGeneration::begin_latency_frame() noexcept
{
    ++state_->latency_frame_id;
    if (state_->xell_context == nullptr) {
        return;
    }
    if (state_->xell_sleep == nullptr) {
        if (!state_->xell_sleep_absent_logged) {
            state_->xell_sleep_absent_logged = true;
            logger::warn(
                "This XeLL runtime does not export xellSleep, so latency "
                "reduction cannot be applied. XeLL will stay configured and "
                "inert");
        }
        return;
    }
    const auto result =
        state_->xell_sleep(state_->xell_context, state_->latency_frame_id);
    if (result != XELL_RESULT_SUCCESS && !state_->xell_sleep_absent_logged) {
        state_->xell_sleep_absent_logged = true;
        logger::warn(
            "xellSleep failed ({}) on frame {}, so Intel latency reduction is "
            "not being applied for as long as this keeps failing. Reported "
            "once rather than once per frame",
            static_cast<int>(result),
            state_->latency_frame_id);
    }
    add_latency_marker(LatencyMarker::simulation_start);
}

void XessFrameGeneration::add_latency_marker(
    const LatencyMarker marker) noexcept
{
    static_assert(
        static_cast<std::uint32_t>(LatencyMarker::simulation_start) ==
                static_cast<std::uint32_t>(XELL_SIMULATION_START) &&
            static_cast<std::uint32_t>(LatencyMarker::present_end) ==
                static_cast<std::uint32_t>(XELL_PRESENT_END),
        "LatencyMarker must mirror xell_latency_marker_type_t");

    if (state_->xell_context == nullptr ||
        state_->xell_add_marker == nullptr ||
        state_->latency_frame_id == 0U) {

        return;
    }
    static_cast<void>(state_->xell_add_marker(
        state_->xell_context,
        state_->latency_frame_id,
        static_cast<xell_latency_marker_type_t>(marker)));
}

void XessFrameGeneration::reset_history() noexcept
{

    state_->reset_pending = true;
}

void XessFrameGeneration::shutdown() noexcept
{
    if (state_->owner_thread_id != 0U &&
        state_->owner_thread_id != GetCurrentThreadId()) {
        logger::critical(
            "{} teardown was reached on thread {} but its runtime contexts "
            "were created on thread {}. The contexts are still being "
            "destroyed, because leaving them alive would leak the swap chain "
            "across an ENB reload, but if a crash follows this line the "
            "cross-thread teardown is the first thing to suspect",
            "XeSS-FG",
            GetCurrentThreadId(),
            state_->owner_thread_id);
    }
#if MFG_DLSS_HAVE_XEFG_HEADERS

    if (state_->handle != nullptr && state_->set_enabled_fn != nullptr) {
        static_cast<void>(state_->set_enabled_fn(state_->handle, 0U));
    }
    if (state_->adopted_swap_chain != nullptr) {
        PresentationBridge::instance().release_presentation_swap_chain();
        const auto remaining = state_->adopted_swap_chain.Reset();
        if (remaining != 0U) {
            logger::warn(
                "The XeSS-FG swap chain still holds {} outstanding "
                "reference(s) after this plugin released its own; "
                "xefgSwapChainDestroy may report POINTER_STILL_IN_USE",
                remaining);
        }
    }
    if (state_->handle != nullptr && state_->destroy_fn != nullptr) {
        const auto result = state_->destroy_fn(state_->handle);
        if (!xefg_ok(result)) {
            logger::error("xefgSwapChainDestroy: {}", xefg_result_text(result));
        }
    }
    state_->handle = nullptr;
    if (state_->xell_context != nullptr && state_->xell_destroy != nullptr) {
        static_cast<void>(state_->xell_destroy(state_->xell_context));
    }
    state_->xell_context = nullptr;
    state_->applied_output_cap = 0xFFFFFFFFU;
    state_->applied_low_latency = true;
    state_->estimated_vram_bytes = 0ULL;
    state_->reinstall_required = false;
    state_->latency_frame_id = 0U;
    state_->last_tagged_frame_id = 0U;
    state_->frame_id_stall_logged = false;
    state_->low_latency_forced_logged = false;
    state_->interpolated_frames = 0U;
    state_->maximum_interpolated_frames = 0U;
    state_->presented_total = 0U;
    state_->first_submission_logged = false;
    state_->status_warning_logged = false;
    state_->tag_warning_logged = false;
    state_->xell_report_warning_logged = false;
    state_->xell_sleep_absent_logged = false;
    state_->stale_temporal_inputs_frames = 0U;
    state_->waiting_reason_logged.clear();
    state_->submission_frames = 0U;
    state_->submission_ok = 0U;
    state_->submission_last_failure.clear();
    state_->census_last_enabled = 0U;
    state_->census_last_result = XEFG_SWAPCHAIN_RESULT_SUCCESS;
    state_->reset_pending = false;
    state_->census_frames = 0U;
    state_->census_generated = 0U;
    if (state_->status == XessGenerationState::installed) {
        state_->status = XessGenerationState::inactive;
    }
#endif
}

XessGenerationState XessFrameGeneration::state() const noexcept
{
    return state_->status;
}

bool XessFrameGeneration::owns_presentation() const noexcept
{
    return state_->status == XessGenerationState::installed;
}

const std::string& XessFrameGeneration::version() const noexcept
{
    return state_->version;
}

const std::string& XessFrameGeneration::detail() const noexcept
{
    return state_->detail;
}

std::uint64_t XessFrameGeneration::estimated_vram_bytes() const noexcept
{
#if MFG_DLSS_HAVE_XEFG_HEADERS
    return state_->estimated_vram_bytes;
#else
    return 0ULL;
#endif
}

std::uint32_t
XessFrameGeneration::maximum_interpolated_frames() const noexcept
{
    return state_->maximum_interpolated_frames;
}

std::uint32_t XessFrameGeneration::maximum_multiplier() const noexcept
{
    return state_->maximum_interpolated_frames == 0U ?
        0U :
        state_->maximum_interpolated_frames + 1U;
}

bool XessFrameGeneration::probe_capability()
{
#if !MFG_DLSS_HAVE_XEFG_HEADERS
    return false;
#else
    if (state_->maximum_interpolated_frames != 0U) {
        return true;
    }
    if (!state_->fg_loader.loaded() &&
        !state_->fg_loader.load(
            kIntelSubdirectory,
            L"libxess_fg.dll",
            true,
            L"Intel Corporation")) {
        return false;
    }
    if (!state_->resolve_exports()) {
        return false;
    }
    if (state_->version.empty()) {
        xefg_swapchain_version_t version{};
        if (xefg_ok(state_->get_version(&version))) {
            state_->version = "XeSS-FG " + std::to_string(version.major) + "." +
                std::to_string(version.minor) + "." +
                std::to_string(version.patch);
        }
    }

    if (state_->handle == nullptr) {
        auto& backend = D3D12Backend::instance();
        auto* const device =
            static_cast<ID3D12Device*>(backend.native_device());
        if (!backend.ready() || device == nullptr) {
            return false;
        }
        const auto created =
            state_->create_context(device, &state_->handle);
        if (!xefg_ok(created) || state_->handle == nullptr) {
            logger::error(
                "xefgSwapChainD3D12CreateContext: {}",
                xefg_result_text(created));
            state_->handle = nullptr;
            return false;
        }
    }

    xefg_swapchain_properties_t properties{};
    const auto queried =
        state_->get_properties(state_->handle, &properties);
    if (!xefg_ok(queried)) {
        logger::error(
            "xefgSwapChainGetProperties: {}", xefg_result_text(queried));
        return false;
    }
    state_->maximum_interpolated_frames = properties.maxSupportedInterpolations;
    state_->estimated_vram_bytes = properties.tempBufferHeapSize +
        properties.tempTextureHeapSize + properties.constantBufferSize;
    if (state_->maximum_interpolated_frames != 0U) {
        logger::info(
            "{} reports a ceiling of {} generated frame(s) on this adapter, so "
            "the highest usable multiplier is {}x",
            state_->version.empty() ? "XeSS-FG" : state_->version,
            state_->maximum_interpolated_frames,
            state_->maximum_interpolated_frames + 1U);
    }
    return state_->maximum_interpolated_frames != 0U;
#endif
}

std::uint32_t XessFrameGeneration::presented_frame_count() const noexcept
{
    return state_->presented_total;
}
}
