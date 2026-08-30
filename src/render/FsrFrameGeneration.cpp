#include "render/FsrFrameGeneration.hpp"

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

#pragma warning(push)
#pragma warning(disable : 4191 4365 4514 4820 5219)
#include <api/include/dx12/ffx_api_dx12.h>
#include <api/include/ffx_api.h>
#include <api/include/ffx_api_types.h>
#include <framegeneration/include/dx12/ffx_api_framegeneration_dx12.h>
#include <framegeneration/include/ffx_framegeneration.h>
#pragma warning(pop)

#include <array>
#include <string>

namespace mfgdlss::render
{
namespace
{
namespace logger = SKSE::log;

using Microsoft::WRL::ComPtr;

using PfnFfxCreateContext = ffxReturnCode_t(*)(
    ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*);
using PfnFfxDestroyContext = ffxReturnCode_t(*)(
    ffxContext*, const ffxAllocationCallbacks*);
using PfnFfxConfigure = ffxReturnCode_t(*)(
    ffxContext*, const ffxConfigureDescHeader*);
using PfnFfxQuery = ffxReturnCode_t(*)(ffxContext*, ffxQueryDescHeader*);
using PfnFfxDispatch = ffxReturnCode_t(*)(
    ffxContext*, const ffxDispatchDescHeader*);

[[nodiscard]] std::string ffx_result_text(const ffxReturnCode_t code)
{
    switch (code) {
    case FFX_API_RETURN_OK: return "ok";
    case FFX_API_RETURN_ERROR: return "generic error";
    case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE:
        return "unknown descriptor type";
    case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "runtime error";
    case FFX_API_RETURN_NO_PROVIDER: return "no provider";
    case FFX_API_RETURN_ERROR_MEMORY: return "out of memory";
    case FFX_API_RETURN_ERROR_PARAMETER: return "invalid parameter";
    case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE:
        return "the installed FidelityFX provider is older than this "
               "integration expects, which AMD documents as usually fixed by "
               "updating the graphics driver or the FidelityFX DLLs";
    default:
        break;
    }
    return "unrecognised code " + std::to_string(static_cast<int>(code));
}

[[nodiscard]] D3D12_RESOURCE_BARRIER transition(
    ID3D12Resource* const resource,
    const D3D12_RESOURCE_STATES before,
    const D3D12_RESOURCE_STATES after) noexcept
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}
}

struct FsrFrameGeneration::State
{
    providers::RuntimeLoader loader;
    providers::RuntimeLoader effect_runtime;

    PfnFfxCreateContext create{};
    PfnFfxDestroyContext destroy{};
    PfnFfxConfigure configure{};
    PfnFfxQuery query{};
    PfnFfxDispatch dispatch{};

    std::uint64_t estimated_vram_bytes{};
    bool installed_depth_reversed{};
    DWORD owner_thread_id{};
    std::uint32_t installed_display_width{};
    std::uint32_t installed_display_height{};
    DXGI_FORMAT installed_back_buffer_format{DXGI_FORMAT_UNKNOWN};
    bool installed_depth_infinite{};
    ffxContext swap_chain_context{};
    ffxContext generation_context{};

    ComPtr<IDXGISwapChain4> wrapped_chain;

    FsrGenerationState status{FsrGenerationState::inactive};
    std::string version;
    std::string detail;
    std::uint64_t frame_id{};
    std::uint64_t suppressed_frames{};
    bool suppression_failure_logged{};
    std::uint32_t requested_frames{};

    std::uint32_t generated_last_frame{};
    bool enabled{true};
    bool reset_pending{};
    bool camera_fov_warning_logged{};
    bool camera_basis_warning_logged{};
    std::uint32_t stale_temporal_inputs_frames{};

    std::string waiting_reason_logged;
    bool first_submission_logged{};

    std::uint32_t census_frames{};
    std::uint32_t census_submitted{};
    std::uint32_t census_generated{};
    std::string census_last_failure;

