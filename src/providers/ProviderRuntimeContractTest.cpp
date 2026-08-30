#include "providers/RuntimeSignature.hpp"

#include <sl_security.h>

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#pragma warning(push)
#pragma warning(disable : 4191 4365 4514 4820 5219)
#include <api/include/ffx_api.h>
#include <api/include/ffx_api_types.h>
#include <api/include/dx12/ffx_api_dx12.h>
#include <upscalers/include/ffx_upscale.h>
#include <xess/xess_d3d12.h>
#pragma warning(pop)

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>

namespace
{
class Module final
{
public:
    explicit Module(const std::filesystem::path& path) : path_(path)
    {
        value_ = LoadLibraryExW(
            path.c_str(),
            nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                LOAD_LIBRARY_SEARCH_SYSTEM32);
    }

    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    ~Module()
    {
        if (value_ != nullptr) {
            static_cast<void>(FreeLibrary(value_));
        }
    }

    [[nodiscard]] bool loaded() const noexcept
    {
        return value_ != nullptr;
    }

    [[nodiscard]] bool has_export(const char* const name) const noexcept
    {
        return value_ != nullptr && GetProcAddress(value_, name) != nullptr;
    }

    template <class Function>
    [[nodiscard]] Function function(const char* const name) const noexcept
    {
#pragma warning(suppress : 4191)
        return value_ != nullptr ? reinterpret_cast<Function>(
                                      GetProcAddress(value_, name)) :
                                  nullptr;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
    HMODULE value_{};
};

int failures{};

using Microsoft::WRL::ComPtr;

struct CommandStream final
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event{};

    CommandStream() = default;
    CommandStream(const CommandStream&) = delete;
    CommandStream& operator=(const CommandStream&) = delete;

    ~CommandStream()
    {
        if (fence_event != nullptr) {
            static_cast<void>(CloseHandle(fence_event));
        }
    }

    [[nodiscard]] bool initialize(ID3D12Device* const source_device)
    {
        if (source_device == nullptr) {
            return false;
        }
        device = source_device;

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        auto result = device->CreateCommandQueue(
            &queue_desc, IID_PPV_ARGS(&queue));
        if (FAILED(result)) {
            return false;
        }
        result = device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&allocator));
        if (FAILED(result)) {
            return false;
        }
        result = device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            allocator.Get(),
            nullptr,
            IID_PPV_ARGS(&list));
        if (FAILED(result)) {
            return false;
        }
        result = device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(result)) {
            return false;
        }
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return fence_event != nullptr;
    }

    [[nodiscard]] bool submit_and_wait()
    {
        if (device == nullptr || queue == nullptr || list == nullptr ||
            fence == nullptr || fence_event == nullptr ||
            FAILED(list->Close())) {
            return false;
        }

        ID3D12CommandList* const lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
        constexpr std::uint64_t fence_value = 1;
        if (FAILED(queue->Signal(fence.Get(), fence_value))) {
            return false;
        }
        if (fence->GetCompletedValue() < fence_value) {
            if (FAILED(fence->SetEventOnCompletion(
                    fence_value, fence_event)) ||
                WaitForSingleObject(fence_event, 60000) != WAIT_OBJECT_0) {
                return false;
            }
        }
        return SUCCEEDED(device->GetDeviceRemovedReason());
    }
};

struct DispatchResources final
{
    ComPtr<ID3D12Resource> color;
    ComPtr<ID3D12Resource> motion;
    ComPtr<ID3D12Resource> depth;
    ComPtr<ID3D12Resource> reactive;
    ComPtr<ID3D12Resource> transparency;
    ComPtr<ID3D12Resource> output;
};

[[nodiscard]] D3D12_RESOURCE_BARRIER transition(
    ID3D12Resource* const resource,
    const D3D12_RESOURCE_STATES before,
    const D3D12_RESOURCE_STATES after) noexcept
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

