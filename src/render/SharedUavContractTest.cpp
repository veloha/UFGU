

#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
int g_passed = 0;
int g_failed = 0;

void check(const bool condition, const std::string& what)
{
    if (condition) {
        ++g_passed;
        std::printf("  PASS  %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

void note(const std::string& what)
{
    std::printf("        %s\n", what.c_str());
}

constexpr UINT kWidth = 256;
constexpr UINT kHeight = 128;

constexpr float kStoreX = 0.5F;
constexpr float kStoreY = 0.25F;

constexpr DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R16G16_FLOAT;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_R32_FLOAT;

[[nodiscard]] float half_to_float(const std::uint16_t value) noexcept
{
    const std::uint32_t sign = static_cast<std::uint32_t>(value & 0x8000U)
        << 16U;
    int exponent = static_cast<int>((value >> 10U) & 0x1FU);
    std::uint32_t mantissa = value & 0x3FFU;
    std::uint32_t bits = 0;

    if (exponent == 0) {
        if (mantissa != 0U) {
            exponent = 1;
            while ((mantissa & 0x400U) == 0U) {
                mantissa <<= 1U;
                --exponent;
            }
            mantissa &= 0x3FFU;
            bits = sign |
                (static_cast<std::uint32_t>(exponent + 112) << 23U) |
                (mantissa << 13U);
        } else {
            bits = sign;
        }
    } else if (exponent == 31) {
        bits = sign | 0x7F800000U | (mantissa << 13U);
    } else {
        bits = sign |
            (static_cast<std::uint32_t>(exponent + 112) << 23U) |
            (mantissa << 13U);
    }

    float out{};
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

constexpr char kTwoChannelShader[] =
    "RWTexture2D<float2> destination : register(u0);\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "    destination[id.xy] = float2(0.5, 0.25);\n"
    "}\n";

constexpr char kOneChannelShader[] =
    "RWTexture2D<float> destination : register(u0);\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "    destination[id.xy] = 0.5;\n"
    "}\n";

[[nodiscard]] bool compile_shader(
    ID3D11Device* const device,
    const char* const source,
    ComPtr<ID3D11ComputeShader>& shader)
{
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    const auto hr = D3DCompile(
        source,
        std::strlen(source),
        nullptr,
        nullptr,
        nullptr,
        "main",
        "cs_5_0",
        0,
        0,
        &bytecode,
        &errors);
    if (FAILED(hr)) {
        if (errors != nullptr) {
            std::printf(
                "        shader compile failed: %.*s\n",
                static_cast<int>(errors->GetBufferSize()),
                static_cast<const char*>(errors->GetBufferPointer()));
        }
        return false;
    }
    return SUCCEEDED(device->CreateComputeShader(
        bytecode->GetBufferPointer(),
        bytecode->GetBufferSize(),
        nullptr,
        &shader));
}

[[nodiscard]] D3D11_TEXTURE2D_DESC base_description(
    const DXGI_FORMAT format) noexcept
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = kWidth;
    description.Height = kHeight;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc = {1, 0};
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags =
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    description.CPUAccessFlags = 0;
    description.MiscFlags = 0;
    return description;
}

struct SharedSurface
{
    ComPtr<ID3D11Texture2D> d3d11;
    ComPtr<ID3D12Resource> d3d12;
    ComPtr<IDXGIKeyedMutex> keyed_mutex;
    HANDLE handle{};
    bool created{};
    HRESULT create_result{};
};

[[nodiscard]] SharedSurface create_shared_surface(
    ID3D11Device* const d3d11_device,
    ID3D12Device* const d3d12_device,
    const DXGI_FORMAT format,
    const bool keyed_mutex)
{
    SharedSurface surface{};
    auto description = base_description(format);
    description.MiscFlags = keyed_mutex ?
        (D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
         D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) :
        D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    surface.create_result = d3d11_device->CreateTexture2D(
        &description, nullptr, &surface.d3d11);
    if (FAILED(surface.create_result)) {
        return surface;
    }

    ComPtr<IDXGIResource1> dxgi_resource;
    if (FAILED(surface.d3d11.As(&dxgi_resource))) {
        return surface;
    }
    if (FAILED(dxgi_resource->CreateSharedHandle(
            nullptr,
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            nullptr,
            &surface.handle))) {
        return surface;
    }
    if (FAILED(d3d12_device->OpenSharedHandle(
            surface.handle, IID_PPV_ARGS(&surface.d3d12)))) {
        return surface;
    }
    if (keyed_mutex) {
        if (FAILED(surface.d3d11.As(&surface.keyed_mutex))) {
            return surface;
        }
        if (FAILED(surface.keyed_mutex->AcquireSync(0, 0))) {
            return surface;
        }
    }
    surface.created = true;
    return surface;
}

struct Readback
{
    bool ok{};
    std::uint64_t total_pixels{};
    std::uint64_t stored_pixels{};
    std::uint64_t zero_pixels{};
    float first_x{};
    float first_y{};
};

[[nodiscard]] Readback read_surface(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context,
    ID3D11Texture2D* const source,
    const DXGI_FORMAT format)
{
    Readback result{};

    auto description = base_description(format);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.MiscFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &staging))) {
        return result;
    }

    context->CopyResource(staging.Get(), source);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return result;
    }

    const auto* const bytes = static_cast<const std::uint8_t*>(mapped.pData);
    bool first = true;
    for (UINT y = 0; y < kHeight; ++y) {
        const auto* const row = bytes + static_cast<std::size_t>(y) *
            mapped.RowPitch;
        for (UINT x = 0; x < kWidth; ++x) {
            float value_x{};
            float value_y{};
            if (format == kMotionFormat) {
                const auto* const texel =
                    reinterpret_cast<const std::uint16_t*>(row) + (x * 2U);
                value_x = half_to_float(texel[0]);
                value_y = half_to_float(texel[1]);
            } else {
                value_x = reinterpret_cast<const float*>(row)[x];
                value_y = kStoreY;
            }
            if (first) {
                first = false;
                result.first_x = value_x;
                result.first_y = value_y;
            }
            if (value_x == kStoreX && value_y == kStoreY) {
                ++result.stored_pixels;
            }
            if (value_x == 0.0F) {
                ++result.zero_pixels;
            }
            ++result.total_pixels;
        }
    }

    context->Unmap(staging.Get(), 0);
    result.ok = true;
    return result;
}