    void refresh_memory_usage() noexcept
    {
        if (query == nullptr || swap_chain_context == nullptr) {
            return;
        }
        FfxApiEffectMemoryUsage usage{};
        ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12 memory_query{};
        memory_query.header.type =
            FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12;
        memory_query.header.pNext = nullptr;
        memory_query.gpuMemoryUsageFrameGenerationSwapchain = &usage;
        if (query(&swap_chain_context, &memory_query.header) !=
            FFX_API_RETURN_OK) {
            return;
        }
        if (usage.totalUsageInBytes == estimated_vram_bytes) {
            return;
        }
        estimated_vram_bytes = usage.totalUsageInBytes;
        logger::info(
            "FSR frame generation swap chain now uses {} MB of GPU memory "
            "({} MB of that aliasable)",
            usage.totalUsageInBytes / (1024ULL * 1024ULL),
            usage.aliasableUsageInBytes / (1024ULL * 1024ULL));
    }

    void suppress_generation_for_incomplete_frame()
    {
        if (status != FsrGenerationState::installed ||
            configure == nullptr ||
            wrapped_chain == nullptr ||
            !enabled) {

            return;
        }
        ++frame_id;
        ffxConfigureDescFrameGeneration disable{};
        disable.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        disable.header.pNext = nullptr;
        disable.swapChain = wrapped_chain.Get();
        disable.frameGenerationEnabled = false;
        disable.frameID = frame_id;
        const auto result =
            configure(&generation_context, &disable.header);
        if (result != FFX_API_RETURN_OK) {
            if (!suppression_failure_logged) {
                suppression_failure_logged = true;
                logger::error(
                    "ffxConfigure(FrameGeneration, enabled=false) failed while "
                    "suppressing interpolation for an incomplete frame: {}. "
                    "FidelityFX may interpolate this frame from the previous "
                    "frame's configuration",
                    ffx_result_text(result));
            }
            return;
        }
        ++suppressed_frames;
        if (suppressed_frames == 1U || suppressed_frames == 600U ||
            suppressed_frames % 6000U == 0U) {

            logger::info(
                "FidelityFX frame generation was disabled for {} incomplete "
                "frame(s) so far rather than left configured from the "
                "previous frame. AMD documents that the frame id must "
                "increment by exactly one per frame, so the id still advances "
                "on these frames and the sequence stays contiguous. A count "
                "near the total frame count means the inputs are almost never "
                "ready and the reason is on the census line",
                suppressed_frames);
        }
    }

    void note_submission(const bool submitted)
    {
        ++census_frames;
        if (submitted) {
            ++census_submitted;

            if (generated_last_frame != 0U) {
                ++census_generated;
            }
        } else if (!detail.empty()) {
            census_last_failure = detail;
        }
        if (census_frames < 600U) {
            return;
        }
        refresh_memory_usage();
        const auto configured_multiplier = requested_frames + 1U;
        const auto requested_multiplier =
            config::Settings::instance().frame_generation_multiplier();
        logger::info(
            "FidelityFX frame generation census over {} frames: {} prepared "
            "and configured successfully, {} of those had the runtime "
            "generate a frame on the preceding present. Runtime is configured "
            "for {} generated frame(s) per rendered frame ({}x); the settings "
            "ask for {}x{}.{}",
            census_frames,
            census_submitted,
            census_generated,
            requested_frames,
            configured_multiplier,
            requested_multiplier,
            configured_multiplier == requested_multiplier ?
                std::string{} :
                std::string{", so the requested multiplier is not what is "
                            "running"},
            census_last_failure.empty() ?
                std::string{} :
                " Most recent submission failure: " + census_last_failure);
        census_frames = 0U;
        census_submitted = 0U;
        census_generated = 0U;
        census_last_failure.clear();
    }

