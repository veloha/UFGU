

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
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

constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_R24G8_TYPELESS;
constexpr UINT kDepthBind =
    D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
constexpr UINT kWidth = 256;
constexpr UINT kHeight = 128;

constexpr float kSourceDepth = 0.75F;

constexpr std::uint32_t kExpectedRaw = 12582911;

[[nodiscard]] D3D11_TEXTURE2D_DESC depth_description() noexcept
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = kWidth;
    description.Height = kHeight;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = kDepthFormat;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = kDepthBind;
    description.CPUAccessFlags = 0;
    description.MiscFlags = 0;
    return description;
}

[[nodiscard]] bool create_depth_texture(
    ID3D11Device* const device,
    ComPtr<ID3D11Texture2D>& texture)
{
    const auto description = depth_description();
    return SUCCEEDED(
        device->CreateTexture2D(&description, nullptr, &texture));
}

struct Readback
{
    bool ok{};
    std::uint32_t min_raw{0xFFFFFFFFU};
    std::uint32_t max_raw{};
    std::uint64_t zero_pixels{};
    std::uint64_t expected_pixels{};
    std::uint64_t total_pixels{};
};

[[nodiscard]] Readback read_depth(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context,
    ID3D11Texture2D* const source)
{
    Readback result{};

    auto description = depth_description();
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
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
    for (UINT y = 0; y < kHeight; ++y) {
        const auto* const row = reinterpret_cast<const std::uint32_t*>(
            bytes + static_cast<std::size_t>(y) * mapped.RowPitch);
        for (UINT x = 0; x < kWidth; ++x) {
            const std::uint32_t raw = row[x] & 0x00FFFFFFU;
            result.min_raw = raw < result.min_raw ? raw : result.min_raw;
            result.max_raw = raw > result.max_raw ? raw : result.max_raw;
            if (raw == 0) {
                ++result.zero_pixels;
            }

            if (raw + 1 >= kExpectedRaw && raw <= kExpectedRaw + 1) {
                ++result.expected_pixels;
            }
            ++result.total_pixels;
        }
    }

    context->Unmap(staging.Get(), 0);
    result.ok = true;
    return result;
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

}

