#include "render/MotionVectorCensus.hpp"

#include "render/ComputePass.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <SKSE/SKSE.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace mfgdlss::render
{
namespace
{
namespace logger = SKSE::log;
using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kTotalsBytes = 64U;
constexpr std::uint32_t kSampleStride = 8U;
constexpr std::uint64_t kPresentsBetweenSamples = 3000ULL;

constexpr std::string_view kCensusShader = R"(
Texture2D<float2> motion : register(t0);
Texture2D<float> depth : register(t1);
RWByteAddressBuffer totals : register(u0);

cbuffer CensusConstants : register(b0)
{
    uint2 extent;
    uint stride;
    uint unused;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 texel = id.xy * stride;
    if (texel.x >= extent.x || texel.y >= extent.y)
    {
        return;
    }

    totals.InterlockedAdd(8, 1);

    float2 sampled = motion.Load(int3(texel, 0));
    float magnitude = sqrt(sampled.x * sampled.x + sampled.y * sampled.y);
    uint bits = asuint(magnitude);

    if ((bits & 0x7F800000u) == 0x7F800000u)
    {
        totals.InterlockedAdd(20, 1);
    }
    else
    {
        totals.InterlockedMax(0, bits);
        if (magnitude == 0.0)
        {
            totals.InterlockedAdd(4, 1);
        }
        else if (magnitude <= 0.001)
        {
            totals.InterlockedAdd(24, 1);
        }
        else if (magnitude <= 0.01)
        {
            totals.InterlockedAdd(28, 1);
        }
        else if (magnitude <= 0.1)
        {
            totals.InterlockedAdd(32, 1);
        }
        else if (magnitude <= 1.0)
        {
            totals.InterlockedAdd(36, 1);
        }
        else if (magnitude <= 10.0)
        {
            totals.InterlockedAdd(40, 1);
        }
        else if (magnitude <= 100.0)
        {
            totals.InterlockedAdd(44, 1);
        }
        else if (magnitude <= 1000.0)
        {
            totals.InterlockedAdd(48, 1);
        }
        else
        {
            totals.InterlockedAdd(52, 1);
        }
    }

    float z = depth.Load(int3(texel, 0));
    if (z != 0.0)
    {
        totals.InterlockedAdd(12, 1);
    }
    totals.InterlockedMax(16, (uint)(saturate(z) * 65536.0 + 0.5));
}
)";

struct CensusState final
{
    std::uint64_t presents{};
    bool reported{};
    bool unavailable_logged{};
};

CensusState g_state;
}

namespace
{

[[nodiscard]] bool read_totals(
    ID3D12Device* const device,
    ID3D12CommandQueue* const queue,
    ID3D12Resource* const motion,
    ID3D12Resource* const depth,
    const std::uint32_t width,
    const std::uint32_t height,
    const DXGI_FORMAT motion_format,
    std::uint32_t* const results)
{
    ComputeBindingLayout layout{};
    layout.shader_resources = 2U;
    layout.unordered_access = 1U;
    layout.root_constant_dwords = 4U;

    ComputePass pass;
    if (!pass.create(
            "motion vector census", kCensusShader, "main", layout, device)) {
        logger::error(
            "Motion vector census could not create its pass: {}",
            pass.detail());
        return false;
    }

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC totals{};
    totals.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    totals.Width = kTotalsBytes;
    totals.Height = 1U;
    totals.DepthOrArraySize = 1U;
    totals.MipLevels = 1U;
    totals.Format = DXGI_FORMAT_UNKNOWN;
    totals.SampleDesc.Count = 1U;
    totals.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    totals.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> accumulator;
    if (FAILED(device->CreateCommittedResource(
            &default_heap,
            D3D12_HEAP_FLAG_NONE,
            &totals,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(accumulator.GetAddressOf())))) {
        return false;
    }

    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    auto readback_desc = totals;
    readback_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ComPtr<ID3D12Resource> readback;
    if (FAILED(device->CreateCommittedResource(
            &readback_heap,
            D3D12_HEAP_FLAG_NONE,
            &readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(readback.GetAddressOf())))) {
        return false;
    }

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
            0ULL,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(fence.GetAddressOf())))) {
        return false;
    }

    if (!pass.bind_shader_resource(
            0U, motion, static_cast<std::uint32_t>(motion_format)) ||
        !pass.bind_shader_resource(1U, depth, DXGI_FORMAT_R32_FLOAT) ||
        !pass.bind_unordered_access_buffer(
            0U, accumulator.Get(), kTotalsBytes)) {
        logger::error(
            "Motion vector census could not bind its inputs: {}",
            pass.detail());
        return false;
    }

    const std::array<std::uint32_t, 4> constants{
        width, height, kSampleStride, 0U};
    const auto groups = compute_dispatch_groups(
        (width + kSampleStride - 1U) / kSampleStride,
        (height + kSampleStride - 1U) / kSampleStride,
        1U,
        ComputeGroupSize{8U, 8U, 1U});
    if (!pass.dispatch(
            commands.Get(),
            groups,
            constants.data(),
            static_cast<std::uint32_t>(constants.size()))) {
        logger::error(
            "Motion vector census could not dispatch: {}", pass.detail());
        return false;
    }

    D3D12_RESOURCE_BARRIER to_copy{};
    to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_copy.Transition.pResource = accumulator.Get();
    to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commands->ResourceBarrier(1U, &to_copy);
    commands->CopyBufferRegion(
        readback.Get(), 0ULL, accumulator.Get(), 0ULL, kTotalsBytes);
    if (FAILED(commands->Close())) {
        return false;
    }

    std::array<ID3D12CommandList*, 1> lists{commands.Get()};
    queue->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());
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
    if (waited != WAIT_OBJECT_0) {
        logger::error("Motion vector census timed out waiting for the GPU");
        return false;
    }

    void* mapped = nullptr;
    const D3D12_RANGE everything{0U, kTotalsBytes};
    if (FAILED(readback->Map(0U, &everything, &mapped)) || mapped == nullptr) {
        return false;
    }
    std::memcpy(results, mapped, kTotalsBytes);
    const D3D12_RANGE nothing{0U, 0U};
    readback->Unmap(0U, &nothing);
    return true;
}
}

bool run_motion_vector_reduction(
    void* d3d12_device,
    void* d3d12_command_queue,
    void* d3d12_motion_texture,
    void* d3d12_depth_texture,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t motion_format,
    std::uint32_t (&results)[16])
{
    return read_totals(
        static_cast<ID3D12Device*>(d3d12_device),
        static_cast<ID3D12CommandQueue*>(d3d12_command_queue),
        static_cast<ID3D12Resource*>(d3d12_motion_texture),
        static_cast<ID3D12Resource*>(d3d12_depth_texture),
        width,
        height,
        static_cast<DXGI_FORMAT>(motion_format),
        results);
}
}
