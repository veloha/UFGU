#include "render/MotionVectorCensus.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kExtent = 256U;
constexpr float kMotionMagnitude = 0.25F;
constexpr float kLargeMagnitude = 3000.0F;
constexpr float kDepthValue = 0.5F;

int g_failures = 0;

void report(const bool passed, const char* const what)
{
    if (!passed) {
        ++g_failures;
    }
    std::printf("  %s  %s\n", passed ? "PASS" : "FAIL", what);
}

[[nodiscard]] bool create_device(
    ComPtr<ID3D12Device>& device,
    ComPtr<ID3D12CommandQueue>& queue)
{
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) {
        return false;
    }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0;
         factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf()) !=
             DXGI_ERROR_NOT_FOUND;
         ++index) {
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description))) {
            continue;
        }
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0U) {
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(
                adapter.Get(),
                D3D_FEATURE_LEVEL_11_0,
                IID_PPV_ARGS(device.ReleaseAndGetAddressOf())))) {
            break;
        }
    }
    if (device == nullptr) {
        ComPtr<IDXGIAdapter> warp;
        if (FAILED(factory->EnumWarpAdapter(
                IID_PPV_ARGS(warp.GetAddressOf()))) ||
            FAILED(D3D12CreateDevice(
                warp.Get(),
                D3D_FEATURE_LEVEL_11_0,
                IID_PPV_ARGS(device.ReleaseAndGetAddressOf())))) {
            return false;
        }
    }
    D3D12_COMMAND_QUEUE_DESC description{};
    description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    return SUCCEEDED(device->CreateCommandQueue(
        &description, IID_PPV_ARGS(queue.GetAddressOf())));
}

[[nodiscard]] bool upload_texture(
    ID3D12Device* const device,
    ID3D12CommandQueue* const queue,
    const DXGI_FORMAT format,
    const std::vector<float>& channels_per_texel,
    ComPtr<ID3D12Resource>& texture)
{
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = kExtent;
    description.Height = kExtent;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = format;
    description.SampleDesc.Count = 1U;

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(device->CreateCommittedResource(
            &default_heap,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(texture.GetAddressOf())))) {
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0ULL;
    device->GetCopyableFootprints(
        &description, 0U, 1U, 0ULL, &footprint, nullptr, nullptr, &bytes);

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = bytes;
    buffer.Height = 1U;
    buffer.DepthOrArraySize = 1U;
    buffer.MipLevels = 1U;
    buffer.Format = DXGI_FORMAT_UNKNOWN;
    buffer.SampleDesc.Count = 1U;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> upload;
    if (FAILED(device->CreateCommittedResource(
            &upload_heap,
            D3D12_HEAP_FLAG_NONE,
            &buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(upload.GetAddressOf())))) {
        return false;
    }

    void* mapped = nullptr;
    const D3D12_RANGE nothing{0U, 0U};
    if (FAILED(upload->Map(0U, &nothing, &mapped)) || mapped == nullptr) {
        return false;
    }
    const auto channels = channels_per_texel.size();
    auto* const rows = static_cast<std::uint8_t*>(mapped);
    for (std::uint32_t y = 0U; y < kExtent; ++y) {
        auto* const row = reinterpret_cast<float*>(
            rows + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch);
        for (std::uint32_t x = 0U; x < kExtent; ++x) {
            for (std::size_t channel = 0U; channel < channels; ++channel) {
                row[x * channels + channel] = channels_per_texel[channel];
            }
        }
    }
    upload->Unmap(0U, nullptr);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(allocator.GetAddressOf()))) ||
        FAILED(device->CreateCommandList(
            0U,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            allocator.Get(),
            nullptr,
            IID_PPV_ARGS(commands.GetAddressOf()))) ||
        FAILED(device->CreateFence(
            0ULL, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())))) {
        return false;
    }

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0U;
    commands->CopyTextureRegion(&destination, 0U, 0U, 0U, &source, nullptr);
    if (FAILED(commands->Close())) {
        return false;
    }

    ID3D12CommandList* lists[]{commands.Get()};
    queue->ExecuteCommandLists(1U, lists);
    if (FAILED(queue->Signal(fence.Get(), 1ULL))) {
        return false;
    }
    auto* const completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (completed == nullptr) {
        return false;
    }
    if (FAILED(fence->SetEventOnCompletion(1ULL, completed))) {
        CloseHandle(completed);
        return false;
    }
    const auto waited = WaitForSingleObject(completed, 5000U);
    CloseHandle(completed);
    return waited == WAIT_OBJECT_0;
}
}