    [[nodiscard]] bool load_runtime(std::string& failure)
    {

        if (!effect_runtime.loaded() &&
            !effect_runtime.load(
                L"UFGU\\AMD",
                L"amd_fidelityfx_framegeneration_dx12.dll",
                true,
                L"Advanced Micro Devices")) {
            failure = "amd_fidelityfx_framegeneration_dx12.dll: " +
                effect_runtime.failure();
            return false;
        }
        if (!loader.loaded() &&
            !loader.load(
                L"UFGU\\AMD",
                L"amd_fidelityfx_loader_dx12.dll",
                true,
                L"Advanced Micro Devices")) {
            failure =
                "amd_fidelityfx_loader_dx12.dll: " + loader.failure();
            return false;
        }
        create = loader.function<PfnFfxCreateContext>("ffxCreateContext");
        destroy = loader.function<PfnFfxDestroyContext>("ffxDestroyContext");
        configure = loader.function<PfnFfxConfigure>("ffxConfigure");
        query = loader.function<PfnFfxQuery>("ffxQuery");
        dispatch = loader.function<PfnFfxDispatch>("ffxDispatch");
        if (create == nullptr || destroy == nullptr || configure == nullptr ||
            query == nullptr || dispatch == nullptr) {
            failure = "required FidelityFX API export is missing";
            return false;
        }
        return true;
    }
};

FsrFrameGeneration::FsrFrameGeneration() : state_{std::make_unique<State>()}
{
}

FsrFrameGeneration::~FsrFrameGeneration()
{

    static_cast<void>(state_.release());
}

FsrFrameGeneration& FsrFrameGeneration::instance() noexcept
{
    static FsrFrameGeneration generation;
    return generation;
}

bool FsrFrameGeneration::selected() noexcept
{
    return config::Settings::instance().vendor_frame_generation() ==
        config::VendorFrameGeneration::amd;
}