[[nodiscard]] bool create_texture(
    ID3D12Device* const device,
    const std::uint32_t width,
    const std::uint32_t height,
    const DXGI_FORMAT format,
    const D3D12_RESOURCE_FLAGS flags,
    ComPtr<ID3D12Resource>& resource)
{
    if (device == nullptr || width == 0 || height == 0) {
        return false;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = static_cast<UINT64>(width);
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = flags;
    return SUCCEEDED(device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &description,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&resource)));
}

[[nodiscard]] bool create_dispatch_resources(
    ID3D12Device* const device,
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    DispatchResources& resources)
{
    return create_texture(
               device,
               render_width,
               render_height,
               DXGI_FORMAT_R8G8B8A8_UNORM,
               D3D12_RESOURCE_FLAG_NONE,
               resources.color) &&
           create_texture(
               device,
               render_width,
               render_height,
               DXGI_FORMAT_R16G16_FLOAT,
               D3D12_RESOURCE_FLAG_NONE,
               resources.motion) &&
           create_texture(
               device,
               render_width,
               render_height,
               DXGI_FORMAT_R32_FLOAT,
               D3D12_RESOURCE_FLAG_NONE,
               resources.depth) &&
           create_texture(
               device,
               render_width,
               render_height,
               DXGI_FORMAT_R8_UNORM,
               D3D12_RESOURCE_FLAG_NONE,
               resources.reactive) &&
           create_texture(
               device,
               render_width,
               render_height,
               DXGI_FORMAT_R8_UNORM,
               D3D12_RESOURCE_FLAG_NONE,
               resources.transparency) &&
           create_texture(
               device,
               output_width,
               output_height,
               DXGI_FORMAT_R8G8B8A8_UNORM,
               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
               resources.output);
}

void require(const bool condition, const char* const message)
{
    if (condition) {
        std::printf("PASS  %s\n", message);
        return;
    }
    std::printf("FAIL  %s\n", message);
    ++failures;
}

void verify_nonempty_file(
    const std::filesystem::path& path,
    const char* const label)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    require(!error && size != 0U, label);
    if (error || size == 0U) {
        std::printf("      %s\n", path.string().c_str());
    }
}

void verify_signed_runtime(
    const std::filesystem::path& path,
    const std::wstring_view publisher,
    const char* const label)
{
    std::string failure;
    const auto verified = mfgdlss::providers::verify_runtime_signature(
        path, publisher, failure);
    require(verified, label);
    if (!verified) {
        std::printf("      %s\n", path.string().c_str());
        std::printf("      %s\n", failure.c_str());
    }
}

void verify_exports(
    const Module& module,
    const std::span<const char* const> exports)
{
    for (const auto* const name : exports) {
        if (!module.has_export(name)) {
            std::printf(
                "FAIL  %s is missing export %s\n",
                module.path().string().c_str(),
                name);
            ++failures;
        } else {
            std::printf("PASS  export %s\n", name);
        }
    }
}

[[nodiscard]] Microsoft::WRL::ComPtr<ID3D12Device> create_d3d12_device()
{
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    const auto result = D3D12CreateDevice(
        nullptr,
        D3D_FEATURE_LEVEL_12_0,
        IID_PPV_ARGS(&device));
    require(
        SUCCEEDED(result) && device != nullptr,
        "D3D12 feature-level 12 device is available for provider contexts");
    return device;
}

