#include "providers/FsrUpscaler.hpp"

#include "config/Settings.hpp"
#include "providers/RuntimeLoader.hpp"
#include "render/D3D12Backend.hpp"
#include "render/SharedResources.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <SKSE/SKSE.h>

#pragma warning(push)
#pragma warning(disable : 4191 4365 4514 4820 5219)
#include <api/include/ffx_api.h>
#include <api/include/ffx_api_types.h>
#include <api/include/dx12/ffx_api_dx12.h>
#include <upscalers/include/ffx_upscale.h>
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string_view>
#include <vector>

namespace mfgdlss::providers
{
namespace
{
namespace logger = SKSE::log;
using Microsoft::WRL::ComPtr;

constexpr std::size_t kCommandSlotCount = 4;

[[nodiscard]] std::uint32_t upscale_context_flags() noexcept
{
    auto flags =
        static_cast<std::uint32_t>(FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION);
    if (config::Settings::instance().fsr_color_space() ==
        config::FsrColorSpace::non_linear_srgb) {
        flags |=
            static_cast<std::uint32_t>(
                FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE);
    } else {
        flags |=
            static_cast<std::uint32_t>(FFX_UPSCALE_ENABLE_AUTO_EXPOSURE);
    }
    return flags;
}

void ffx_message_bridge(const uint32_t type, const wchar_t* const message)
{
    if (message == nullptr) {
        return;
    }
    const auto wide_length = static_cast<int>(std::wcslen(message));
    if (wide_length <= 0) {
        return;
    }
    const auto needed = WideCharToMultiByte(
        CP_UTF8, 0, message, wide_length, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return;
    }
    std::string text(static_cast<std::size_t>(needed), '\0');
    static_cast<void>(WideCharToMultiByte(
        CP_UTF8, 0, message, wide_length, text.data(), needed,
        nullptr, nullptr));
    while (!text.empty() &&
           (text.back() == '\n' || text.back() == '\r' ||
            text.back() == '\t' || text.back() == ' ')) {
        text.pop_back();
    }
    if (text.empty()) {
        return;
    }
    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        logger::error("[FidelityFX] {}", text);
        return;
    }
    logger::warn("[FidelityFX] {}", text);
}

[[nodiscard]] std::string_view ffx_result_name(
    const ffxReturnCode_t result) noexcept
{
    switch (result) {
    case FFX_API_RETURN_OK: return "success";
    case FFX_API_RETURN_ERROR: return "unspecified-error";
    case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE:
        return "unknown-descriptor-type";
    case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "runtime-error";
    case FFX_API_RETURN_NO_PROVIDER: return "no-provider";
    case FFX_API_RETURN_ERROR_MEMORY: return "out-of-memory";
    case FFX_API_RETURN_ERROR_PARAMETER: return "invalid-parameter";
    case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE:
        return "provider-too-old";
    }
    return "unrecognized-result";
}

[[nodiscard]] Availability availability_for_result(
    const ffxReturnCode_t result) noexcept
{
    switch (result) {
    case FFX_API_RETURN_NO_PROVIDER:
        return Availability::adapter_unsupported;
    case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE:
    case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE:
        return Availability::runtime_version_mismatch;
    case FFX_API_RETURN_ERROR_PARAMETER:
        return Availability::required_input_unavailable;
    default:
        return Availability::context_creation_failed;
    }
}

[[nodiscard]] bool to_fsr_quality(
    const QualityMode quality,
    std::uint32_t& result) noexcept
{
    switch (quality) {
    case QualityMode::native_antialiasing:
        result = FFX_UPSCALE_QUALITY_MODE_NATIVEAA;
        return true;
    case QualityMode::quality:
        result = FFX_UPSCALE_QUALITY_MODE_QUALITY;
        return true;
    case QualityMode::balanced:
        result = FFX_UPSCALE_QUALITY_MODE_BALANCED;
        return true;
    case QualityMode::performance:
        result = FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;
        return true;
    case QualityMode::ultra_performance:
        result = FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE;
        return true;
    case QualityMode::off:
    case QualityMode::ultra_quality_plus:
    case QualityMode::ultra_quality:
        break;
    }
    return false;
}

[[nodiscard]] D3D12_RESOURCE_BARRIER transition(
    ID3D12Resource* const resource,
    const D3D12_RESOURCE_STATES before,
    const D3D12_RESOURCE_STATES after) noexcept
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

[[nodiscard]] bool validate_texture(
    ID3D12Resource* const resource,
    const std::uint32_t minimum_width,
    const std::uint32_t minimum_height,
    const DXGI_FORMAT expected_format,
    const D3D12_RESOURCE_FLAGS required_flags,
    const std::string_view label,
    std::string& failure)
{
    if (resource == nullptr) {
        failure = std::string(label) + " is unavailable";
        return false;
    }

    const auto description = resource->GetDesc();
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        description.Width < minimum_width ||
        description.Height < minimum_height ||
        description.DepthOrArraySize != 1 ||
        description.MipLevels != 1 ||
        description.SampleDesc.Count != 1 ||
        description.Format != expected_format ||
        (description.Flags & required_flags) != required_flags) {
        char text[320]{};
        static_cast<void>(std::snprintf(
            text,
            sizeof(text),
            "%.*s contract mismatch: dimension=%u extent=%llux%u "
            "array=%u mips=%u samples=%u format=%u flags=0x%X; expected "
            "2D >=%ux%u array=1 mips=1 samples=1 format=%u flags&0x%X",
            static_cast<int>((std::min)(
                label.size(),
                static_cast<std::size_t>((std::numeric_limits<int>::max)()))),
            label.data(),
            static_cast<unsigned>(description.Dimension),
            static_cast<unsigned long long>(description.Width),
            static_cast<unsigned>(description.Height),
            static_cast<unsigned>(description.DepthOrArraySize),
            static_cast<unsigned>(description.MipLevels),
            static_cast<unsigned>(description.SampleDesc.Count),
            static_cast<unsigned>(description.Format),
            static_cast<unsigned>(description.Flags),
            minimum_width,
            minimum_height,
            static_cast<unsigned>(expected_format),
            static_cast<unsigned>(required_flags)));
        failure = text;
        return false;
    }
    return true;
}

