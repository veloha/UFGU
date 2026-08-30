#include "providers/XessUpscaler.hpp"

#include "config/Settings.hpp"
#include "providers/RuntimeLoader.hpp"
#include "render/D3D12Backend.hpp"
#include "render/SharedResources.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <SKSE/SKSE.h>

#include <xess/xess_d3d12.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <string_view>

namespace mfgdlss::providers
{
namespace
{
namespace logger = SKSE::log;
using Microsoft::WRL::ComPtr;

constexpr std::size_t kCommandSlotCount = 4;

void xess_log_bridge(
    const char* const message,
    const xess_logging_level_t level)
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
    switch (level) {
    case XESS_LOGGING_LEVEL_ERROR:
        logger::error("[XeSS] {}", text);
        return;
    case XESS_LOGGING_LEVEL_WARNING:
        logger::warn("[XeSS] {}", text);
        return;
    default:
        break;
    }
    logger::info("[XeSS] {}", text);
}

[[nodiscard]] bool xess_succeeded(const xess_result_t result) noexcept
{
    return result == XESS_RESULT_SUCCESS ||
           result == XESS_RESULT_WARNING_OLD_DRIVER;
}

[[nodiscard]] std::string_view xess_result_name(
    const xess_result_t result) noexcept
{
    switch (result) {
    case XESS_RESULT_SUCCESS: return "success";
    case XESS_RESULT_WARNING_NONEXISTING_FOLDER:
        return "warning-nonexisting-folder";
    case XESS_RESULT_WARNING_OLD_DRIVER: return "warning-old-driver";
    case XESS_RESULT_ERROR_UNSUPPORTED_DEVICE:
        return "unsupported-device";
    case XESS_RESULT_ERROR_UNSUPPORTED_DRIVER:
        return "unsupported-driver";
    case XESS_RESULT_ERROR_UNINITIALIZED: return "uninitialized";
    case XESS_RESULT_ERROR_INVALID_ARGUMENT: return "invalid-argument";
    case XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY:
        return "device-out-of-memory";
    case XESS_RESULT_ERROR_DEVICE: return "device-error";
    case XESS_RESULT_ERROR_NOT_IMPLEMENTED: return "not-implemented";
    case XESS_RESULT_ERROR_INVALID_CONTEXT: return "invalid-context";
    case XESS_RESULT_ERROR_OPERATION_IN_PROGRESS:
        return "operation-in-progress";
    case XESS_RESULT_ERROR_UNSUPPORTED: return "unsupported";
    case XESS_RESULT_ERROR_CANT_LOAD_LIBRARY:
        return "cannot-load-library";
    case XESS_RESULT_ERROR_WRONG_CALL_ORDER: return "wrong-call-order";
    case XESS_RESULT_ERROR_UNKNOWN: return "unknown-error";
    }
    return "unrecognized-result";
}

[[nodiscard]] bool to_xess_quality(
    const QualityMode quality,
    xess_quality_settings_t& result) noexcept
{
    switch (quality) {
    case QualityMode::native_antialiasing:
        result = XESS_QUALITY_SETTING_AA;
        return true;
    case QualityMode::ultra_quality_plus:
        result = XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS;
        return true;
    case QualityMode::ultra_quality:
        result = XESS_QUALITY_SETTING_ULTRA_QUALITY;
        return true;
    case QualityMode::quality:
        result = XESS_QUALITY_SETTING_QUALITY;
        return true;
    case QualityMode::balanced:
        result = XESS_QUALITY_SETTING_BALANCED;
        return true;
    case QualityMode::performance:
        result = XESS_QUALITY_SETTING_PERFORMANCE;
        return true;
    case QualityMode::ultra_performance:
        result = XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
        return true;
    case QualityMode::off:
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

[[nodiscard]] Availability availability_for_result(
    const xess_result_t result) noexcept
{
    switch (result) {
    case XESS_RESULT_ERROR_UNSUPPORTED_DEVICE:
        return Availability::adapter_unsupported;
    case XESS_RESULT_ERROR_UNSUPPORTED_DRIVER:
        return Availability::driver_too_old;
    case XESS_RESULT_ERROR_CANT_LOAD_LIBRARY:
        return Availability::runtime_missing;
    default:
        return Availability::context_creation_failed;
    }
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
                  " does not belong to the XeSS D3D12 device";
        return false;
    }
    return true;
}
}

struct XessUpscaler::State
{
    using CreateFunction = decltype(&xessD3D12CreateContext);
    using InitFunction = decltype(&xessD3D12Init);
    using ExecuteFunction = decltype(&xessD3D12Execute);
    using DestroyFunction = decltype(&xessDestroyContext);
    using VersionFunction = decltype(&xessGetVersion);
    using InputResolutionFunction = decltype(&xessGetInputResolution);
    using OptimalInputResolutionFunction =
        decltype(&xessGetOptimalInputResolution);
    using VelocityScaleFunction = decltype(&xessSetVelocityScale);
    using XeFXVersionFunction = decltype(&xessGetIntelXeFXVersion);
    using OptimalDriverFunction = decltype(&xessIsOptimalDriver);
    using LoggingCallbackFunction = decltype(&xessSetLoggingCallback);