void verify_fidelityfx_context(
    const Module& loader,
    ID3D12Device* const device)
{
    const auto create = loader.function<PfnFfxCreateContext>(
        "ffxCreateContext");
    const auto destroy = loader.function<PfnFfxDestroyContext>(
        "ffxDestroyContext");
    const auto query = loader.function<PfnFfxQuery>("ffxQuery");
    const auto dispatch = loader.function<PfnFfxDispatch>("ffxDispatch");
    if (create == nullptr || destroy == nullptr || query == nullptr ||
        dispatch == nullptr || device == nullptr) {
        require(false, "FidelityFX context-test prerequisites are available");
        return;
    }

    constexpr std::uint32_t output_width = 3840;
    constexpr std::uint32_t output_height = 2160;
    ffxCreateContextDescUpscale create_desc{};
    ffxCreateBackendDX12Desc backend_desc{};
    ffxCreateContextDescUpscaleVersion version_desc{};
    create_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    create_desc.header.pNext = &backend_desc.header;
    create_desc.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
                        FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    create_desc.maxRenderSize = {output_width, output_height};
    create_desc.maxUpscaleSize = {output_width, output_height};

    backend_desc.header.type =
        FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend_desc.header.pNext = &version_desc.header;
    backend_desc.device = device;

    version_desc.header.type =
        FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version_desc.version = FFX_UPSCALER_VERSION;

    ffxContext context{};
    const auto created = create(&context, &create_desc.header, nullptr);
    require(
        created == FFX_API_RETURN_OK && context != nullptr,
        "Real FidelityFX 4.1 D3D12 upscaler context is created");
    if (created != FFX_API_RETURN_OK || context == nullptr) {
        return;
    }

    std::uint32_t render_width{};
    std::uint32_t render_height{};
    ffxQueryDescUpscaleGetRenderResolutionFromQualityMode extent{};
    extent.header.type =
        FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
    extent.displayWidth = output_width;
    extent.displayHeight = output_height;
    extent.qualityMode = FFX_UPSCALE_QUALITY_MODE_QUALITY;
    extent.pOutRenderWidth = &render_width;
    extent.pOutRenderHeight = &render_height;
    const auto queried = query(&context, &extent.header);
    const auto extent_valid =
        queried == FFX_API_RETURN_OK && render_width != 0 &&
        render_height != 0 && render_width <= output_width &&
        render_height <= output_height;
    require(
        extent_valid,
        "FidelityFX runtime returns a valid 4K Quality input extent");

    if (extent_valid) {
        CommandStream commands;
        DispatchResources resources;
        const auto prepared = commands.initialize(device) &&
                              create_dispatch_resources(
                                  device,
                                  render_width,
                                  render_height,
                                  output_width,
                                  output_height,
                                  resources);
        require(
            prepared,
            "FidelityFX one-frame GPU resources and command stream are "
            "created");
        if (prepared) {
            const std::array to_dispatch{
                transition(
                    resources.color.Get(),
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                transition(
                    resources.motion.Get(),
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                transition(
                    resources.depth.Get(),
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                transition(
                    resources.reactive.Get(),
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                transition(
                    resources.transparency.Get(),
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                transition(
                    resources.output.Get(),
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
            commands.list->ResourceBarrier(
                static_cast<UINT>(to_dispatch.size()),
                to_dispatch.data());

            ffxDispatchDescUpscale parameters{};
            parameters.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
            parameters.commandList = commands.list.Get();
            parameters.color = ffxApiGetResourceDX12(
                resources.color.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            parameters.depth = ffxApiGetResourceDX12(
                resources.depth.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            parameters.motionVectors = ffxApiGetResourceDX12(
                resources.motion.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            parameters.reactive = ffxApiGetResourceDX12(
                resources.reactive.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            parameters.transparencyAndComposition = ffxApiGetResourceDX12(
                resources.transparency.Get(),
                FFX_API_RESOURCE_STATE_COMPUTE_READ);
            parameters.output = ffxApiGetResourceDX12(
                resources.output.Get(),
                FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
            parameters.jitterOffset = {0.0F, 0.0F};
            parameters.motionVectorScale = {
                static_cast<float>(render_width),
                static_cast<float>(render_height)};
            parameters.renderSize = {render_width, render_height};
            parameters.upscaleSize = {output_width, output_height};
            parameters.enableSharpening = false;
            parameters.sharpness = 0.0F;
            parameters.frameTimeDelta = 16.6667F;
            parameters.preExposure = 1.0F;
            parameters.reset = true;
            parameters.cameraNear = 0.1F;
            parameters.cameraFar = 1000.0F;
            parameters.cameraFovAngleVertical = 1.04719755F;
            parameters.viewSpaceToMetersFactor = 1.0F;
            parameters.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;

            const auto dispatched = dispatch(&context, &parameters.header);
            require(
                dispatched == FFX_API_RETURN_OK,
                "FidelityFX records one real 4K Quality evaluation");
            if (dispatched == FFX_API_RETURN_OK) {
                D3D12_RESOURCE_BARRIER output_barrier{};
                output_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                output_barrier.UAV.pResource = resources.output.Get();
                commands.list->ResourceBarrier(1, &output_barrier);
                const std::array to_common{
                    transition(
                        resources.color.Get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COMMON),
                    transition(
                        resources.motion.Get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COMMON),
                    transition(
                        resources.depth.Get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COMMON),
                    transition(
                        resources.reactive.Get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COMMON),
                    transition(
                        resources.transparency.Get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COMMON),
                    transition(
                        resources.output.Get(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COMMON)};
                commands.list->ResourceBarrier(
                    static_cast<UINT>(to_common.size()), to_common.data());
                require(
                    commands.submit_and_wait(),
                    "FidelityFX one-frame evaluation reaches a healthy GPU "
                    "fence");
            }
        }
    }

    const auto destroyed = destroy(&context, nullptr);
    require(
        destroyed == FFX_API_RETURN_OK,
        "FidelityFX context is destroyed cleanly");
    context = nullptr;
}

[[nodiscard]] bool xess_succeeded(const xess_result_t result) noexcept
{
    return result == XESS_RESULT_SUCCESS ||
           result == XESS_RESULT_WARNING_OLD_DRIVER;
}

void verify_xess_context(const Module& runtime, ID3D12Device* const device)
{
    using Create = decltype(&xessD3D12CreateContext);
    using Init = decltype(&xessD3D12Init);
    using Destroy = decltype(&xessDestroyContext);
    using Version = decltype(&xessGetVersion);
    using Optimal = decltype(&xessGetOptimalInputResolution);
    using Velocity = decltype(&xessSetVelocityScale);
    using Execute = decltype(&xessD3D12Execute);

    const auto create = runtime.function<Create>("xessD3D12CreateContext");
    const auto init = runtime.function<Init>("xessD3D12Init");
    const auto destroy = runtime.function<Destroy>("xessDestroyContext");
    const auto get_version = runtime.function<Version>("xessGetVersion");
    const auto get_optimal = runtime.function<Optimal>(
        "xessGetOptimalInputResolution");
    const auto set_velocity = runtime.function<Velocity>(
        "xessSetVelocityScale");
    const auto execute = runtime.function<Execute>("xessD3D12Execute");
    if (create == nullptr || init == nullptr || destroy == nullptr ||
        get_version == nullptr || get_optimal == nullptr ||
        set_velocity == nullptr || execute == nullptr || device == nullptr) {
        require(false, "XeSS context-test prerequisites are available");
        return;
    }

    xess_context_handle_t context{};
    auto result = create(device, &context);
    require(
        xess_succeeded(result) && context != nullptr,
        "Real Intel XeSS D3D12 context is created");
    if (!xess_succeeded(result) || context == nullptr) {
        return;
    }

    xess_version_t version{};
    result = get_version(&version);
    require(
        xess_succeeded(result) && version.major != 0,
        "XeSS runtime reports a concrete version");

    constexpr xess_2d_t output{3840, 2160};
    xess_2d_t optimal{};
    xess_2d_t minimum{};
    xess_2d_t maximum{};
    result = get_optimal(
        context,
        &output,
        XESS_QUALITY_SETTING_QUALITY,
        &optimal,
        &minimum,
        &maximum);
    const auto extent_valid =
        xess_succeeded(result) && optimal.x != 0 && optimal.y != 0 &&
        optimal.x >= minimum.x && optimal.y >= minimum.y &&
        optimal.x <= maximum.x && optimal.y <= maximum.y;
    require(
        extent_valid,
        "XeSS runtime returns a valid 4K Quality input extent");

    xess_d3d12_init_params_t parameters{};
    parameters.outputResolution = output;
    parameters.qualitySetting = XESS_QUALITY_SETTING_QUALITY;
    parameters.initFlags = XESS_INIT_FLAG_LDR_INPUT_COLOR |
                           XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK;
    result = init(context, &parameters);
    const auto initialized = xess_succeeded(result);
    require(
        initialized,
        "XeSS D3D12 Quality pipeline initializes on the active adapter");
    if (initialized && extent_valid) {
        result = set_velocity(
            context,
            static_cast<float>(optimal.x),
            static_cast<float>(optimal.y));
        const auto velocity_configured = xess_succeeded(result);
        require(
            velocity_configured,
            "XeSS accepts the production pixel-velocity scale");
        if (velocity_configured) {
            CommandStream commands;
            DispatchResources resources;
            const auto prepared = commands.initialize(device) &&
                                  create_dispatch_resources(
                                      device,
                                      optimal.x,
                                      optimal.y,
                                      output.x,
                                      output.y,
                                      resources);
            require(
                prepared,
                "XeSS one-frame GPU resources and command stream are "
                "created");
            if (prepared) {
                const std::array to_dispatch{
                    transition(
                        resources.color.Get(),
                        D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                    transition(
                        resources.motion.Get(),
                        D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                    transition(
                        resources.depth.Get(),
                        D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                    transition(
                        resources.reactive.Get(),
                        D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                    transition(
                        resources.output.Get(),
                        D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
                commands.list->ResourceBarrier(
                    static_cast<UINT>(to_dispatch.size()),
                    to_dispatch.data());

                xess_d3d12_execute_params_t execution{};
                execution.pColorTexture = resources.color.Get();
                execution.pVelocityTexture = resources.motion.Get();
                execution.pDepthTexture = resources.depth.Get();
                execution.pResponsivePixelMaskTexture =
                    resources.reactive.Get();
                execution.pOutputTexture = resources.output.Get();
                execution.jitterOffsetX = 0.0F;
                execution.jitterOffsetY = 0.0F;
                execution.exposureScale = 1.0F;
                execution.resetHistory = 1;
                execution.inputWidth = optimal.x;
                execution.inputHeight = optimal.y;
                result = execute(context, commands.list.Get(), &execution);
                const auto dispatched = xess_succeeded(result);
                require(
                    dispatched,
                    "XeSS records one real 4K Quality evaluation");
                if (dispatched) {
                    D3D12_RESOURCE_BARRIER output_barrier{};
                    output_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    output_barrier.UAV.pResource = resources.output.Get();
                    commands.list->ResourceBarrier(1, &output_barrier);
                    const std::array to_common{
                        transition(
                            resources.color.Get(),
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COMMON),
                        transition(
                            resources.motion.Get(),
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COMMON),
                        transition(
                            resources.depth.Get(),
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COMMON),
                        transition(
                            resources.reactive.Get(),
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COMMON),
                        transition(
                            resources.output.Get(),
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COMMON)};
                    commands.list->ResourceBarrier(
                        static_cast<UINT>(to_common.size()),
                        to_common.data());
                    require(
                        commands.submit_and_wait(),
                        "XeSS one-frame evaluation reaches a healthy GPU "
                        "fence");
                }
            }
        }
    }

    result = destroy(context);
    require(
        xess_succeeded(result),
        "XeSS context is destroyed cleanly");
}
}

int wmain(const int argc, const wchar_t* const* const argv)
{
    if (argc != 2) {
        std::printf(
            "ProviderRuntimeContractTest requires the staged SKSE/Plugins "
            "directory.\n");
        return 2;
    }

    const std::filesystem::path plugins(argv[1]);
    const auto package_root = plugins.parent_path().parent_path();
    const auto runtime_root = plugins / L"UFGU";
    const auto amd_root = runtime_root / L"AMD";
    const auto intel_root = runtime_root / L"Intel";
    const auto streamline_root = runtime_root / L"Streamline";

    const auto fsr_effect_path =
        amd_root / L"amd_fidelityfx_upscaler_dx12.dll";
    const auto fsr_loader_path =
        amd_root / L"amd_fidelityfx_loader_dx12.dll";
    const auto fsr_frame_generation_path =
        amd_root / L"amd_fidelityfx_framegeneration_dx12.dll";
    const auto xess_path = intel_root / L"libxess.dll";
    const auto xess_frame_generation_path = intel_root / L"libxess_fg.dll";
    const auto xell_path = intel_root / L"libxell.dll";

    const std::array required_files{
        std::pair{plugins / L"UFGU.dll", "plugin DLL is staged"},
        std::pair{plugins / L"UFGU.ini", "public default INI is staged"},
        std::pair{
            package_root / L"fomod" / L"ModuleConfig.xml",
            "FOMOD installer is staged"},
        std::pair{
            package_root / L"fomod" / L"images" / L"header.png",
            "FOMOD header is staged"},
        std::pair{
            package_root / L"Licenses" / L"PROJECT-LICENSE.txt",
            "project license is staged"},
        std::pair{
            amd_root / L"Licenses" / L"AMD-FidelityFX-license.txt",
            "AMD license is staged"},
        std::pair{
            amd_root / L"Licenses" / L"AMD-FidelityFX-third-party.txt",
            "AMD third-party notice is staged"},
        std::pair{
            intel_root / L"Licenses" / L"Intel-XeSS-license.txt",
            "Intel license is staged"},
        std::pair{
            intel_root / L"Licenses" / L"Intel-XeSS-third-party.txt",
            "Intel third-party notice is staged"},
        std::pair{
            streamline_root / L"Licenses" / L"Streamline-license.txt",
            "Streamline license is staged"},
        std::pair{
            streamline_root / L"Licenses" /
                L"Streamline-3rd-party-licenses.txt",
            "Streamline third-party notice is staged"},
        std::pair{
            streamline_root / L"Licenses" / L"NVIDIA-RTX-SDK-license.txt",
            "NVIDIA RTX SDK license is staged"},
        std::pair{
            streamline_root / L"Licenses" / L"NVIDIA-Reflex-license.txt",
            "NVIDIA Reflex license is staged"},
    };
    for (const auto& [path, label] : required_files) {
        verify_nonempty_file(path, label);
    }

    verify_signed_runtime(
        fsr_effect_path,
        L"Advanced Micro Devices",
        "FidelityFX upscaler has a valid AMD Authenticode signature");
    verify_signed_runtime(
        fsr_loader_path,
        L"Advanced Micro Devices",
        "FidelityFX loader has a valid AMD Authenticode signature");
    verify_signed_runtime(
        fsr_frame_generation_path,
        L"Advanced Micro Devices",
        "FidelityFX frame generator has a valid AMD Authenticode signature");
    verify_signed_runtime(
        xess_path,
        L"Intel Corporation",
        "XeSS runtime has a valid Intel Authenticode signature");
    verify_signed_runtime(
        xess_frame_generation_path,
        L"Intel Corporation",
        "XeSS frame-generation runtime has a valid Intel signature");
    verify_signed_runtime(
        xell_path,
        L"Intel Corporation",
        "Xe Low Latency runtime has a valid Intel signature");

    const std::array nvidia_runtime_names{
        L"nvngx_dlss.dll",
        L"nvngx_dlssg.dll",
        L"sl.common.dll",
        L"sl.dlss.dll",
        L"sl.dlss_g.dll",
        L"sl.interposer.dll",
        L"sl.pcl.dll",
        L"sl.reflex.dll",
    };
    for (const auto* const name : nvidia_runtime_names) {
        verify_signed_runtime(
            streamline_root / name,
            L"NVIDIA Corporation",
            "required Streamline runtime has a valid NVIDIA signature");
    }
    require(
        sl::security::verifyEmbeddedSignature(
            (streamline_root / L"sl.interposer.dll").c_str()),
        "Streamline interposer passes NVIDIA embedded-signature verification");

    const Module fsr_effect(fsr_effect_path);
    require(fsr_effect.loaded(), "FidelityFX upscaler and dependencies load");
    const Module fsr_frame_generation(fsr_frame_generation_path);
    require(
        fsr_frame_generation.loaded(),
        "FidelityFX frame generator and dependencies load");
    const Module fsr_loader(fsr_loader_path);
    require(fsr_loader.loaded(), "FidelityFX loader and dependencies load");

    constexpr std::array fsr_exports{
        "ffxCreateContext",
        "ffxDestroyContext",
        "ffxConfigure",
        "ffxQuery",
        "ffxDispatch",
    };
    verify_exports(fsr_effect, fsr_exports);
    verify_exports(fsr_frame_generation, fsr_exports);
    verify_exports(fsr_loader, fsr_exports);

    const Module xess(xess_path);
    require(xess.loaded(), "XeSS runtime and dependencies load");
    constexpr std::array xess_exports{
        "xessD3D12CreateContext",
        "xessD3D12Init",
        "xessD3D12Execute",
        "xessDestroyContext",
        "xessGetVersion",
        "xessGetInputResolution",
        "xessGetOptimalInputResolution",
        "xessSetVelocityScale",
    };
    verify_exports(xess, xess_exports);
    constexpr std::array xess_optional_exports{
        "xessIsOptimalDriver",
        "xessSetLoggingCallback",
        "xessGetIntelXeFXVersion",
    };
    verify_exports(xess, xess_optional_exports);

    const Module xess_frame_generation(xess_frame_generation_path);
    require(
        xess_frame_generation.loaded(),
        "XeSS frame-generation runtime and dependencies load");
    constexpr std::array xess_frame_generation_exports{
        "xefgSwapChainGetVersion",
        "xefgSwapChainGetProperties",
        "xefgSwapChainD3D12CreateContext",
        "xefgSwapChainD3D12InitFromSwapChain",
        "xefgSwapChainD3D12GetSwapChainPtr",
        "xefgSwapChainD3D12TagFrameResource",
        "xefgSwapChainD3D12BuildPipelines",
        "xefgSwapChainTagFrameConstants",
        "xefgSwapChainSetPresentId",
        "xefgSwapChainSetEnabled",
        "xefgSwapChainSetLatencyReduction",
        "xefgSwapChainSetNumInterpolatedFrames",
        "xefgSwapChainGetLastPresentStatus",
        "xefgSwapChainDestroy",
    };
    verify_exports(xess_frame_generation, xess_frame_generation_exports);
    constexpr std::array xess_frame_generation_optional_exports{
        "xefgSwapChainSetLoggingCallback",
    };
    verify_exports(
        xess_frame_generation, xess_frame_generation_optional_exports);

    const Module xell(xell_path);
    require(xell.loaded(), "Xe Low Latency runtime and dependencies load");
    constexpr std::array xell_exports{
        "xellGetVersion",
        "xellD3D12CreateContext",
        "xellDestroyContext",
        "xellSetSleepMode",
        "xellSleep",
        "xellAddMarkerData",
    };
    verify_exports(xell, xell_exports);
    constexpr std::array xell_optional_exports{
        "xellSetLoggingCallback",
        "xellGetFramesReports",
        "xellSleep",
        "xellAddMarkerData",
    };
    verify_exports(xell, xell_optional_exports);

    const Module streamline(streamline_root / L"sl.interposer.dll");
    require(streamline.loaded(), "Streamline interposer and dependencies load");
    constexpr std::array streamline_exports{
        "slInit",
        "slShutdown",
        "slGetFeatureFunction",
        "slGetFeatureRequirements",
        "slIsFeatureSupported",
        "slSetConstants",
        "slSetTagForFrame",
    };
    verify_exports(streamline, streamline_exports);

    const auto device = create_d3d12_device();
    if (device != nullptr) {
        verify_fidelityfx_context(fsr_loader, device.Get());
        verify_xess_context(xess, device.Get());
    }

    std::printf(
        "ProviderRuntimeContractTest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