int main()
{
    std::printf(
        "=== The depth-copy contract: what actually reaches DLSS ===\n\n");

    UINT flags = D3D11_CREATE_DEVICE_DEBUG;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    auto hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_WARP,
        nullptr,
        flags,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &device,
        &level,
        &context);
    auto debug_layer = true;
    if (FAILED(hr)) {
        debug_layer = false;
        flags = 0;
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            flags,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &device,
            &level,
            &context);
    }
    if (FAILED(hr) || device == nullptr || context == nullptr) {
        std::printf("FATAL: no WARP D3D11 device (0x%08X)\n",
                    static_cast<unsigned>(hr));
        return 2;
    }
    std::printf(
        "WARP D3D11 device created, feature level 0x%X, debug layer %s\n\n",
        static_cast<unsigned>(level),
        debug_layer ? "ACTIVE" : "unavailable");

    ComPtr<ID3D11InfoQueue> info_queue;
    if (debug_layer) {
        static_cast<void>(device.As(&info_queue));
        if (info_queue != nullptr) {
            info_queue->ClearStoredMessages();
        }
    }

    ComPtr<ID3D11Texture2D> source;
    if (!create_depth_texture(device.Get(), source)) {
        std::printf("FATAL: could not create the source depth texture\n");
        return 2;
    }
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_description{};
    dsv_description.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv_description.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> dsv;
    if (FAILED(device->CreateDepthStencilView(
            source.Get(), &dsv_description, &dsv))) {
        std::printf("FATAL: could not create the source DSV\n");
        return 2;
    }
    context->ClearDepthStencilView(
        dsv.Get(), D3D11_CLEAR_DEPTH, kSourceDepth, 0);

    const auto source_state = read_depth(device.Get(), context.Get(), source.Get());
    std::printf("-- the source surface --\n");
    check(source_state.ok, "the source surface can be read back");
    check(source_state.expected_pixels == source_state.total_pixels,
          "every source pixel holds the cleared depth 0.75: the source is "
          "known-good before any copy is attempted");
    std::printf("        source raw range %u..%u over %llu pixels\n\n",
                source_state.min_raw,
                source_state.max_raw,
                static_cast<unsigned long long>(source_state.total_pixels));
    if (info_queue != nullptr) {
        static_cast<void>(drain_info_queue(info_queue.Get(), "setup"));
    }

    const D3D11_BOX whole_subresource{0, 0, 0, kWidth, kHeight, 1};

    std::printf(
        "-- case A: CopySubresourceRegion with a non-NULL pSrcBox "
        "(what shipped) --\n");
    ComPtr<ID3D11Texture2D> destination_a;
    if (!create_depth_texture(device.Get(), destination_a)) {
        std::printf("FATAL: could not create destination A\n");
        return 2;
    }
    const auto fresh = read_depth(device.Get(), context.Get(), destination_a.Get());
    check(fresh.ok && fresh.zero_pixels == fresh.total_pixels,
          "a freshly created DEFAULT depth texture reads back all zero: this is "
          "the value a dropped copy leaves behind");

    context->CopySubresourceRegion(
        destination_a.Get(), 0, 0, 0, 0, source.Get(), 0, &whole_subresource);
    const auto complaints_a =
        info_queue != nullptr ? drain_info_queue(info_queue.Get(), "case A") : 0;
    const auto after_a =
        read_depth(device.Get(), context.Get(), destination_a.Get());

    if (debug_layer) {
        check(complaints_a > 0,
              "the D3D11 runtime itself rejects the call: the box copy of a "
              "depth-stencil resource is invalid regardless of driver "
              "behaviour");
    } else {
        std::printf(
            "  SKIP  debug layer unavailable; runtime diagnosis not collected\n");
    }
    check(after_a.ok, "destination A can be read back");
    check(after_a.expected_pixels != after_a.total_pixels,
          "destination A does NOT hold the source depth: the shipping copy did "
          "not deliver the depth buffer");
    check(after_a.zero_pixels == after_a.total_pixels,
          "destination A is entirely zero, which the debug view renders as a "
          "uniform cleared/far-plane field - exactly the magenta frame");
    std::printf("        destination A raw range %u..%u, zero pixels %llu of "
                "%llu\n\n",
                after_a.min_raw,
                after_a.max_raw,
                static_cast<unsigned long long>(after_a.zero_pixels),
                static_cast<unsigned long long>(after_a.total_pixels));

    std::printf(
        "-- case B: CopySubresourceRegion with pSrcBox = NULL "
        "(the correction) --\n");
    ComPtr<ID3D11Texture2D> destination_b;
    if (!create_depth_texture(device.Get(), destination_b)) {
        std::printf("FATAL: could not create destination B\n");
        return 2;
    }
    context->CopySubresourceRegion(
        destination_b.Get(), 0, 0, 0, 0, source.Get(), 0, nullptr);
    const auto complaints_b =
        info_queue != nullptr ? drain_info_queue(info_queue.Get(), "case B") : 0;
    const auto after_b =
        read_depth(device.Get(), context.Get(), destination_b.Get());

    if (debug_layer) {
        check(complaints_b == 0,
              "the runtime accepts the whole-subresource form without "
              "complaint");
    }
    check(after_b.ok, "destination B can be read back");
    check(after_b.expected_pixels == after_b.total_pixels,
          "every pixel of destination B holds the source depth 0.75: the "
          "correction delivers the real depth buffer");
    std::printf("        destination B raw range %u..%u over %llu pixels\n\n",
                after_b.min_raw,
                after_b.max_raw,
                static_cast<unsigned long long>(after_b.total_pixels));

    std::printf("-- case C: CopyResource (the same-shape shortcut) --\n");
    ComPtr<ID3D11Texture2D> destination_c;
    if (!create_depth_texture(device.Get(), destination_c)) {
        std::printf("FATAL: could not create destination C\n");
        return 2;
    }
    context->CopyResource(destination_c.Get(), source.Get());
    const auto complaints_c =
        info_queue != nullptr ? drain_info_queue(info_queue.Get(), "case C") : 0;
    const auto after_c =
        read_depth(device.Get(), context.Get(), destination_c.Get());
    if (debug_layer) {
        check(complaints_c == 0, "the runtime accepts CopyResource");
    }
    check(after_c.expected_pixels == after_c.total_pixels,
          "CopyResource also delivers the real depth buffer");
    std::printf("\n");

    std::printf(
        "-- control: the identical box copy on Skyrim's motion resource --\n");
    D3D11_TEXTURE2D_DESC motion_description{};
    motion_description.Width = kWidth;
    motion_description.Height = kHeight;
    motion_description.MipLevels = 1;
    motion_description.ArraySize = 1;

    motion_description.Format = DXGI_FORMAT_R16G16_FLOAT;
    motion_description.SampleDesc.Count = 1;
    motion_description.Usage = D3D11_USAGE_DEFAULT;
    motion_description.BindFlags =
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> motion_source;
    ComPtr<ID3D11Texture2D> motion_destination;
    const auto motion_created =
        SUCCEEDED(device->CreateTexture2D(
            &motion_description, nullptr, &motion_source)) &&
        SUCCEEDED(device->CreateTexture2D(
            &motion_description, nullptr, &motion_destination));
    check(motion_created, "the control motion textures were created");
    if (motion_created) {
        context->CopySubresourceRegion(
            motion_destination.Get(),
            0, 0, 0, 0,
            motion_source.Get(),
            0,
            &whole_subresource);
        const auto complaints_motion =
            info_queue != nullptr ?
                drain_info_queue(info_queue.Get(), "control") : 0;
        if (debug_layer) {
            check(complaints_motion == 0,
                  "the same box copy is legal on a non-depth-stencil resource: "
                  "this is why the Motion view showed real data on the frame "
                  "the Depth view did not");
        }
    }

    std::printf(
        "\n=== DepthCopyContractTest: %d passed, %d failed ===\n",
        g_passed,
        g_failed);
    return g_failed == 0 ? 0 : 1;
}