[[nodiscard]] bool dispatch_store(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context,
    ID3D11ComputeShader* const shader,
    ID3D11Texture2D* const destination)
{
    ComPtr<ID3D11UnorderedAccessView> view;
    if (FAILED(device->CreateUnorderedAccessView(
            destination, nullptr, &view))) {
        return false;
    }
    ID3D11UnorderedAccessView* views[] = {view.Get()};
    context->CSSetShader(shader, nullptr, 0);
    context->CSSetUnorderedAccessViews(0, 1, views, nullptr);
    context->Dispatch((kWidth + 7U) / 8U, (kHeight + 7U) / 8U, 1U);
    ID3D11UnorderedAccessView* none[] = {nullptr};
    context->CSSetUnorderedAccessViews(0, 1, none, nullptr);
    context->CSSetShader(nullptr, nullptr, 0);
    context->Flush();
    return true;
}

[[nodiscard]] std::size_t drain_info_queue(
    ID3D11InfoQueue* const queue,
    const char* const label)
{
    if (queue == nullptr) {
        return 0;
    }
    const auto count = queue->GetNumStoredMessages();
    std::size_t complaints = 0;
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T length = 0;
        if (FAILED(queue->GetMessage(i, nullptr, &length)) || length == 0) {
            continue;
        }
        std::vector<std::uint8_t> storage(length);
        auto* const message =
            reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        if (FAILED(queue->GetMessage(i, message, &length))) {
            continue;
        }
        if (message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION ||
            message->Severity == D3D11_MESSAGE_SEVERITY_ERROR ||
            message->Severity == D3D11_MESSAGE_SEVERITY_WARNING) {
            ++complaints;
            std::printf(
                "        [%s] runtime says: %.*s\n",
                label,
                static_cast<int>(message->DescriptionByteLength),
                message->pDescription);
        }
    }
    queue->ClearStoredMessages();
    return complaints;
}

[[nodiscard]] const char* describe_format(const DXGI_FORMAT format) noexcept
{
    return format == kMotionFormat ? "R16G16_FLOAT" : "R32_FLOAT";
}

void run_case(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context,
    ID3D11InfoQueue* const info_queue,
    ID3D11ComputeShader* const shader,
    ID3D11Texture2D* const destination,
    const DXGI_FORMAT format,
    const char* const label,
    const bool expect_landing)
{
    if (destination == nullptr) {
        check(false, std::string{label} + ": the destination was created");
        return;
    }
    const auto dispatched =
        dispatch_store(device, context, shader, destination);
    check(dispatched, std::string{label} + ": the UAV and dispatch were "
                                           "accepted by the runtime");
    static_cast<void>(drain_info_queue(info_queue, label));
    if (!dispatched) {
        return;
    }

    const auto after = read_surface(device, context, destination, format);
    check(after.ok, std::string{label} + ": the surface can be read back");
    if (!after.ok) {
        return;
    }

    const auto landed = after.stored_pixels == after.total_pixels;
    note("first texel reads " + std::to_string(after.first_x) + ", " +
         std::to_string(after.first_y) + "; " +
         std::to_string(after.stored_pixels) + " of " +
         std::to_string(after.total_pixels) +
         " texels hold the stored constant, " +
         std::to_string(after.zero_pixels) + " are zero");
    if (expect_landing) {
        check(landed,
              std::string{label} + ": every texel holds the stored constant");
    } else {
        note(landed ? "-> the store LANDED" : "-> the store was DISCARDED");
    }
}
}