bool FsrFrameGeneration::install_if_ready(
    const std::uint32_t generated_frames_requested)
{
    if (state_->status == FsrGenerationState::installed) {
        const auto live_depth = CameraData::instance().depth_contract();
        auto& live_presentation = PresentationBridge::instance();
        const auto live_width = live_presentation.output_width();
        const auto live_height = live_presentation.output_height();
        const auto live_format = live_presentation.back_buffer_format();
        if (live_width != 0U && live_height != 0U &&
            (live_width != state_->installed_display_width ||
             live_height != state_->installed_display_height)) {
            logger::warn(
                "The presentation extent changed from {}x{} to {}x{} after "
                "FidelityFX frame generation was created. displaySize and "
                "maxRenderSize are fixed at context creation, so the context "
                "is being rebuilt rather than left generating at the old "
                "extent.",
                state_->installed_display_width,
                state_->installed_display_height,
                live_width,
                live_height);
            shutdown();
        } else if (
            live_format != DXGI_FORMAT_UNKNOWN &&
            live_format != state_->installed_back_buffer_format) {
            logger::warn(
                "The back buffer format changed from {} to {} after "
                "FidelityFX frame generation was created. backBufferFormat is "
                "fixed at context creation, so the context is being rebuilt "
                "rather than left generating into a format it was not built "
                "for.",
                static_cast<int>(state_->installed_back_buffer_format),
                static_cast<int>(live_format));
            shutdown();
        } else if (
            live_depth.determinate() &&
            (live_depth.reversed() != state_->installed_depth_reversed ||
             live_depth.infinite() != state_->installed_depth_infinite)) {
            logger::warn(
                "The depth convention changed after FidelityFX frame "
                "generation was created (inverted {} to {}, infinite {} to "
                "{}). Both are fixed at context creation, so the context is "
                "being rebuilt rather than left misreading depth.",
                state_->installed_depth_reversed,
                live_depth.reversed(),
                state_->installed_depth_infinite,
                live_depth.infinite());
            shutdown();
        } else {
            return true;
        }
    }
    if (state_->status == FsrGenerationState::failed) {

        return false;
    }
    if (!selected()) {
        state_->status = FsrGenerationState::inactive;
        return false;
    }

    const auto fail = [this](std::string reason) {
        state_->status = FsrGenerationState::failed;
        state_->detail = std::move(reason);
        logger::error(
            "FidelityFX frame generation installation failed: {}",
            state_->detail);
        return false;
    };
    const auto wait = [this](std::string reason) {
        state_->status = FsrGenerationState::waiting;
        state_->detail = std::move(reason);

        if (state_->waiting_reason_logged != state_->detail) {
            state_->waiting_reason_logged = state_->detail;
            logger::info(
                "FidelityFX frame generation is selected and waiting to "
                "install: {}",
                state_->detail);
        }
        return false;
    };

    std::string failure;
    if (!state_->load_runtime(failure)) {
        return fail(failure);
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
        return wait("the presentation bridge has no swap chain to wrap");
    }

    DXGI_SWAP_CHAIN_DESC1 description{};
    {

        ComPtr<IDXGISwapChain1> described;
        if (FAILED(application_chain->QueryInterface(
                IID_PPV_ARGS(&described))) ||
            described == nullptr ||
            FAILED(described->GetDesc1(&description))) {
            return fail(
                "the presentation swap chain description is unavailable");
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

    IDXGISwapChain4* chain_pointer{};
    const auto interface_result =
        handed_over->QueryInterface(IID_PPV_ARGS(&chain_pointer));
    handed_over->Release();
    if (FAILED(interface_result) || chain_pointer == nullptr) {
        return fail_after_handoff(
            "the presentation swap chain does not expose IDXGISwapChain4, "
            "which the wrap descriptor requires");
    }

    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 wrap_version{};
    wrap_version.header.type =
        FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    wrap_version.header.pNext = nullptr;
    wrap_version.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;

    ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrap{};
    wrap.header.type =
        FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
    wrap.header.pNext = &wrap_version.header;
    wrap.swapchain = &chain_pointer;
    wrap.gameQueue = queue;

    auto result = state_->create(
        &state_->swap_chain_context, &wrap.header, nullptr);
    if (result != FFX_API_RETURN_OK || chain_pointer == nullptr) {

        state_->wrapped_chain.Reset();
        return fail_after_handoff(
            "ffxCreateContext(FrameGenerationSwapChainWrapDX12): " +
            ffx_result_text(result));
    }
    state_->wrapped_chain.Attach(chain_pointer);

    {
        FfxApiSwapchainFramePacingTuning pacing{};
        pacing.safetyMarginInMs = 0.01F;
        pacing.varianceFactor = 0.3F;
        pacing.allowHybridSpin = false;
        pacing.hybridSpinTime = 2U;
        pacing.allowWaitForSingleObjectOnFence = false;

        ffxConfigureDescFrameGenerationSwapChainKeyValueDX12 pacing_config{};
        pacing_config.header.type =
            FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12;
        pacing_config.header.pNext = nullptr;
        pacing_config.key = FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING;
        pacing_config.u64 = 0U;
        pacing_config.ptr = &pacing;

        const auto pacing_result = state_->configure(
            &state_->swap_chain_context, &pacing_config.header);
        if (pacing_result != FFX_API_RETURN_OK) {
            logger::warn(
                "ffxConfigure(FramePacingTuning) failed ({}); FSR frame pacing "
                "keeps AMD's default tuning, which is documented to stay "
                "locked to a slow target after a complex scene becomes simple "
                "again",
                ffx_result_text(pacing_result));
        } else {
            logger::info(
                "FSR frame pacing tuned: safety margin {}ms, variance factor "
                "{}. AMD's default gets stuck at a low target after a camera "
                "pan from a complex scene back to a simple one; this recovers "
                "the frame rate for a small increase in frame to frame "
                "variance.",
                pacing.safetyMarginInMs,
                pacing.varianceFactor);
        }
    }

    state_->refresh_memory_usage();

    if (!PresentationBridge::instance().adopt_presentation_swap_chain(
            chain_pointer)) {
        shutdown();
        return fail_after_handoff(
            "the presentation bridge refused the wrapped swap chain");
    }

    ffxCreateBackendDX12Desc backend_desc{};
    backend_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend_desc.header.pNext = nullptr;
    backend_desc.device = device;

    ffxCreateContextDescFrameGenerationVersion version_desc{};
    version_desc.header.type =
        FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    version_desc.header.pNext = &backend_desc.header;
    version_desc.version = FFX_FRAMEGENERATION_VERSION;

    ffxCreateContextDescFrameGeneration create_generation{};
    create_generation.header.type =
        FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    create_generation.header.pNext = &version_desc.header;
    create_generation.displaySize = {description.Width, description.Height};

    create_generation.maxRenderSize = {description.Width, description.Height};
    create_generation.backBufferFormat =
        ffxApiGetSurfaceFormatDX12(description.Format);
    create_generation.flags =
        depth.reversed() ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0U;
    if (depth.infinite()) {
        create_generation.flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
    }

    result = state_->create(
        &state_->generation_context, &create_generation.header, nullptr);
    if (result != FFX_API_RETURN_OK) {
        shutdown();
        return fail(
            "ffxCreateContext(FrameGeneration): " + ffx_result_text(result));
    }

    logger::info(
        "FidelityFX frame generation context created against API version "
        "{}.{}.{}. AMD requires this version descriptor to be linked at "
        "context creation and states that omitting it prevents the newer "
        "APIs from functioning, which includes the V2 prepare dispatch this "
        "plugin uses to supply camera position and orientation",
        FFX_FRAMEGENERATION_VERSION_MAJOR,
        FFX_FRAMEGENERATION_VERSION_MINOR,
        FFX_FRAMEGENERATION_VERSION_PATCH);

    if (generated_frames_requested > 1U) {
        logger::info(
            "FidelityFX frame generation was asked for {} generated frames "
            "and will produce 1: this runtime is 2x only, on every GPU",
            generated_frames_requested);
    }
    state_->requested_frames = 1U;
    state_->installed_depth_reversed = depth.reversed();
    state_->owner_thread_id = GetCurrentThreadId();
    state_->installed_depth_infinite = depth.infinite();
    state_->installed_display_width = description.Width;
    state_->installed_display_height = description.Height;
    state_->installed_back_buffer_format = description.Format;
    state_->frame_id = 0U;
    state_->suppressed_frames = 0ULL;
    state_->suppression_failure_logged = false;
    state_->reset_pending = true;
    state_->status = FsrGenerationState::installed;
    state_->version = "FidelityFX frame generation";
    state_->detail.clear();
    logger::info(
        "FidelityFX frame generation wrapped this plugin's D3D12 swap chain: "
        "display {}x{}, 1 generated frame (2x), depth={}{}. Skyrim and ENB "
        "continue to present through SwapChainProxy and observe no change.",
        description.Width,
        description.Height,
        depth.reversed() ? "inverted" : "standard",
        depth.infinite() ? ", infinite far plane" : "");
    return true;
}

bool FsrFrameGeneration::submit_frame(
    void* const command_list,
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    const float frame_time_milliseconds,
    const bool reset_history_requested)
{
    if (state_->status != FsrGenerationState::installed || !state_->enabled) {
        return false;
    }
    auto* const graphics_list =
        static_cast<ID3D12GraphicsCommandList*>(command_list);
    if (graphics_list == nullptr) {
        return false;
    }

    bool submitted = false;
    const struct Census {
        State& state;
        const bool& submitted;
        ~Census()
        {
            if (!submitted) {
                state.suppress_generation_for_incomplete_frame();
            }
            state.note_submission(submitted);
        }
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
                "FidelityFX frame generation",
                count);
        }
    }

    auto* const hudless =
        static_cast<ID3D12Resource*>(resources.frame_generation_hudless());
    auto* const depth_resource =
        static_cast<ID3D12Resource*>(resources.depth());

    auto* const motion =
        static_cast<ID3D12Resource*>(resources.vendor_motion_vectors());
    auto* const ui = static_cast<ID3D12Resource*>(resources.ui_color_alpha());
    if (hudless == nullptr || depth_resource == nullptr ||
        motion == nullptr || ui == nullptr) {
        state_->detail = std::string{"a shared input is missing:"} +
            (hudless == nullptr ? " HUD-less colour" : "") +
            (depth_resource == nullptr ? " depth" : "") +
            (motion == nullptr ? " motion vectors" : "") +
            (ui == nullptr ? " UI colour" : "");
        return false;
    }
    resources.log_vendor_input_provenance("FidelityFX");

    float near_plane{};
    float far_plane{};
    if (!CameraData::instance().camera_planes(near_plane, far_plane)) {
        state_->detail =
            "the camera near and far planes were not available this frame";
        return false;
    }

    float camera_fov_vertical{};
    if (!CameraData::instance().camera_fov_vertical(camera_fov_vertical)) {
        if (!state_->camera_fov_warning_logged) {
            state_->camera_fov_warning_logged = true;
            logger::warn(
                "FidelityFX frame generation has no usable vertical field of "
                "view this frame, so interpolation is skipped rather than "
                "given a zero angle, which FidelityFX turns into an infinite "
                "cotangent while reconstructing position");
        }
        state_->detail =
            "the vertical field of view was not available this frame";
        return false;
    }

    ++state_->frame_id;

    ffxConfigureDescFrameGeneration generation_config{};
    generation_config.header.type =
        FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    generation_config.header.pNext = nullptr;
    generation_config.swapChain = state_->wrapped_chain.Get();
    generation_config.frameGenerationEnabled = state_->enabled;
    generation_config.allowAsyncWorkloads = false;

    generation_config.HUDLessColor =
        ffxApiGetResourceDX12(hudless, FFX_API_RESOURCE_STATE_COMMON);
    generation_config.flags = 0U;
    generation_config.onlyPresentGenerated = false;
    generation_config.frameID = state_->frame_id;

    generation_config.frameGenerationCallback =
        [](ffxDispatchDescFrameGeneration* params,
           void* user_context) -> ffxReturnCode_t {
        auto* const owner = static_cast<State*>(user_context);
        if (owner == nullptr || owner->dispatch == nullptr) {
            return FFX_API_RETURN_ERROR_PARAMETER;
        }

        params->backbufferTransferFunction =
            FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        owner->generated_last_frame = params->numGeneratedFrames;
        return owner->dispatch(&owner->generation_context, &params->header);
    };
    generation_config.frameGenerationCallbackUserContext = state_.get();

    generation_config.generationRect = {
        0, 0, static_cast<int32_t>(output_width),
        static_cast<int32_t>(output_height)};
    auto result = state_->configure(
        &state_->generation_context, &generation_config.header);
    if (result != FFX_API_RETURN_OK) {
        state_->detail =
            "ffxConfigure(FrameGeneration): " + ffx_result_text(result);
        return false;
    }

    ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 ui_config{};
    ui_config.header.type =
        FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
    ui_config.header.pNext = nullptr;
    ui_config.uiResource =
        ffxApiGetResourceDX12(ui, FFX_API_RESOURCE_STATE_COMMON);

    ui_config.flags =
        FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA |
        FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;
    result = state_->configure(
        &state_->swap_chain_context, &ui_config.header);
    if (result != FFX_API_RETURN_OK) {
        state_->detail =
            "ffxConfigure(RegisterUiResource): " + ffx_result_text(result);
        return false;
    }

    const std::array<D3D12_RESOURCE_BARRIER, 2U> to_read{
        transition(
            depth_resource,
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(
            motion,
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    graphics_list->ResourceBarrier(
        static_cast<UINT>(to_read.size()), to_read.data());

    const auto jitter = CameraData::instance().frame_jitter();
    ffxDispatchDescFrameGenerationPrepareV2 prepare{};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prepare.header.pNext = nullptr;
    prepare.frameID = state_->frame_id;
    prepare.flags = 0U;
    prepare.commandList = graphics_list;
    prepare.renderSize = {render_width, render_height};

    prepare.jitterOffset = {jitter.pixels_x, jitter.pixels_y};

    const auto& settings = config::Settings::instance();
    prepare.motionVectorScale = {
        static_cast<float>(render_width) * settings.motion_scale_x(),
        static_cast<float>(render_height) * settings.motion_scale_y()};
    prepare.frameTimeDelta = frame_time_milliseconds;
    prepare.reset = reset_history_requested || state_->reset_pending;
    prepare.cameraNear = near_plane;
    prepare.cameraFar = far_plane;
    prepare.cameraFovAngleVertical = camera_fov_vertical;
    prepare.viewSpaceToMetersFactor = 1.0F;
    if (!CameraData::instance().camera_basis(
            prepare.cameraPosition,
            prepare.cameraUp,
            prepare.cameraRight,
            prepare.cameraForward)) {

        if (!state_->camera_basis_warning_logged) {
            state_->camera_basis_warning_logged = true;
            logger::warn(
                "FidelityFX frame generation has no world-space camera "
                "position or orientation this frame. AMD documents those four "
                "vectors as required for the prepare dispatch, so the frame is "
                "skipped rather than dispatched with an origin camera that "
                "would place the whole scene at the world origin facing "
                "nowhere");
        }
        state_->detail =
            "the world-space camera basis was not available this frame";
        return false;
    }
    prepare.depth = ffxApiGetResourceDX12(
        depth_resource, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    prepare.motionVectors = ffxApiGetResourceDX12(
        motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);

    result = state_->dispatch(
        &state_->generation_context, &prepare.header);

    const std::array<D3D12_RESOURCE_BARRIER, 2U> to_common{
        transition(
            depth_resource,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COMMON),
        transition(
            motion,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COMMON)};
    graphics_list->ResourceBarrier(
        static_cast<UINT>(to_common.size()), to_common.data());

    if (result != FFX_API_RETURN_OK) {
        state_->detail =
            "ffxDispatch(FrameGenerationPrepareV2): " + ffx_result_text(result);
        return false;
    }

    state_->reset_pending = false;
    if (!state_->first_submission_logged && state_->generated_last_frame != 0U) {
        state_->first_submission_logged = true;

        logger::info(
            "FidelityFX frame generation is running: render {}x{}, the runtime "
            "generated {} frame(s) on its last present{}",
            render_width,
            render_height,
            state_->generated_last_frame,
            state_->generated_last_frame == state_->requested_frames ?
                "" :
                " -- which does NOT match the 1 this integration assumes");
    }
    state_->detail.clear();
    submitted = true;
    return true;
}

void FsrFrameGeneration::set_enabled(const bool enabled) noexcept
{
    state_->enabled = enabled;
    if (!enabled) {
        state_->generated_last_frame = 0U;
    }
    if (state_->status != FsrGenerationState::installed ||
        state_->configure == nullptr) {
        return;
    }
    if (enabled) {

        state_->reset_pending = true;
        return;
    }

    ffxConfigureDescFrameGeneration generation_config{};
    generation_config.header.type =
        FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    generation_config.header.pNext = nullptr;
    generation_config.swapChain = state_->wrapped_chain.Get();
    generation_config.frameGenerationEnabled = false;
    ++state_->frame_id;
    generation_config.frameID = state_->frame_id;
    const auto result = state_->configure(
        &state_->generation_context, &generation_config.header);
    if (result != FFX_API_RETURN_OK) {
        logger::error(
            "ffxConfigure(FrameGeneration, enabled=false): {}",
            ffx_result_text(result));
    }
}

std::uint64_t FsrFrameGeneration::estimated_vram_bytes() const noexcept
{
    return state_->estimated_vram_bytes;
}

void FsrFrameGeneration::reset_history() noexcept
{
    state_->reset_pending = true;
}

void FsrFrameGeneration::shutdown() noexcept
{
    if (state_->owner_thread_id != 0U &&
        state_->owner_thread_id != GetCurrentThreadId()) {
        logger::critical(
            "{} teardown was reached on thread {} but its runtime contexts "
            "were created on thread {}. The contexts are still being "
            "destroyed, because leaving them alive would leak the swap chain "
            "across an ENB reload, but if a crash follows this line the "
            "cross-thread teardown is the first thing to suspect",
            "FidelityFX frame generation",
            GetCurrentThreadId(),
            state_->owner_thread_id);
    }

    if (state_->configure != nullptr &&
        state_->generation_context != nullptr &&
        state_->wrapped_chain != nullptr) {

        ffxConfigureDescFrameGeneration disable{};
        disable.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        disable.header.pNext = nullptr;
        disable.swapChain = state_->wrapped_chain.Get();
        disable.frameGenerationEnabled = false;
        ++state_->frame_id;
        disable.frameID = state_->frame_id;
        const auto disabled =
            state_->configure(&state_->generation_context, &disable.header);
        if (disabled != FFX_API_RETURN_OK) {
            logger::warn(
                "ffxConfigure(FrameGeneration, enabled=false) failed during "
                "teardown: {}. AMD's own shutdown sample disables frame "
                "generation before destroying the context, so the context is "
                "being destroyed in a state AMD does not document",
                ffx_result_text(disabled));
        }
    }

    if (state_->destroy != nullptr && state_->generation_context != nullptr) {
        static_cast<void>(
            state_->destroy(&state_->generation_context, nullptr));
    }
    state_->generation_context = nullptr;

    if (state_->destroy != nullptr && state_->swap_chain_context != nullptr) {
        static_cast<void>(
            state_->destroy(&state_->swap_chain_context, nullptr));
    }
    state_->swap_chain_context = nullptr;

    const auto had_wrapped_chain = state_->wrapped_chain != nullptr;
    if (had_wrapped_chain) {
        const auto remaining = state_->wrapped_chain.Reset();
        if (remaining != 0U) {
            logger::warn(
                "The FidelityFX frame generation swap chain still holds {} "
                "outstanding reference(s) after this plugin released its own; "
                "something else is keeping the presentation chain alive past "
                "teardown",
                remaining);
        }
    }

    if (had_wrapped_chain) {
        PresentationBridge::instance().release_presentation_swap_chain();
    }
    state_->frame_id = 0U;
    state_->suppressed_frames = 0ULL;
    state_->suppression_failure_logged = false;
    state_->requested_frames = 0U;
    state_->estimated_vram_bytes = 0ULL;
    state_->generated_last_frame = 0U;
    state_->first_submission_logged = false;
    state_->census_frames = 0U;
    state_->census_submitted = 0U;
    state_->census_generated = 0U;
    state_->census_last_failure.clear();
    state_->camera_fov_warning_logged = false;
    state_->camera_basis_warning_logged = false;
    state_->stale_temporal_inputs_frames = 0U;
    state_->waiting_reason_logged.clear();
    if (state_->status == FsrGenerationState::installed) {
        state_->status = FsrGenerationState::inactive;
    }
}

FsrGenerationState FsrFrameGeneration::state() const noexcept
{
    return state_->status;
}

bool FsrFrameGeneration::owns_presentation() const noexcept
{
    return state_->status == FsrGenerationState::installed;
}

const std::string& FsrFrameGeneration::version() const noexcept
{
    return state_->version;
}

const std::string& FsrFrameGeneration::detail() const noexcept
{
    return state_->detail;
}

std::uint32_t FsrFrameGeneration::generated_frames() const noexcept
{

    return state_->generated_last_frame;
}

std::uint32_t FsrFrameGeneration::maximum_multiplier() noexcept
{

    return 2U;
}
}