    struct CommandSlot
    {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        std::uint64_t fence_value{};
    };

    [[nodiscard]] static RuntimeLoader& runtime_loader() noexcept
    {
        static RuntimeLoader loader;
        return loader;
    }

    CreateFunction create{};
    InitFunction init{};
    ExecuteFunction execute{};
    DestroyFunction destroy{};
    VersionFunction get_version{};
    InputResolutionFunction get_input_resolution{};
    OptimalInputResolutionFunction get_optimal_input_resolution{};
    VelocityScaleFunction set_velocity_scale{};
    OptimalDriverFunction is_optimal_driver{};
    XeFXVersionFunction get_xefx_version{};
    LoggingCallbackFunction set_logging_callback{};
    xess_context_handle_t context{};
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
    std::uint32_t init_flags{};
    bool first_dispatch_logged{};
    std::uint64_t window_dispatches{};
    std::uint64_t window_resets{};
    std::uint64_t window_zero_jitter{};
    float velocity_scale_x{};
    float velocity_scale_y{};
    DWORD owner_thread_id{};
    std::array<ID3D12Resource*, 5> validated_resources{};
    HANDLE fence_event{};

    ~State() noexcept
    {

        const auto safe_to_destroy = wait_for_idle(5000);
        if (context != nullptr && destroy != nullptr && safe_to_destroy) {
            static_cast<void>(destroy(context));
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
        auto& runtime = runtime_loader();
        if (!runtime.loaded() &&
            !runtime.load(
                L"UFGU\\Intel",
                L"libxess.dll",
                true,
                L"Intel Corporation")) {
            failure = runtime.failure();
            return false;
        }

        create = runtime.function<CreateFunction>("xessD3D12CreateContext");
        init = runtime.function<InitFunction>("xessD3D12Init");
        execute = runtime.function<ExecuteFunction>("xessD3D12Execute");
        destroy = runtime.function<DestroyFunction>("xessDestroyContext");
        get_version = runtime.function<VersionFunction>("xessGetVersion");
        get_input_resolution = runtime.function<InputResolutionFunction>(
            "xessGetInputResolution");
        get_optimal_input_resolution =
            runtime.function<OptimalInputResolutionFunction>(
                "xessGetOptimalInputResolution");
        set_velocity_scale = runtime.function<VelocityScaleFunction>(
            "xessSetVelocityScale");
        is_optimal_driver = runtime.function<OptimalDriverFunction>(
            "xessIsOptimalDriver");
        get_xefx_version = runtime.function<XeFXVersionFunction>(
            "xessGetIntelXeFXVersion");
        set_logging_callback = runtime.function<LoggingCallbackFunction>(
            "xessSetLoggingCallback");
        if (create == nullptr || init == nullptr || execute == nullptr ||
            destroy == nullptr || get_version == nullptr ||
            get_input_resolution == nullptr ||
            get_optimal_input_resolution == nullptr ||
            set_velocity_scale == nullptr) {
            failure = "required XeSS D3D12 export is missing";
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
        fence->SetName(L"Universal Upscaling XeSS dispatch fence");

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

XessUpscaler& XessUpscaler::instance() noexcept
{
    static XessUpscaler upscaler;
    return upscaler;
}

XessUpscaler::~XessUpscaler()
{
    if (state_ != nullptr) {

        static_cast<void>(state_.release());
    }
}

bool XessUpscaler::query_input_resolution_range(
    const QualityMode quality,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    std::uint32_t& optimal_width,
    std::uint32_t& optimal_height,
    std::uint32_t& minimum_width,
    std::uint32_t& minimum_height,
    std::uint32_t& maximum_width,
    std::uint32_t& maximum_height)
{
    optimal_width = 0;
    optimal_height = 0;
    minimum_width = 0;
    minimum_height = 0;
    maximum_width = 0;
    maximum_height = 0;

    xess_quality_settings_t xess_quality{};
    if (!to_xess_quality(quality, xess_quality) || output_width == 0 ||
        output_height == 0) {
        availability_ = Availability::required_input_unavailable;
        detail_ = "invalid XeSS quality or output extent";
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
    query_state.device = device;

    auto result = query_state.create(device, &query_state.context);
    if (!xess_succeeded(result) || query_state.context == nullptr) {
        availability_ = availability_for_result(result);
        detail_ = "xessD3D12CreateContext for extent query: ";
        detail_ += xess_result_name(result);
        return false;
    }

    const xess_2d_t output_resolution{output_width, output_height};
    xess_2d_t optimal{};
    xess_2d_t minimum{};
    xess_2d_t maximum{};
    result = query_state.get_optimal_input_resolution(
        query_state.context,
        &output_resolution,
        xess_quality,
        &optimal,
        &minimum,
        &maximum);
    if (!xess_succeeded(result) || optimal.x == 0 || optimal.y == 0 ||
        optimal.x > output_width || optimal.y > output_height ||
        optimal.x < minimum.x || optimal.y < minimum.y ||
        optimal.x > maximum.x || optimal.y > maximum.y) {
        availability_ = Availability::required_input_unavailable;
        detail_ = "xessGetOptimalInputResolution: ";
        detail_ += xess_result_name(result);
        return false;
    }

    optimal_width = optimal.x;
    optimal_height = optimal.y;
    minimum_width = minimum.x;
    minimum_height = minimum.y;
    maximum_width = maximum.x;
    maximum_height = maximum.y;
    availability_ = Availability::available;
    detail_.clear();
    return true;
}

bool XessUpscaler::query_render_resolution(
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

    std::uint32_t minimum_width{};
    std::uint32_t minimum_height{};
    std::uint32_t maximum_width{};
    std::uint32_t maximum_height{};
    if (!query_input_resolution_range(
            quality,
            output_width,
            output_height,
            render_width,
            render_height,
            minimum_width,
            minimum_height,
            maximum_width,
            maximum_height)) {
        render_width = 0;
        render_height = 0;
        return false;
    }

    logger::info(
        "Intel XeSS runtime selected its {} input extent: {}x{} -> {}x{} "
        "(supported range {}x{}-{}x{})",
        quality_mode_key(quality),
        render_width,
        render_height,
        output_width,
        output_height,
        minimum_width,
        minimum_height,
        maximum_width,
        maximum_height);
    return true;
}

bool XessUpscaler::supports_render_resolution(
    const QualityMode quality,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    const std::uint32_t render_width,
    const std::uint32_t render_height)
{
    if (render_width == 0 || render_height == 0 || output_width == 0 ||
        output_height == 0) {
        return false;
    }

    std::uint32_t optimal_width{};
    std::uint32_t optimal_height{};
    std::uint32_t minimum_width{};
    std::uint32_t minimum_height{};
    std::uint32_t maximum_width{};
    std::uint32_t maximum_height{};
    if (!query_input_resolution_range(
            quality,
            output_width,
            output_height,
            optimal_width,
            optimal_height,
            minimum_width,
            minimum_height,
            maximum_width,
            maximum_height)) {
        return false;
    }

    const auto aspect_error =
        static_cast<std::uint64_t>(render_width) * output_height >
                static_cast<std::uint64_t>(render_height) * output_width ?
            static_cast<std::uint64_t>(render_width) * output_height -
                static_cast<std::uint64_t>(render_height) * output_width :
            static_cast<std::uint64_t>(render_height) * output_width -
                static_cast<std::uint64_t>(render_width) * output_height;
    const auto aspect_tolerance = static_cast<std::uint64_t>(
        (std::max)(output_width, output_height));
    const auto supported =
        render_width >= minimum_width && render_height >= minimum_height &&
        render_width <= maximum_width && render_height <= maximum_height &&
        aspect_error <= aspect_tolerance;
    logger::info(
        "Intel XeSS {} would {} an explicit {}x{} input for a {}x{} output "
        "(it recommends {}x{}; supported range {}x{}-{}x{})",
        quality_mode_key(quality),
        supported ? "accept" : "refuse",
        render_width,
        render_height,
        output_width,
        output_height,
        optimal_width,
        optimal_height,
        minimum_width,
        minimum_height,
        maximum_width,
        maximum_height);
    return supported;
}

bool XessUpscaler::configure(
    const QualityMode quality,
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    const bool reversed_depth)
{
    if (state_ != nullptr &&
        state_->owner_thread_id != GetCurrentThreadId()) {
        detail_ = "XeSS configuration moved to a different render thread";
        return false;
    }
    const auto requested_init_flags = static_cast<std::uint32_t>(
        XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK |
        (reversed_depth ? XESS_INIT_FLAG_INVERTED_DEPTH : 0) |
        (config::Settings::instance().fsr_color_space() ==
                 config::FsrColorSpace::linear ?
             XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE :
             XESS_INIT_FLAG_LDR_INPUT_COLOR));
    if (state_ != nullptr && state_->context != nullptr &&
        state_->quality == quality &&
        state_->render_width == render_width &&
        state_->render_height == render_height &&
        state_->output_width == output_width &&
        state_->output_height == output_height &&
        state_->reversed_depth == reversed_depth &&
        state_->init_flags == requested_init_flags) {
        return true;
    }
    shutdown();
    if (quality == QualityMode::off || render_width == 0 ||
        render_height == 0 || output_width == 0 || output_height == 0) {
        availability_ = Availability::required_input_unavailable;
        detail_ = "invalid XeSS quality or zero-sized render contract";
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

    xess_quality_settings_t xess_quality{};
    if (!to_xess_quality(quality, xess_quality)) {
        availability_ = Availability::required_input_unavailable;
        detail_ = "the requested quality mode is not supported by XeSS";
        return false;
    }

    auto result = state->create(device, &state->context);
    if (!xess_succeeded(result) || state->context == nullptr) {
        availability_ = availability_for_result(result);
        detail_ = "xessD3D12CreateContext: ";
        detail_ += xess_result_name(result);
        return false;
    }

    if (state->set_logging_callback != nullptr) {
        static_cast<void>(state->set_logging_callback(
            state->context, XESS_LOGGING_LEVEL_WARNING, &xess_log_bridge));
    }

    if (result == XESS_RESULT_WARNING_OLD_DRIVER) {
        logger::warn(
            "XeSS created its context but reported an old driver. Intel "
            "documents this as possibly degraded performance or visual "
            "quality rather than a failure, so upscaling continues. Updating "
            "the Intel graphics driver is the fix");
    } else if (state->is_optimal_driver != nullptr) {
        const auto optimal = state->is_optimal_driver(state->context);
        if (optimal == XESS_RESULT_WARNING_OLD_DRIVER) {
            logger::warn(
                "Intel reports that the installed driver does not support the "
                "best XeSS experience, which its own documentation describes "
                "as possibly degraded performance or visual quality. "
                "Upscaling still runs. Updating the Intel graphics driver is "
                "the fix");
        }
    }

    xess_version_t runtime_version{};
    result = state->get_version(&runtime_version);
    if (xess_succeeded(result)) {
        char text[32]{};
        static_cast<void>(std::snprintf(
            text,
            sizeof(text),
            "%u.%u.%u",
            runtime_version.major,
            runtime_version.minor,
            runtime_version.patch));
        version_ = text;
    } else {
        version_ = "unknown";
    }

    if (state->get_xefx_version != nullptr) {
        xess_version_t xefx_version{};
        if (xess_succeeded(
                state->get_xefx_version(state->context, &xefx_version))) {

            if (xefx_version.major == 0U && xefx_version.minor == 0U &&
                xefx_version.patch == 0U) {

                logger::info(
                    "Intel XeFX library reports 0.0.0, which Intel documents "
                    "as the answer on a non-Intel platform. XeSS is running "
                    "on its cross-vendor path rather than the Intel one, "
                    "which is expected on a GeForce or Radeon card and would "
                    "be a fault on an Arc");
            } else {
                logger::info(
                    "Intel XeFX library {}.{}.{} is loaded, so XeSS is "
                    "running on Intel hardware rather than its cross-vendor "
                    "path. Quote this alongside the XeSS runtime version when "
                    "reporting an Intel-specific problem, because the two "
                    "version numbers move independently",
                    xefx_version.major,
                    xefx_version.minor,
                    xefx_version.patch);
            }
        }
    }


    const xess_2d_t output_resolution{output_width, output_height};
    xess_2d_t optimal{};
    xess_2d_t minimum{};
    xess_2d_t maximum{};
    result = state->get_optimal_input_resolution(
        state->context,
        &output_resolution,
        xess_quality,
        &optimal,
        &minimum,
        &maximum);

    const auto aspect_error =
        static_cast<std::uint64_t>(render_width) * output_height >
                static_cast<std::uint64_t>(render_height) * output_width ?
            static_cast<std::uint64_t>(render_width) * output_height -
                static_cast<std::uint64_t>(render_height) * output_width :
            static_cast<std::uint64_t>(render_height) * output_width -
                static_cast<std::uint64_t>(render_width) * output_height;
    const auto aspect_tolerance = static_cast<std::uint64_t>(
        (std::max)(output_width, output_height));
    if (!xess_succeeded(result) || render_width < minimum.x ||
        render_height < minimum.y || render_width > maximum.x ||
        render_height > maximum.y ||
        aspect_error > aspect_tolerance) {
        availability_ = Availability::required_input_unavailable;
        detail_ = "the current render extent is outside XeSS's supported "
                  "input range for this output and quality mode";
        return false;
    }

    xess_d3d12_init_params_t parameters{};
    parameters.outputResolution = output_resolution;
    parameters.qualitySetting = xess_quality;
    parameters.initFlags = requested_init_flags;
    result = state->init(state->context, &parameters);
    if (!xess_succeeded(result)) {
        availability_ = availability_for_result(result);
        detail_ = "xessD3D12Init: ";
        detail_ += xess_result_name(result);
        return false;
    }

    result = state->set_velocity_scale(
        state->context,
        static_cast<float>(render_width),
        static_cast<float>(render_height));
    state->velocity_scale_x = static_cast<float>(render_width);
    state->velocity_scale_y = static_cast<float>(render_height);
    if (!xess_succeeded(result)) {
        availability_ = Availability::context_creation_failed;
        detail_ = "xessSetVelocityScale: ";
        detail_ += xess_result_name(result);
        return false;
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
    state->init_flags = requested_init_flags;
    logger::info(
        "Intel XeSS D3D12 configured: runtime={}, quality={}, "
        "active={}x{}, optimal={}x{}, range={}x{}-{}x{}, output={}x{}, "
        "depth={}, colour={}, auto-exposure={}",
        version_,
        quality_mode_key(quality),
        render_width,
        render_height,
        optimal.x,
        optimal.y,
        minimum.x,
        minimum.y,
        maximum.x,
        maximum.y,
        output_width,
        output_height,
        reversed_depth ? "reversed" : "standard",
        (requested_init_flags & XESS_INIT_FLAG_LDR_INPUT_COLOR) != 0U ?
            "display-ready, XeSS tonemapping disabled" :
            "linear scene, XeSS applies its own tonemapping",
        (requested_init_flags & XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE) != 0U ?
            "on, because linear scene colour carries no exposure and Intel "
            "documents correct exposure as essential to minimise ghosting" :
            "off, because display-ready colour has already been exposed");

    availability_ = Availability::available;
    detail_.clear();
    state_ = std::move(state);
    return true;
}

bool XessUpscaler::evaluate(const XessDispatch& dispatch)
{
    if (!ready() || dispatch.color == nullptr ||
        dispatch.motion_vectors == nullptr || dispatch.depth == nullptr ||
        dispatch.responsive_mask == nullptr || dispatch.output == nullptr ||
        dispatch.render_width != state_->render_width ||
        dispatch.render_height != state_->render_height ||
        dispatch.output_width != state_->output_width ||
        dispatch.output_height != state_->output_height ||
        dispatch.reversed_depth != state_->reversed_depth) {
        detail_ = "XeSS dispatch did not match the configured render contract";
        return false;
    }
    if (state_->owner_thread_id != GetCurrentThreadId()) {
        detail_ = "XeSS evaluation moved to a different render thread";
        return false;
    }

    auto* const color = static_cast<ID3D12Resource*>(dispatch.color);
    auto* const motion =
        static_cast<ID3D12Resource*>(dispatch.motion_vectors);
    auto* const depth = static_cast<ID3D12Resource*>(dispatch.depth);
    auto* const responsive =
        static_cast<ID3D12Resource*>(dispatch.responsive_mask);
    auto* const output = static_cast<ID3D12Resource*>(dispatch.output);

    const std::array current_resources{
        color, motion, depth, responsive, output};
    if (current_resources != state_->validated_resources) {
        detail_.clear();
        const auto resources_valid =
            validate_texture(
                color,
                dispatch.render_width,
                dispatch.render_height,
                DXGI_FORMAT_R8G8B8A8_UNORM,
                D3D12_RESOURCE_FLAG_NONE,
                "XeSS input color",
                detail_) &&
            validate_texture(
                motion,
                dispatch.render_width,
                dispatch.render_height,
                DXGI_FORMAT_R16G16_FLOAT,
                D3D12_RESOURCE_FLAG_NONE,
                "XeSS motion vectors",
                detail_) &&
            validate_texture(
                depth,
                dispatch.render_width,
                dispatch.render_height,
                DXGI_FORMAT_R32_FLOAT,
                D3D12_RESOURCE_FLAG_NONE,
                "XeSS depth",
                detail_) &&
            validate_texture(
                responsive,
                dispatch.render_width,
                dispatch.render_height,
                DXGI_FORMAT_R8_UNORM,
                D3D12_RESOURCE_FLAG_NONE,
                "XeSS responsive mask",
                detail_) &&
            validate_texture(
                output,
                dispatch.output_width,
                dispatch.output_height,
                DXGI_FORMAT_R8G8B8A8_UNORM,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                "XeSS output",
                detail_) &&
            color != output && motion != output && depth != output &&
            responsive != output &&
            validate_resource_device(
                color, state_->device.Get(), "XeSS input color", detail_) &&
            validate_resource_device(
                motion,
                state_->device.Get(),
                "XeSS motion vectors",
                detail_) &&
            validate_resource_device(
                depth, state_->device.Get(), "XeSS depth", detail_) &&
            validate_resource_device(
                responsive,
                state_->device.Get(),
                "XeSS responsive mask",
                detail_) &&
            validate_resource_device(
                output, state_->device.Get(), "XeSS output", detail_);
        if (!resources_valid) {
            if (detail_.empty()) {
                detail_ = "XeSS output aliases one of its input textures";
            }
            return false;
        }
        state_->validated_resources = current_resources;
        logger::info(
            "Intel XeSS resource contract verified: color=R8G8B8A8_UNORM, "
            "motion=R16G16_FLOAT, depth=R32_FLOAT, "
            "responsive=R8_UNORM, output=R8G8B8A8_UNORM UAV");
    }
    auto* const slot = state_->acquire_slot();
    if (slot == nullptr) {
        detail_ = "all XeSS command slots are still in flight";
        return false;
    }

    auto hr = slot->allocator->Reset();
    if (SUCCEEDED(hr)) {
        hr = slot->list->Reset(slot->allocator.Get(), nullptr);
    }
    if (FAILED(hr)) {
        detail_ = "XeSS command-list reset failed";
        return false;
    }

    const std::array to_dispatch{
        transition(
            color,
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(
            motion,
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(
            depth,
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(
            responsive,
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(
            output,
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    slot->list->ResourceBarrier(
        static_cast<UINT>(to_dispatch.size()), to_dispatch.data());

    xess_d3d12_execute_params_t parameters{};
    parameters.pColorTexture = color;
    parameters.pVelocityTexture = motion;
    parameters.pDepthTexture = depth;
    parameters.pResponsivePixelMaskTexture = responsive;
    parameters.pOutputTexture = output;

    parameters.jitterOffsetX = dispatch.jitter_x;
    parameters.jitterOffsetY = dispatch.jitter_y;
    parameters.exposureScale = 1.0F;
    parameters.resetHistory = dispatch.reset_history ? 1U : 0U;
    parameters.inputWidth = dispatch.render_width;
    parameters.inputHeight = dispatch.render_height;
    ++state_->window_dispatches;
    if (parameters.resetHistory != 0U) {
        ++state_->window_resets;
    }
    if (parameters.jitterOffsetX == 0.0F && parameters.jitterOffsetY == 0.0F) {
        ++state_->window_zero_jitter;
    }
    constexpr std::uint64_t kCensusWindow = 600U;
    if (state_->window_dispatches >= kCensusWindow) {
        logger::info(
            "Intel XeSS dispatch census over {} dispatches: reset=true on {}, "
            "zero jitter on {}, velocity scale {:.1f},{:.1f}. A reset rate at "
            "or near the window size means history is discarded every frame "
            "and no temporal reconstruction is occurring. A zero jitter count "
            "at or near the window size means the sequence is not moving, "
            "which Intel documents a range of [-0.5, 0.5] for and which both "
            "vendors need in order to resolve subpixel detail. The velocity "
            "scale should equal the render extent, because Skyrim's motion "
            "vectors are in UV space rather than pixels",
            state_->window_dispatches,
            state_->window_resets,
            state_->window_zero_jitter,
            state_->velocity_scale_x,
            state_->velocity_scale_y);
        state_->window_dispatches = 0U;
        state_->window_resets = 0U;
        state_->window_zero_jitter = 0U;
    }

    const auto result =
        state_->execute(state_->context, slot->list.Get(), &parameters);
    if (!xess_succeeded(result)) {
        static_cast<void>(slot->list->Close());
        detail_ = "xessD3D12Execute: ";
        detail_ += xess_result_name(result);
        return false;
    }

    D3D12_RESOURCE_BARRIER output_barrier{};
    output_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    output_barrier.UAV.pResource = output;
    slot->list->ResourceBarrier(1, &output_barrier);
    const std::array to_common{
        transition(
            color,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COMMON),
        transition(
            motion,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COMMON),
        transition(
            depth,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COMMON),
        transition(
            responsive,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COMMON),
        transition(
            output,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COMMON)};
    slot->list->ResourceBarrier(
        static_cast<UINT>(to_common.size()), to_common.data());
    hr = slot->list->Close();
    if (FAILED(hr)) {
        detail_ = "XeSS command-list close failed";
        return false;
    }

    ID3D12CommandList* lists[]{slot->list.Get()};
    state_->queue->ExecuteCommandLists(1, lists);
    slot->fence_value = state_->next_fence_value++;
    hr = state_->queue->Signal(state_->fence.Get(), slot->fence_value);
    const auto synchronized =
        render::SharedResources::instance().complete_upscaler_dispatch();
    if (FAILED(hr) || !synchronized) {
        detail_ = "XeSS output synchronization failed";
        return false;
    }

    if (!state_->first_dispatch_logged) {
        state_->first_dispatch_logged = true;
        logger::info(
            "First real Intel XeSS D3D12 evaluation completed: {}x{} -> "
            "{}x{}, undilated normalized motion scaled to pixels, "
            "responsive-mask=true, reset={}",
            dispatch.render_width,
            dispatch.render_height,
            dispatch.output_width,
            dispatch.output_height,
            dispatch.reset_history);
    }
    detail_.clear();
    return true;
}

void XessUpscaler::shutdown() noexcept
{
    if (state_ != nullptr &&
        state_->owner_thread_id != GetCurrentThreadId()) {
        logger::critical(
            "Intel XeSS shutdown reached a different thread; retaining its "
            "context and signed runtime until process termination rather "
            "than violating XeSS's thread-affinity contract");
        static_cast<void>(state_.release());
        availability_ = Availability::context_creation_failed;
        version_.clear();
        detail_ = "XeSS teardown was requested from a different thread";
        return;
    }
    if (state_ != nullptr && !state_->wait_for_idle(5000)) {

        logger::critical(
            "Intel XeSS teardown timed out; retaining its context and runtime "
            "until process termination rather than destroying in-flight GPU "
            "state");
        static_cast<void>(state_.release());
        availability_ = Availability::context_creation_failed;
        version_.clear();
        detail_ = "XeSS teardown timed out while GPU work was in flight";
        return;
    }
    state_.reset();
    availability_ = Availability::unknown;
    version_.clear();
    detail_.clear();
}

bool XessUpscaler::ready() const noexcept
{
    return state_ != nullptr && state_->context != nullptr &&
           availability_ == Availability::available;
}

Availability XessUpscaler::availability() const noexcept
{
    return availability_;
}

const std::string& XessUpscaler::version() const noexcept
{
    return version_;
}

const std::string& XessUpscaler::detail() const noexcept
{
    return detail_;
}

std::uint32_t XessUpscaler::expected_render_width() const noexcept
{
    return state_ != nullptr ? state_->render_width : 0;
}

std::uint32_t XessUpscaler::expected_render_height() const noexcept
{
    return state_ != nullptr ? state_->render_height : 0;
}
}