[[nodiscard]] bool validate_resource_device(
    ID3D12Resource* const resource,
    ID3D12Device* const expected_device,
    const std::string_view label,
    std::string& failure)
{
    ComPtr<ID3D12Device> owner;
    if (resource == nullptr ||
        FAILED(resource->GetDevice(IID_PPV_ARGS(&owner))) ||
        owner.Get() != expected_device) {
        failure = std::string(label) +
                  " does not belong to the FidelityFX D3D12 device";
        return false;
    }
    return true;
}

[[nodiscard]] float frame_delta_milliseconds(
    std::chrono::steady_clock::time_point& previous,
    bool& measured) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (!measured) {
        previous = now;
        measured = true;
        return 16.6667F;
    }
    const auto elapsed = std::chrono::duration<float, std::milli>(
        now - previous).count();
    previous = now;
    if (!std::isfinite(elapsed)) {
        return 16.6667F;
    }
    return (std::clamp)(elapsed, 1.0F, 100.0F);
}
}

struct FsrUpscaler::State
{
    struct CommandSlot
    {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        std::uint64_t fence_value{};
    };

    [[nodiscard]] static RuntimeLoader& loader_runtime() noexcept
    {
        static RuntimeLoader loader;
        return loader;
    }

    [[nodiscard]] static RuntimeLoader& effect_runtime_loader() noexcept
    {
        static RuntimeLoader effect_runtime;
        return effect_runtime;
    }

    PfnFfxCreateContext create{};
    PfnFfxDestroyContext destroy{};
    PfnFfxConfigure configure{};
    PfnFfxQuery query{};
    PfnFfxDispatch dispatch{};
    ffxContext context{};