int main()
{
    std::printf(
        "=== The shared-texture UAV contract: does a D3D11 compute store "
        "reach a texture D3D12 also holds? ===\n\n");

    UINT flags = D3D11_CREATE_DEVICE_DEBUG;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    auto driver = D3D_DRIVER_TYPE_HARDWARE;
    auto debug_layer = true;

    auto create = [&](const D3D_DRIVER_TYPE type, const UINT create_flags) {
        device.Reset();
        context.Reset();
        return D3D11CreateDevice(
            nullptr,
            type,
            nullptr,
            create_flags,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &device,
            &level,
            &context);
    };

    auto hr = create(D3D_DRIVER_TYPE_HARDWARE, flags);
    if (FAILED(hr)) {
        debug_layer = false;
        flags = 0;
        hr = create(D3D_DRIVER_TYPE_HARDWARE, flags);
    }
    if (FAILED(hr)) {
        driver = D3D_DRIVER_TYPE_WARP;
        debug_layer = true;
        flags = D3D11_CREATE_DEVICE_DEBUG;
        hr = create(D3D_DRIVER_TYPE_WARP, flags);
        if (FAILED(hr)) {
            debug_layer = false;
            flags = 0;
            hr = create(D3D_DRIVER_TYPE_WARP, flags);
        }
    }
    if (FAILED(hr) || device == nullptr || context == nullptr) {
        std::printf(
            "FATAL: no D3D11 device (0x%08X)\n", static_cast<unsigned>(hr));
        return 2;
    }
    std::printf(
        "D3D11 device: %s, feature level 0x%X, debug layer %s\n",
        driver == D3D_DRIVER_TYPE_HARDWARE ? "HARDWARE" : "WARP",
        static_cast<unsigned>(level),
        debug_layer ? "ACTIVE" : "unavailable");

    ComPtr<ID3D12Device> d3d12_device;
    hr = D3D12CreateDevice(
        nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12_device));
    if (FAILED(hr) || d3d12_device == nullptr) {
        std::printf(
            "FATAL: no D3D12 device (0x%08X); the shared cases cannot run\n",
            static_cast<unsigned>(hr));
        return 2;
    }
    std::printf("D3D12 device created for the shared-handle open\n\n");

    ComPtr<ID3D11InfoQueue> info_queue;
    if (debug_layer) {
        static_cast<void>(device.As(&info_queue));
        if (info_queue != nullptr) {
            info_queue->ClearStoredMessages();
        }
    }

    std::printf("-- factor 1: typed UAV store support, as declared by the "
                "driver --\n");
    auto typed_store_supported = [&](const DXGI_FORMAT format) {
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support{};
        support.InFormat = format;
        if (FAILED(device->CheckFeatureSupport(
                D3D11_FEATURE_FORMAT_SUPPORT2, &support, sizeof(support)))) {
            return false;
        }
        return (support.OutFormatSupport2 &
                D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0U;
    };
    const auto motion_store = typed_store_supported(kMotionFormat);
    const auto depth_store = typed_store_supported(kDepthFormat);
    note(std::string{"R16G16_FLOAT (Skyrim's motion format) typed UAV store: "} +
         (motion_store ? "SUPPORTED" : "NOT SUPPORTED"));
    note(std::string{"R32_FLOAT (the shared depth format) typed UAV store: "} +
         (depth_store ? "SUPPORTED" : "NOT SUPPORTED"));
    check(depth_store,
          "R32_FLOAT typed UAV store is supported: this is one of the formats "
          "Direct3D 11 requires, so a failure here would mean the query "
          "itself is wrong");
    std::printf("\n");

    ComPtr<ID3D11ComputeShader> two_channel;
    ComPtr<ID3D11ComputeShader> one_channel;
    if (!compile_shader(device.Get(), kTwoChannelShader, two_channel) ||
        !compile_shader(device.Get(), kOneChannelShader, one_channel)) {
        std::printf("FATAL: the store shaders did not compile\n");
        return 2;
    }

    std::printf("-- control: a PRIVATE R16G16_FLOAT texture, UAV store --\n");
    ComPtr<ID3D11Texture2D> private_motion;
    {
        const auto description = base_description(kMotionFormat);
        static_cast<void>(device->CreateTexture2D(
            &description, nullptr, &private_motion));
    }
    run_case(
        device.Get(),
        context.Get(),
        info_queue.Get(),
        two_channel.Get(),
        private_motion.Get(),
        kMotionFormat,
        "control (private, R16G16_FLOAT)",
        true);
    std::printf("\n");

    std::printf("-- control: a PRIVATE R32_FLOAT texture, UAV store --\n");
    ComPtr<ID3D11Texture2D> private_depth;
    {
        const auto description = base_description(kDepthFormat);
        static_cast<void>(device->CreateTexture2D(
            &description, nullptr, &private_depth));
    }
    run_case(
        device.Get(),
        context.Get(),
        info_queue.Get(),
        one_channel.Get(),
        private_depth.Get(),
        kDepthFormat,
        "control (private, R32_FLOAT)",
        true);
    std::printf("\n");

    std::printf(
        "-- case A: the PRODUCTION shape - shared NTHANDLE|KEYEDMUTEX, opened "
        "in D3D12, mutex held, UAV store --\n");
    for (const auto format : {kMotionFormat, kDepthFormat}) {
        auto surface = create_shared_surface(
            device.Get(), d3d12_device.Get(), format, true);
        const std::string label =
            std::string{"case A ("} + describe_format(format) + ")";
        if (!surface.created) {
            check(false,
                  label + ": the shared surface was created and opened in "
                          "D3D12 (0x" +
                      std::to_string(
                          static_cast<unsigned>(surface.create_result)) +
                      ")");
            continue;
        }
        run_case(
            device.Get(),
            context.Get(),
            info_queue.Get(),
            format == kMotionFormat ? two_channel.Get() : one_channel.Get(),
            surface.d3d11.Get(),
            format,
            label.c_str(),
            false);
    }
    std::printf("\n");

    std::printf(
        "-- case B: shared NTHANDLE only, no keyed mutex, UAV store --\n");
    for (const auto format : {kMotionFormat, kDepthFormat}) {
        auto surface = create_shared_surface(
            device.Get(), d3d12_device.Get(), format, false);
        const std::string label =
            std::string{"case B ("} + describe_format(format) + ")";
        if (!surface.created) {
            note(label + ": NOT CREATABLE - CreateTexture2D or the share "
                         "returned 0x" +
                 std::to_string(
                     static_cast<unsigned>(surface.create_result)) +
                 ". Direct3D 11 requires SHARED_NTHANDLE to be paired with "
                 "SHARED_KEYEDMUTEX, so dropping the mutex is not available "
                 "as a fix.");
            static_cast<void>(drain_info_queue(info_queue.Get(), label.c_str()));
            continue;
        }
        run_case(
            device.Get(),
            context.Get(),
            info_queue.Get(),
            format == kMotionFormat ? two_channel.Get() : one_channel.Get(),
            surface.d3d11.Get(),
            format,
            label.c_str(),
            false);
    }
    std::printf("\n");

    std::printf(
        "-- case C: the PRODUCTION shape, written by CopyResource instead --\n");
    {
        auto surface = create_shared_surface(
            device.Get(), d3d12_device.Get(), kMotionFormat, true);
        if (!surface.created) {
            check(false, "case C: the shared surface was created");
        } else if (private_motion == nullptr) {
            check(false, "case C: the control source is available");
        } else {
            const auto source_state = read_surface(
                device.Get(), context.Get(), private_motion.Get(),
                kMotionFormat);
            context->CopyResource(surface.d3d11.Get(), private_motion.Get());
            context->Flush();
            static_cast<void>(drain_info_queue(info_queue.Get(), "case C"));
            const auto after = read_surface(
                device.Get(), context.Get(), surface.d3d11.Get(),
                kMotionFormat);
            check(after.ok, "case C: the shared surface can be read back");
            note("source held " + std::to_string(source_state.stored_pixels) +
                 " stored texels; after the copy the shared surface holds " +
                 std::to_string(after.stored_pixels) + " of " +
                 std::to_string(after.total_pixels));
            check(after.stored_pixels == after.total_pixels &&
                      source_state.stored_pixels ==
                          source_state.total_pixels,
                  "CopyResource delivers into a shared keyed-mutex surface: "
                  "this is the path the scene colour takes, and it is why "
                  "colour is the one input that arrives intact");
        }
    }
    std::printf("\n");

    std::printf(
        "=== SharedUavContractTest: %d passed, %d failed ===\n",
        g_passed,
        g_failed);
    return g_failed == 0 ? 0 : 1;
}