int main()
{
    std::printf("=== Motion vector census contract ===\n\n");

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    if (!create_device(device, queue)) {
        std::printf("  SKIP  no Direct3D 12 device could be created here\n");
        std::printf("=== MotionVectorCensusContractTest: skipped ===\n");
        return 0;
    }

    ComPtr<ID3D12Resource> depth;
    const std::vector<float> depth_texel{kDepthValue};
    if (!upload_texture(
            device.Get(),
            queue.Get(),
            DXGI_FORMAT_R32_FLOAT,
            depth_texel,
            depth)) {
        std::printf("  FAIL  could not build the synthetic depth input\n");
        std::printf(
            "=== MotionVectorCensusContractTest: 0 passed, 1 failed ===\n");
        return 1;
    }

    const auto census = [&](const float channel_x,
                            std::uint32_t (&results)[16]) {
        ComPtr<ID3D12Resource> motion;
        const std::vector<float> motion_texel{channel_x, 0.0F};
        if (!upload_texture(
                device.Get(),
                queue.Get(),
                DXGI_FORMAT_R32G32_FLOAT,
                motion_texel,
                motion)) {
            return false;
        }
        return mfgdlss::render::run_motion_vector_reduction(
            device.Get(),
            queue.Get(),
            motion.Get(),
            depth.Get(),
            kExtent,
            kExtent,
            static_cast<std::uint32_t>(DXGI_FORMAT_R32G32_FLOAT),
            results);
    };

    const auto expected_samples = (kExtent / 8U) * (kExtent / 8U);

    std::uint32_t modest[16]{};
    std::uint32_t large[16]{};
    std::uint32_t poisoned[16]{};
    if (!census(kMotionMagnitude, modest) || !census(kLargeMagnitude, large) ||
        !census(std::numeric_limits<float>::infinity(), poisoned)) {
        std::printf("  FAIL  a census reduction did not complete\n");
        std::printf(
            "=== MotionVectorCensusContractTest: 0 passed, 1 failed ===\n");
        return 1;
    }

    const auto peak_of = [](const std::uint32_t (&results)[16]) {
        float value = 0.0F;
        const auto bits = results[0];
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };

    report(
        modest[2] == expected_samples,
        "every eighth texel is sampled, and no more, so the stride the census "
        "reports is the stride it actually used");

    report(
        peak_of(modest) == kMotionMagnitude,
        "a uniform field reports its magnitude EXACTLY, bit for bit, because "
        "the peak travels as a float bit pattern through InterlockedMax "
        "rather than through a fixed point scale that can round");

    report(
        peak_of(large) == kLargeMagnitude,
        "a magnitude of 3000 reports as 3000 rather than as a clamp. The "
        "previous census multiplied by 64 after min(magnitude, 60000) and "
        "reported the ceiling as though it were a measurement, which is the "
        "defect this case exists to catch");

    report(
        modest[9] == expected_samples && large[13] == expected_samples,
        "each field lands wholly in the one histogram bucket that contains "
        "it, so the distribution can separate UV from pixel encoding without "
        "a mean that saturates at 1.0");

    report(
        modest[1] == 0U && large[1] == 0U,
        "no vector is counted as exactly zero when every vector moves");

    report(
        poisoned[5] == expected_samples && poisoned[0] == 0U,
        "an infinite field is counted as non-finite and is kept out of the "
        "peak, so one poisoned texel is reported rather than silently "
        "becoming the maximum");

    report(
        modest[3] == expected_samples,
        "the depth cross check sees every sample as non-zero, which is what "
        "distinguishes a real read from an unbound resource reading zero");

    std::printf(
        "\n=== MotionVectorCensusContractTest: %d passed, %d failed ===\n",
        7 - g_failures,
        g_failures);
    return g_failures == 0 ? 0 : 1;
}