    ffxCreateContextDescUpscale create_desc{};
    ffxCreateBackendDX12Desc backend_desc{};
    ffxCreateContextDescUpscaleVersion version_desc{};

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    std::array<CommandSlot, kCommandSlotCount> slots;
    std::uint64_t next_fence_value{1};
    std::size_t next_slot{};
    QualityMode quality{QualityMode::off};
    std::uint32_t render_width{};
    std::uint32_t render_height{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    bool reversed_depth{};
    bool infinite_depth{};

    bool reset_on_next_dispatch{};
    bool first_dispatch_logged{};

    std::uint64_t dispatch_count{};
    std::uint64_t window_dispatches{};
    std::uint64_t window_resets{};
    std::uint64_t window_zero_jitter{};
    DWORD owner_thread_id{};
    std::array<ID3D12Resource*, 6> validated_resources{};
    std::chrono::steady_clock::time_point previous_dispatch{};
    bool dispatch_time_measured{};
    HANDLE fence_event{};

    ~State() noexcept
    {
        const auto safe_to_destroy = wait_for_idle(5000);
        if (context != nullptr && destroy != nullptr && safe_to_destroy) {
            static_cast<void>(destroy(&context, nullptr));
            context = nullptr;
        }
        if (fence_event != nullptr) {
            static_cast<void>(CloseHandle(fence_event));
            fence_event = nullptr;
        }
    }

    [[nodiscard]] bool wait_for_idle(const DWORD timeout_ms) noexcept
    {
        if (fence == nullptr) {
            return true;
        }
        std::uint64_t final_value{};
        for (const auto& slot : slots) {
            if (slot.fence_value > final_value) {
                final_value = slot.fence_value;
            }
        }
        if (final_value == 0 || fence->GetCompletedValue() >= final_value) {
            return true;
        }
        if (fence_event == nullptr) {
            fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }
        if (fence_event == nullptr ||
            FAILED(fence->SetEventOnCompletion(final_value, fence_event))) {
            return false;
        }
        return WaitForSingleObject(fence_event, timeout_ms) == WAIT_OBJECT_0;
    }

    [[nodiscard]] bool load_runtime(std::string& failure)
    {

        auto& effect_runtime = effect_runtime_loader();
        auto& loader = loader_runtime();
        if (!effect_runtime.loaded() &&
            !effect_runtime.load(
                L"UFGU\\AMD",
                L"amd_fidelityfx_upscaler_dx12.dll",
                true,
                L"Advanced Micro Devices")) {
            failure = "amd_fidelityfx_upscaler_dx12.dll: " +
                      effect_runtime.failure();
            return false;
        }
        if (!loader.loaded() &&
            !loader.load(
                L"UFGU\\AMD",
                L"amd_fidelityfx_loader_dx12.dll",
                true,
                L"Advanced Micro Devices")) {
            failure = "amd_fidelityfx_loader_dx12.dll: " + loader.failure();
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

    [[nodiscard]] bool create_command_slots(std::string& failure)
    {
        auto hr = device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(hr)) {
            failure = "D3D12 fence creation failed";
            return false;
        }
        fence->SetName(L"Universal Upscaling FidelityFX dispatch fence");

        for (auto& slot : slots) {
            hr = device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&slot.allocator));
            if (FAILED(hr)) {
                failure = "D3D12 command allocator creation failed";
                return false;
            }
            hr = device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                slot.allocator.Get(),
                nullptr,
                IID_PPV_ARGS(&slot.list));
            if (FAILED(hr) || FAILED(slot.list->Close())) {
                failure = "D3D12 command list creation failed";
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] CommandSlot* acquire_slot() noexcept
    {
        const auto completed = fence->GetCompletedValue();
        for (std::size_t offset = 0; offset < slots.size(); ++offset) {
            const auto index = (next_slot + offset) % slots.size();
            auto& slot = slots[index];
            if (slot.fence_value == 0 || completed >= slot.fence_value) {
                next_slot = (index + 1) % slots.size();
                return &slot;
            }
        }
        return nullptr;
    }
};

FsrUpscaler& FsrUpscaler::instance() noexcept
{
    static FsrUpscaler upscaler;
    return upscaler;
}

FsrUpscaler::~FsrUpscaler()
{
    if (state_ != nullptr) {

        static_cast<void>(state_.release());
    }
}

bool FsrUpscaler::supports_render_resolution(
    const QualityMode quality,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    const std::uint32_t render_width,
    const std::uint32_t render_height)
{
    std::uint32_t fsr_quality{};
    if (!to_fsr_quality(quality, fsr_quality) || output_width == 0 ||
        output_height == 0 || render_width == 0 || render_height == 0) {
        return false;
    }

    const auto supported =
        render_width <= output_width && render_height <= output_height;
    logger::info(
        "AMD FidelityFX {} would {} an explicit {}x{} input for a {}x{} "
        "output",
        quality_mode_key(quality),
        supported ? "accept" : "refuse",
        render_width,
        render_height,
        output_width,
        output_height);
    return supported;
}

bool FsrUpscaler::query_render_resolution(
    const QualityMode quality,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    std::uint32_t& render_width,
    std::uint32_t& render_height)
{
    render_width = 0;
    render_height = 0;
    if (state_ != nullptr && state_->context != nullptr &&
        state_->quality == quality &&
        state_->output_width == output_width &&
        state_->output_height == output_height) {
        render_width = state_->render_width;
        render_height = state_->render_height;
        return render_width != 0 && render_height != 0;
    }

    std::uint32_t fsr_quality{};
    if (!to_fsr_quality(quality, fsr_quality) || output_width == 0 ||
        output_height == 0) {
        availability_ = Availability::required_input_unavailable;
        detail_ = "invalid FidelityFX quality or output extent";
        return false;
    }

    State query_state;
    query_state.owner_thread_id = GetCurrentThreadId();
    if (!query_state.load_runtime(detail_)) {
        availability_ =
            detail_.find("signature verification failed") != std::string::npos ?
                Availability::runtime_rejected_signature :
                Availability::runtime_missing;
        return false;
    }

    auto& backend = render::D3D12Backend::instance();
    auto* const device =
        static_cast<ID3D12Device*>(backend.native_device());
    if (!backend.ready() || device == nullptr) {
        availability_ = Availability::d3d12_unavailable;
        detail_ = "the native D3D12 device is unavailable";
        return false;
    }

    query_state.create_desc.header.type =
        FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    query_state.create_desc.header.pNext =
        &query_state.backend_desc.header;

    query_state.create_desc.flags = upscale_context_flags();

    query_state.create_desc.maxRenderSize = {output_width, output_height};
    query_state.create_desc.maxUpscaleSize = {output_width, output_height};
    query_state.create_desc.fpMessage = &ffx_message_bridge;

    query_state.backend_desc.header.type =
        FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    query_state.backend_desc.header.pNext =
        &query_state.version_desc.header;
    query_state.backend_desc.device = device;

    query_state.version_desc.header.type =
        FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    query_state.version_desc.header.pNext = nullptr;
    query_state.version_desc.version = FFX_UPSCALER_VERSION;

    const auto create_result = query_state.create(
        &query_state.context, &query_state.create_desc.header, nullptr);
    if (create_result != FFX_API_RETURN_OK ||
        query_state.context == nullptr) {
        availability_ = availability_for_result(create_result);
        detail_ = "ffxCreateContext for extent query: ";
        detail_ += ffx_result_name(create_result);
        return false;
    }

    ffxQueryDescUpscaleGetRenderResolutionFromQualityMode extent_query{};
    extent_query.header.type =
        FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
    extent_query.displayWidth = output_width;
    extent_query.displayHeight = output_height;
    extent_query.qualityMode = fsr_quality;
    extent_query.pOutRenderWidth = &render_width;
    extent_query.pOutRenderHeight = &render_height;
    const auto extent_result = query_state.query(
        &query_state.context, &extent_query.header);
    if (extent_result != FFX_API_RETURN_OK || render_width == 0 ||
        render_height == 0 || render_width > output_width ||
        render_height > output_height) {
        render_width = 0;
        render_height = 0;
        availability_ = Availability::required_input_unavailable;
        detail_ = "FidelityFX render-resolution query: ";
        detail_ += ffx_result_name(extent_result);
        return false;
    }

    availability_ = Availability::available;
    detail_.clear();
    logger::info(
        "AMD FidelityFX runtime selected its {} input extent: {}x{} -> "
        "{}x{}",
        quality_mode_key(quality),
        render_width,
        render_height,
        output_width,
        output_height);
    return true;
}

bool FsrUpscaler::configure(
    const QualityMode quality,
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    const bool reversed_depth,
    const bool infinite_depth)
{
    if (state_ != nullptr &&
        state_->owner_thread_id != GetCurrentThreadId()) {
        detail_ = "FidelityFX configuration moved to a different render thread";
        return false;
    }

    const auto live_context_flags =
        upscale_context_flags() |
        (reversed_depth ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0U) |
        (infinite_depth ? FFX_UPSCALE_ENABLE_DEPTH_INFINITE : 0U);
    if (state_ != nullptr && state_->context != nullptr &&
        state_->create_desc.flags != live_context_flags) {
        logger::info(
            "AMD FidelityFX context flags changed from 0x{:08X} to 0x{:08X}, "
            "so the context is being rebuilt. Colour space and depth "
            "convention are fixed when the context is created.",
            state_->create_desc.flags,
            live_context_flags);
        shutdown();
    }
    if (state_ != nullptr && state_->context != nullptr &&
        state_->output_width == output_width &&
        state_->output_height == output_height &&
        state_->reversed_depth == reversed_depth &&
        state_->infinite_depth == infinite_depth &&
        render_width != 0 && render_height != 0 &&
        render_width <= state_->output_width &&
        render_height <= state_->output_height) {
        if (state_->quality != quality ||
            state_->render_width != render_width ||
            state_->render_height != render_height) {
            logger::info(
                "AMD FidelityFX quality changed without rebuilding the "
                "context: {} {}x{} -> {} {}x{} inside the {}x{} allocation. "
                "No context recreation, no reallocation, no restart.",
                quality_mode_key(state_->quality),
                state_->render_width,
                state_->render_height,
                quality_mode_key(quality),
                render_width,
                render_height,
                state_->output_width,
                state_->output_height);
            state_->quality = quality;
            state_->render_width = render_width;
            state_->render_height = render_height;

            state_->reset_on_next_dispatch = true;
        }
        return true;
    }
    shutdown();

    std::uint32_t fsr_quality{};
    if (!to_fsr_quality(quality, fsr_quality) || render_width == 0 ||
        render_height == 0 || output_width == 0 || output_height == 0 ||
        render_width > output_width || render_height > output_height) {
        availability_ = Availability::required_input_unavailable;
        detail_ = "invalid FidelityFX quality or render contract";
        return false;
    }

    auto state = std::make_unique<State>();
    state->owner_thread_id = GetCurrentThreadId();
    if (!state->load_runtime(detail_)) {
        availability_ =
            detail_.find("signature verification failed") != std::string::npos ?
                Availability::runtime_rejected_signature :
                Availability::runtime_missing;
        return false;
    }

    auto& backend = render::D3D12Backend::instance();
    auto* device = static_cast<ID3D12Device*>(backend.native_device());
    auto* queue = static_cast<ID3D12CommandQueue*>(backend.command_queue());
    if (!backend.ready() || device == nullptr || queue == nullptr) {
        availability_ = Availability::d3d12_unavailable;
        detail_ = "the native D3D12 device or graphics queue is unavailable";
        return false;
    }
    state->device = device;
    state->queue = queue;

    state->create_desc.header.type =
        FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    state->create_desc.header.pNext = &state->backend_desc.header;
    state->create_desc.flags =
        upscale_context_flags() |
        (reversed_depth ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0U) |
        (infinite_depth ? FFX_UPSCALE_ENABLE_DEPTH_INFINITE : 0U);

    state->create_desc.maxRenderSize = {output_width, output_height};
    state->create_desc.maxUpscaleSize = {output_width, output_height};
    state->create_desc.fpMessage = &ffx_message_bridge;

    state->backend_desc.header.type =
        FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    state->backend_desc.header.pNext = &state->version_desc.header;
    state->backend_desc.device = device;

    state->version_desc.header.type =
        FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    state->version_desc.header.pNext = nullptr;
    state->version_desc.version = FFX_UPSCALER_VERSION;

    const auto create_result = state->create(
        &state->context, &state->create_desc.header, nullptr);
    if (create_result != FFX_API_RETURN_OK || state->context == nullptr) {
        availability_ = availability_for_result(create_result);
        detail_ = "ffxCreateContext: ";
        detail_ += ffx_result_name(create_result);
        return false;
    }

    std::uint32_t recommended_width{};
    std::uint32_t recommended_height{};
    ffxQueryDescUpscaleGetRenderResolutionFromQualityMode extent_query{};
    extent_query.header.type =
        FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
    extent_query.displayWidth = output_width;
    extent_query.displayHeight = output_height;
    extent_query.qualityMode = fsr_quality;
    extent_query.pOutRenderWidth = &recommended_width;
    extent_query.pOutRenderHeight = &recommended_height;
    const auto extent_result = state->query(
        &state->context, &extent_query.header);
    if (extent_result != FFX_API_RETURN_OK) {
        availability_ = Availability::required_input_unavailable;
        detail_ = "FidelityFX render-resolution query: ";
        detail_ += ffx_result_name(extent_result);
        return false;
    }

    if (recommended_width != render_width ||
        recommended_height != render_height) {
        logger::info(
            "AMD FidelityFX {} is rendering at {}x{} rather than the {}x{} it "
            "recommends for {}x{} output. The active renderer contract chooses "
            "the extent; the context is allocated for the full range.",
            quality_mode_key(quality),
            render_width,
            render_height,
            recommended_width,
            recommended_height,
            output_width,
            output_height);
    }

    ffxQueryGetProviderVersion provider_version{};
    provider_version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (state->query(&state->context, &provider_version.header) ==
            FFX_API_RETURN_OK &&
        provider_version.versionName != nullptr) {
        version_ = provider_version.versionName;
    } else {
        version_ = "FidelityFX 4.1.1 API";
    }

    if (!state->create_command_slots(detail_)) {
        availability_ = Availability::context_creation_failed;
        return false;
    }

    state->quality = quality;
    state->render_width = render_width;
    state->render_height = render_height;
    state->output_width = output_width;
    state->output_height = output_height;
    state->reversed_depth = reversed_depth;
    state->infinite_depth = infinite_depth;

    const auto configured_flags = state->create_desc.flags;
    logger::info(
        "AMD FidelityFX D3D12 upscaler configured: runtime={}, quality={}, "
        "active={}x{}, output={}x{}, depth={}, infinite-far={}, "
        "auto-exposure={}, nonlinear-sRGB={}, exposure-resource=none, "
        "preExposure=1.0, flags=0x{:04X}. Read back from the flags submitted "
        "to the runtime, not asserted.",
        version_,
        quality_mode_key(quality),
        render_width,
        render_height,
        output_width,
        output_height,
        reversed_depth ? "reversed" : "standard",
        infinite_depth,
        (configured_flags & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE) != 0U,
        (configured_flags & FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE) != 0U,
        configured_flags);

    availability_ = Availability::available;
    detail_.clear();
    state_ = std::move(state);
    return true;
}

bool FsrUpscaler::evaluate(const FsrDispatch& dispatch_description)
{

    if (!ready() || dispatch_description.color == nullptr ||
        dispatch_description.motion_vectors == nullptr ||
        dispatch_description.depth == nullptr ||
        dispatch_description.output == nullptr ||
        dispatch_description.render_width != state_->render_width ||
        dispatch_description.render_height != state_->render_height ||
        dispatch_description.output_width != state_->output_width ||
        dispatch_description.output_height != state_->output_height ||
        dispatch_description.reversed_depth != state_->reversed_depth ||
        dispatch_description.infinite_depth != state_->infinite_depth ||
        !std::isfinite(dispatch_description.camera_near) ||
        !std::isfinite(dispatch_description.camera_far) ||
        !std::isfinite(dispatch_description.camera_fov_vertical) ||
        dispatch_description.camera_near <= 0.0F ||
        dispatch_description.camera_fov_vertical <= 0.0F) {
        detail_ =
            "FidelityFX dispatch did not match the configured render contract";
        return false;
    }
    if (state_->owner_thread_id != GetCurrentThreadId()) {
        detail_ = "FidelityFX evaluation moved to a different render thread";
        return false;
    }

    auto* const color =
        static_cast<ID3D12Resource*>(dispatch_description.color);
    auto* const motion =
        static_cast<ID3D12Resource*>(dispatch_description.motion_vectors);
    auto* const depth =
        static_cast<ID3D12Resource*>(dispatch_description.depth);
    auto* const reactive =
        static_cast<ID3D12Resource*>(dispatch_description.reactive_mask);
    auto* const transparency =
        static_cast<ID3D12Resource*>(dispatch_description.transparency_mask);
    auto* const output =
        static_cast<ID3D12Resource*>(dispatch_description.output);

    const std::array current_resources{
        color, motion, depth, reactive, transparency, output};
    if (current_resources != state_->validated_resources) {
        detail_.clear();
        const auto resources_valid =
            validate_texture(
                color,
                dispatch_description.render_width,
                dispatch_description.render_height,
                DXGI_FORMAT_R8G8B8A8_UNORM,
                D3D12_RESOURCE_FLAG_NONE,
                "FidelityFX input color",
                detail_) &&
            validate_texture(
                motion,
                dispatch_description.render_width,
                dispatch_description.render_height,
                DXGI_FORMAT_R16G16_FLOAT,
                D3D12_RESOURCE_FLAG_NONE,
                "FidelityFX motion vectors",
                detail_) &&
            validate_texture(
                depth,
                dispatch_description.render_width,
                dispatch_description.render_height,
                DXGI_FORMAT_R32_FLOAT,
                D3D12_RESOURCE_FLAG_NONE,
                "FidelityFX depth",
                detail_) &&
            (reactive == nullptr ||
             validate_texture(
                 reactive,
                 dispatch_description.render_width,
                 dispatch_description.render_height,
                 DXGI_FORMAT_R8_UNORM,
                 D3D12_RESOURCE_FLAG_NONE,
                 "FidelityFX reactive mask",
                 detail_)) &&
            (transparency == nullptr ||
             validate_texture(
                 transparency,
                 dispatch_description.render_width,
                 dispatch_description.render_height,
                 DXGI_FORMAT_R8_UNORM,
                 D3D12_RESOURCE_FLAG_NONE,
                 "FidelityFX transparency mask",
                 detail_)) &&
            validate_texture(
                output,
                dispatch_description.output_width,
                dispatch_description.output_height,
                DXGI_FORMAT_R8G8B8A8_UNORM,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                "FidelityFX output",
                detail_) &&
            color != output && motion != output && depth != output &&
            reactive != output && transparency != output &&
            validate_resource_device(
                color,
                state_->device.Get(),
                "FidelityFX input color",
                detail_) &&
            validate_resource_device(
                motion,
                state_->device.Get(),
                "FidelityFX motion vectors",
                detail_) &&
            validate_resource_device(
                depth,
                state_->device.Get(),
                "FidelityFX depth",
                detail_) &&
            (reactive == nullptr ||
             validate_resource_device(
                 reactive,
                 state_->device.Get(),
                 "FidelityFX reactive mask",
                 detail_)) &&
            (transparency == nullptr ||
             validate_resource_device(
                 transparency,
                 state_->device.Get(),
                 "FidelityFX transparency mask",
                 detail_)) &&
            validate_resource_device(
                output,
                state_->device.Get(),
                "FidelityFX output",
                detail_);
        if (!resources_valid) {
            if (detail_.empty()) {
                detail_ = "FidelityFX output aliases one of its input textures";
            }
            return false;
        }
        state_->validated_resources = current_resources;
        logger::info(
            "AMD FidelityFX resource contract verified: "
            "color=R8G8B8A8_UNORM, motion=R16G16_FLOAT, depth=R32_FLOAT, "
            "reactive={}, transparency={}, output=R8G8B8A8_UNORM UAV",
            reactive != nullptr ? "R8_UNORM" : "absent",
            transparency != nullptr ? "R8_UNORM" : "absent");
    }

    auto* const slot = state_->acquire_slot();
    if (slot == nullptr) {
        detail_ = "all FidelityFX command slots are still in flight";
        return false;
    }
    auto hr = slot->allocator->Reset();
    if (SUCCEEDED(hr)) {
        hr = slot->list->Reset(slot->allocator.Get(), nullptr);
    }
    if (FAILED(hr)) {
        detail_ = "FidelityFX command-list reset failed";
        return false;
    }

    std::array<D3D12_RESOURCE_BARRIER, 6U> to_dispatch{};
    std::size_t dispatch_barriers = 0U;
    const auto add_read_barrier =
        [&to_dispatch, &dispatch_barriers](ID3D12Resource* const resource) {
            if (resource != nullptr) {
                to_dispatch[dispatch_barriers++] = transition(
                    resource,
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
        };
    add_read_barrier(color);
    add_read_barrier(motion);
    add_read_barrier(depth);
    add_read_barrier(reactive);
    add_read_barrier(transparency);
    to_dispatch[dispatch_barriers++] = transition(
        output,
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    slot->list->ResourceBarrier(
        static_cast<UINT>(dispatch_barriers), to_dispatch.data());

    ffxDispatchDescUpscale parameters{};
    parameters.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    parameters.commandList = slot->list.Get();
    parameters.color = ffxApiGetResourceDX12(
        color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    parameters.depth = ffxApiGetResourceDX12(
        depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    parameters.motionVectors = ffxApiGetResourceDX12(
        motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);

    parameters.reactive = reactive != nullptr ?
        ffxApiGetResourceDX12(
            reactive, FFX_API_RESOURCE_STATE_COMPUTE_READ) :
        decltype(parameters.reactive){};
    parameters.transparencyAndComposition = transparency != nullptr ?
        ffxApiGetResourceDX12(
            transparency, FFX_API_RESOURCE_STATE_COMPUTE_READ) :
        decltype(parameters.transparencyAndComposition){};
    parameters.output = ffxApiGetResourceDX12(
        output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);

    parameters.jitterOffset = {
        dispatch_description.jitter_x,
        dispatch_description.jitter_y};

    const auto& settings = config::Settings::instance();
    parameters.motionVectorScale = {
        static_cast<float>(dispatch_description.render_width) *
            settings.motion_scale_x(),
        static_cast<float>(dispatch_description.render_height) *
            settings.motion_scale_y()};
    parameters.renderSize = {
        dispatch_description.render_width,
        dispatch_description.render_height};
    parameters.upscaleSize = {
        dispatch_description.output_width,
        dispatch_description.output_height};
    parameters.enableSharpening = false;
    parameters.sharpness = 0.0F;
    parameters.frameTimeDelta = frame_delta_milliseconds(
        state_->previous_dispatch, state_->dispatch_time_measured);
    parameters.preExposure = 1.0F;

    parameters.reset =
        dispatch_description.reset_history || state_->reset_on_next_dispatch;
    state_->reset_on_next_dispatch = false;
    parameters.cameraNear = dispatch_description.camera_near;
    parameters.cameraFar = dispatch_description.camera_far;
    parameters.cameraFovAngleVertical =
        dispatch_description.camera_fov_vertical;
    parameters.viewSpaceToMetersFactor = 1.0F;

    parameters.flags =
        settings.fsr_color_space() == config::FsrColorSpace::non_linear_srgb ?
            FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB :
            0U;

    const auto result = state_->dispatch(
        &state_->context, &parameters.header);
    if (result != FFX_API_RETURN_OK) {
        static_cast<void>(slot->list->Close());
        detail_ = "ffxDispatch: ";
        detail_ += ffx_result_name(result);
        return false;
    }

    D3D12_RESOURCE_BARRIER output_barrier{};
    output_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    output_barrier.UAV.pResource = output;
    slot->list->ResourceBarrier(1, &output_barrier);

    std::array<D3D12_RESOURCE_BARRIER, 6U> to_common{};
    std::size_t common_barriers = 0U;
    const auto add_release_barrier =
        [&to_common, &common_barriers](ID3D12Resource* const resource) {
            if (resource != nullptr) {
                to_common[common_barriers++] = transition(
                    resource,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COMMON);
            }
        };
    add_release_barrier(color);
    add_release_barrier(motion);
    add_release_barrier(depth);
    add_release_barrier(reactive);
    add_release_barrier(transparency);
    to_common[common_barriers++] = transition(
        output,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COMMON);
    slot->list->ResourceBarrier(
        static_cast<UINT>(common_barriers), to_common.data());
    hr = slot->list->Close();
    if (FAILED(hr)) {
        detail_ = "FidelityFX command-list close failed";
        return false;
    }

    ID3D12CommandList* lists[]{slot->list.Get()};
    state_->queue->ExecuteCommandLists(1, lists);
    slot->fence_value = state_->next_fence_value++;
    hr = state_->queue->Signal(state_->fence.Get(), slot->fence_value);
    const auto synchronized =
        render::SharedResources::instance().complete_upscaler_dispatch();
    if (FAILED(hr) || !synchronized) {
        detail_ = "FidelityFX output synchronization failed";
        return false;
    }

    ++state_->dispatch_count;
    ++state_->window_dispatches;
    if (parameters.reset) {
        ++state_->window_resets;
    }
    if (parameters.jitterOffset.x == 0.0F &&
        parameters.jitterOffset.y == 0.0F) {
        ++state_->window_zero_jitter;
    }
    constexpr std::uint64_t kCensusWindow = 600U;
    if (state_->window_dispatches >= kCensusWindow) {
        logger::info(
            "AMD FidelityFX dispatch census over {} dispatches: reset=true on "
            "{}, zero jitter on {}, motionVectorScale={:.1f},{:.1f}. A reset "
            "rate at or near the window size means history is discarded every "
            "frame and no temporal reconstruction is occurring.",
            state_->window_dispatches,
            state_->window_resets,
            state_->window_zero_jitter,
            parameters.motionVectorScale.x,
            parameters.motionVectorScale.y);
        state_->window_dispatches = 0U;
        state_->window_resets = 0U;
        state_->window_zero_jitter = 0U;
    }

    if (!state_->first_dispatch_logged) {
        state_->first_dispatch_logged = true;
        logger::info(
            "First real AMD FidelityFX D3D12 evaluation completed: "
            "{}x{} -> {}x{}, undilated normalized motion, "
            "motionVectorScale={:.1f},{:.1f} (multipliers {:.2f},{:.2f}), "
            "reactive={}, transparency={}, reset={}",
            dispatch_description.render_width,
            dispatch_description.render_height,
            dispatch_description.output_width,
            dispatch_description.output_height,
            parameters.motionVectorScale.x,
            parameters.motionVectorScale.y,
            settings.motion_scale_x(),
            settings.motion_scale_y(),
            reactive != nullptr,
            transparency != nullptr,
            dispatch_description.reset_history);
    }
    detail_.clear();
    return true;
}

void FsrUpscaler::shutdown() noexcept
{
    if (state_ != nullptr &&
        state_->owner_thread_id != GetCurrentThreadId()) {
        logger::critical(
            "AMD FidelityFX shutdown reached a different thread; retaining "
            "its context and signed runtimes until process termination");
        static_cast<void>(state_.release());
        availability_ = Availability::context_creation_failed;
        version_.clear();
        detail_ = "FidelityFX teardown was requested from a different thread";
        return;
    }
    if (state_ != nullptr && !state_->wait_for_idle(5000)) {
        logger::critical(
            "AMD FidelityFX teardown timed out; retaining its context and "
            "runtimes rather than destroying in-flight GPU state");
        static_cast<void>(state_.release());
        availability_ = Availability::context_creation_failed;
        version_.clear();
        detail_ = "FidelityFX teardown timed out while GPU work was in flight";
        return;
    }
    state_.reset();
    availability_ = Availability::unknown;
    version_.clear();
    detail_.clear();
}

bool FsrUpscaler::ready() const noexcept
{
    return state_ != nullptr && state_->context != nullptr &&
           availability_ == Availability::available;
}

Availability FsrUpscaler::availability() const noexcept
{
    return availability_;
}

const std::string& FsrUpscaler::version() const noexcept
{
    return version_;
}

const std::string& FsrUpscaler::detail() const noexcept
{
    return detail_;
}

std::uint32_t FsrUpscaler::expected_render_width() const noexcept
{
    return state_ != nullptr ? state_->render_width : 0;
}

std::uint32_t FsrUpscaler::expected_render_height() const noexcept
{
    return state_ != nullptr ? state_->render_height : 0;
}
}
