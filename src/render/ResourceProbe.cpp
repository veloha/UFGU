#include "render/ResourceProbe.hpp"

#include "render/DebugViewCore.hpp"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace mfgdlss::render
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kSlotMinimum = 0;
constexpr std::uint32_t kSlotMaximum = 1;
constexpr std::uint32_t kSlotNonfinite = 2;
constexpr std::uint32_t kSlotFar = 3;
constexpr std::uint32_t kSlotNearZero = 4;
constexpr std::uint32_t kSlotBoundsLeft = 5;
constexpr std::uint32_t kSlotBoundsTop = 6;
constexpr std::uint32_t kSlotBoundsRight = 7;
constexpr std::uint32_t kSlotBoundsBottom = 8;
constexpr std::uint32_t kSlotSampled = 9;
constexpr std::uint32_t kSlotQuadrantRightDown = 10;
constexpr std::uint32_t kSlotQuadrantRightUp = 11;
constexpr std::uint32_t kSlotQuadrantLeftDown = 12;
constexpr std::uint32_t kSlotQuadrantLeftUp = 13;
constexpr std::uint32_t kSlotOutOfRange = 14;
constexpr std::uint32_t kSlotHistogram = 16;
constexpr std::uint32_t kSlotCount = kSlotHistogram + kProbeHistogramBuckets;

constexpr std::uint32_t kThreadGroupSize = 8;

constexpr std::uint32_t kMinimumSeed = 0x7F7FFFFFU;
constexpr std::uint32_t kBoundsMinimumSeed = 0xFFFFFFFFU;

constexpr char kProbeShader[] = R"(
Texture2D<float4> source : register(t0);
RWByteAddressBuffer stats : register(u0);

cbuffer ProbeConstants : register(b0)
{
    uint2 extent;
    uint  stride;
    uint  mode;
    float far_epsilon;
    float near_zero;
    float magnitude_scale;
    uint  reversed_depth;
};

static const uint kMinimum       = 0;
static const uint kMaximum       = 1;
static const uint kNonfinite     = 2;
static const uint kFar           = 3;
static const uint kNearZero      = 4;
static const uint kBoundsLeft    = 5;
static const uint kBoundsTop     = 6;
static const uint kBoundsRight   = 7;
static const uint kBoundsBottom  = 8;
static const uint kSampled       = 9;
static const uint kQuadRightDown = 10;
static const uint kQuadRightUp   = 11;
static const uint kQuadLeftDown  = 12;
static const uint kQuadLeftUp    = 13;
static const uint kOutOfRange    = 14;
static const uint kHistogram     = 16;

void bump(uint slot)
{
    uint ignored;
    stats.InterlockedAdd(slot * 4, 1, ignored);
}

bool is_finite(float value)
{
    return (asuint(value) & 0x7F800000u) != 0x7F800000u;
}

void note_bounds(uint2 pixel)
{
    uint ignored;
    stats.InterlockedMin(kBoundsLeft * 4, pixel.x, ignored);
    stats.InterlockedMin(kBoundsTop * 4, pixel.y, ignored);
    stats.InterlockedMax(kBoundsRight * 4, pixel.x, ignored);
    stats.InterlockedMax(kBoundsBottom * 4, pixel.y, ignored);
}

[numthreads(8, 8, 1)]
void main(uint3 thread_id : SV_DispatchThreadID)
{
    const uint2 pixel = thread_id.xy * stride;
    if (pixel.x >= extent.x || pixel.y >= extent.y) {
        return;
    }

    const float4 texel = source.Load(int3(int2(pixel), 0));
    bump(kSampled);
    uint ignored;

    if (mode == 0) {
        const float raw = texel.r;
        if (!is_finite(raw)) {
            bump(kNonfinite);
            return;
        }
        if (raw < 0.0 || raw > 1.0) {
            bump(kOutOfRange);
        }
        const float clamped = saturate(raw);
        stats.InterlockedMin(kMinimum * 4, asuint(clamped), ignored);
        stats.InterlockedMax(kMaximum * 4, asuint(clamped), ignored);

        const float nearness =
            (reversed_depth != 0) ? clamped : (1.0 - clamped);
        if (nearness <= far_epsilon) {
            bump(kFar);
        } else {
            note_bounds(pixel);
        }

        const uint bucket =
            min(15u, (uint)(pow(saturate(nearness), 0.25) * 16.0));
        stats.InterlockedAdd((kHistogram + bucket) * 4, 1, ignored);
        return;
    }

    const float2 motion = texel.rg;
    if (!is_finite(motion.x) || !is_finite(motion.y)) {
        bump(kNonfinite);
        return;
    }

    const float magnitude = length(motion);
    if (!is_finite(magnitude)) {
        bump(kNonfinite);
        return;
    }
    stats.InterlockedMin(kMinimum * 4, asuint(magnitude), ignored);
    stats.InterlockedMax(kMaximum * 4, asuint(magnitude), ignored);
    if (magnitude <= near_zero) {
        bump(kNearZero);
    } else {
        note_bounds(pixel);
        if (motion.x >= 0.0) {
            bump((motion.y >= 0.0) ? kQuadRightDown : kQuadRightUp);
        } else {
            bump((motion.y >= 0.0) ? kQuadLeftDown : kQuadLeftUp);
        }
    }
    const float scale = max(magnitude_scale, 1.0e-8);
    const uint bucket =
        min(15u, (uint)(saturate(magnitude / scale) * 16.0));
    stats.InterlockedAdd((kHistogram + bucket) * 4, 1, ignored);
}
)";

struct ProbeConstants
{
    std::uint32_t extent_x{};
    std::uint32_t extent_y{};
    std::uint32_t stride{};
    std::uint32_t mode{};
    float far_epsilon{};
    float near_zero{};
    float magnitude_scale{};
    std::uint32_t reversed_depth{};
};
static_assert(
    sizeof(ProbeConstants) % 16 == 0,
    "the constant buffer must be a multiple of 16 bytes");

[[nodiscard]] float from_bits(const std::uint32_t bits) noexcept
{
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

}

std::uint32_t probe_stride_for(
    const std::uint32_t width,
    const std::uint32_t height) noexcept
{
    if (width == 0 || height == 0) {
        return 1;
    }
    const auto texels =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (texels <= kMaximumSamples) {
        return 1;
    }

    const auto ratio =
        static_cast<double>(texels) / static_cast<double>(kMaximumSamples);
    const auto stride =
        static_cast<std::uint32_t>(std::ceil(std::sqrt(ratio)));
    return stride == 0 ? 1U : stride;
}

const char* describe(const ProbeStatus status) noexcept
{
    switch (status) {
    case ProbeStatus::complete:
        return "complete";
    case ProbeStatus::idle:
        return "idle";
    case ProbeStatus::pending:
        return "pending: the readback has not arrived yet";
    case ProbeStatus::abandoned_readback_timed_out:
        return "abandoned: the readback did not arrive within the frame budget";
    case ProbeStatus::no_device:
        return "no device or context";
    case ProbeStatus::no_source:
        return "no source resource";
    case ProbeStatus::source_format_unsupported:
        return "the source format has no sampleable interpretation";
    case ProbeStatus::source_not_shader_readable:
        return "the source is not bound as a shader resource";
    case ProbeStatus::shader_compilation_failed:
        return "the reduction shader could not be compiled";
    case ProbeStatus::resource_creation_failed:
        return "the reduction resources could not be created";
    }
    return "unknown";
}

float ProbeStatistics::bucket_percentile(const float fraction) const noexcept
{
    std::uint64_t total = 0;
    for (const auto count : histogram) {
        total += count;
    }
    if (total == 0) {
        return 0.0F;
    }
    const auto clamped = std::clamp(fraction, 0.0F, 1.0F);
    const auto wanted = static_cast<std::uint64_t>(
        static_cast<double>(total) * static_cast<double>(clamped));
    std::uint64_t running = 0;
    for (std::uint32_t bucket = 0; bucket < kProbeHistogramBuckets; ++bucket) {
        running += histogram[bucket];
        if (running >= wanted) {
            return static_cast<float>(bucket + 1) /
                   static_cast<float>(kProbeHistogramBuckets);
        }
    }
    return 1.0F;
}

struct ResourceProbe::State
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> results;
    ComPtr<ID3D11UnorderedAccessView> results_view;
    ComPtr<ID3D11Buffer> staging;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11ShaderResourceView> source_view;
    ID3D11Texture2D* source_identity{};

    bool armed{};
    bool readback_pending{};
    std::uint32_t frames_waiting{};
    ProbeStatus status{ProbeStatus::idle};
    ProbeStatistics statistics{};
    ProbeParams pending_params{};
    std::uint32_t pending_width{};
    std::uint32_t pending_height{};
    std::uint32_t pending_stride{};

    [[nodiscard]] bool ensure_device_objects(ID3D11Device* new_device);
    [[nodiscard]] bool ensure_source_view(
        ID3D11Device* new_device,
        ID3D11Texture2D* source,
        const D3D11_TEXTURE2D_DESC& description);
    void release_device_objects() noexcept;
};

void ResourceProbe::State::release_device_objects() noexcept
{
    source_view.Reset();
    constants.Reset();
    staging.Reset();
    results_view.Reset();
    results.Reset();
    shader.Reset();
    device.Reset();
    source_identity = nullptr;
    readback_pending = false;
    frames_waiting = 0;
}

bool ResourceProbe::State::ensure_device_objects(ID3D11Device* const new_device)
{
    if (device.Get() == new_device && shader != nullptr) {
        return true;
    }

    release_device_objects();
    device = new_device;

    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    auto hr = D3DCompile(
        kProbeShader,
        sizeof(kProbeShader) - 1,
        "ResourceProbe.hlsl",
        nullptr,
        nullptr,
        "main",
        "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &code,
        &errors);
    if (FAILED(hr) || code == nullptr) {
        release_device_objects();
        return false;
    }
    hr = device->CreateComputeShader(
        code->GetBufferPointer(),
        code->GetBufferSize(),
        nullptr,
        &shader);
    if (FAILED(hr)) {
        release_device_objects();
        return false;
    }

    D3D11_BUFFER_DESC results_description{};
    results_description.ByteWidth = kSlotCount * sizeof(std::uint32_t);
    results_description.Usage = D3D11_USAGE_DEFAULT;
    results_description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    results_description.MiscFlags =
        D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(device->CreateBuffer(
            &results_description, nullptr, &results))) {
        release_device_objects();
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC view_description{};
    view_description.Format = DXGI_FORMAT_R32_TYPELESS;
    view_description.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    view_description.Buffer.FirstElement = 0;
    view_description.Buffer.NumElements = kSlotCount;
    view_description.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(device->CreateUnorderedAccessView(
            results.Get(), &view_description, &results_view))) {
        release_device_objects();
        return false;
    }

    auto staging_description = results_description;
    staging_description.Usage = D3D11_USAGE_STAGING;
    staging_description.BindFlags = 0;
    staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_description.MiscFlags = 0;
    if (FAILED(device->CreateBuffer(
            &staging_description, nullptr, &staging))) {
        release_device_objects();
        return false;
    }

    D3D11_BUFFER_DESC constant_description{};
    constant_description.ByteWidth = sizeof(ProbeConstants);
    constant_description.Usage = D3D11_USAGE_DYNAMIC;
    constant_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constant_description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(
            &constant_description, nullptr, &constants))) {
        release_device_objects();
        return false;
    }
    return true;
}

bool ResourceProbe::State::ensure_source_view(
    ID3D11Device* const new_device,
    ID3D11Texture2D* const source,
    const D3D11_TEXTURE2D_DESC& description)
{
    if (source_view != nullptr && source_identity == source) {
        return true;
    }
    source_view.Reset();
    source_identity = nullptr;

    DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN;
    if (!srv_format_for(
            description.Format, description.BindFlags, srv_format)) {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC view_description{};
    view_description.Format = srv_format;
    view_description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view_description.Texture2D.MipLevels = 1;
    if (FAILED(new_device->CreateShaderResourceView(
            source, &view_description, &source_view))) {
        return false;
    }
    source_identity = source;
    return true;
}

ResourceProbe::ResourceProbe() : state_(std::make_unique<State>())
{
}

ResourceProbe::~ResourceProbe() = default;

void ResourceProbe::arm() noexcept
{
    if (state_ == nullptr) {
        return;
    }
    state_->armed = true;
}

bool ResourceProbe::armed() const noexcept
{
    return state_ != nullptr && state_->armed;
}

ProbeStatus ResourceProbe::status() const noexcept
{
    return state_ != nullptr ? state_->status : ProbeStatus::idle;
}

const ProbeStatistics& ResourceProbe::statistics() const noexcept
{
    static const ProbeStatistics empty{};
    return state_ != nullptr ? state_->statistics : empty;
}

bool ResourceProbe::holds_resources() const noexcept
{
    return state_ != nullptr && state_->shader != nullptr;
}

ProbeStatus ResourceProbe::collect(ID3D11DeviceContext* const context)
{
    if (state_ == nullptr) {
        return ProbeStatus::idle;
    }
    if (!state_->readback_pending) {
        return state_->status;
    }
    if (context == nullptr || state_->staging == nullptr) {
        state_->readback_pending = false;
        state_->status = ProbeStatus::no_device;
        return state_->status;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};

    const auto hr = context->Map(
        state_->staging.Get(),
        0,
        D3D11_MAP_READ,
        D3D11_MAP_FLAG_DO_NOT_WAIT,
        &mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING || hr == S_FALSE) {
        ++state_->frames_waiting;
        if (state_->frames_waiting > kCollectionFrameBudget) {
            state_->readback_pending = false;
            state_->status = ProbeStatus::abandoned_readback_timed_out;
        } else {
            state_->status = ProbeStatus::pending;
        }
        return state_->status;
    }
    if (FAILED(hr) || mapped.pData == nullptr) {
        state_->readback_pending = false;
        state_->status = ProbeStatus::abandoned_readback_timed_out;
        return state_->status;
    }

    std::array<std::uint32_t, kSlotCount> slots{};
    std::memcpy(slots.data(), mapped.pData, sizeof(slots));
    context->Unmap(state_->staging.Get(), 0);
    state_->readback_pending = false;
    state_->frames_waiting = 0;

    auto& out = state_->statistics;
    out = ProbeStatistics{};
    out.channel = state_->pending_params.channel;
    out.source_width = state_->pending_width;
    out.source_height = state_->pending_height;
    out.stride = state_->pending_stride;
    out.sampled_pixels = slots[kSlotSampled];

    out.minimum = slots[kSlotMinimum] == kMinimumSeed ?
        0.0F : from_bits(slots[kSlotMinimum]);
    out.maximum = from_bits(slots[kSlotMaximum]);
    out.nonfinite_pixels = slots[kSlotNonfinite];

    if (!std::isfinite(out.maximum) && out.nonfinite_pixels == 0) {
        out.nonfinite_pixels = 1;
    }
    out.out_of_range_pixels = slots[kSlotOutOfRange];
    out.far_pixels = slots[kSlotFar];
    out.near_zero_pixels = slots[kSlotNearZero];
    if (slots[kSlotBoundsLeft] == kBoundsMinimumSeed) {

        out.bounds_left = 1;
        out.bounds_top = 1;
        out.bounds_right = 0;
        out.bounds_bottom = 0;
    } else {
        out.bounds_left = static_cast<std::int32_t>(slots[kSlotBoundsLeft]);
        out.bounds_top = static_cast<std::int32_t>(slots[kSlotBoundsTop]);
        out.bounds_right = static_cast<std::int32_t>(slots[kSlotBoundsRight]);
        out.bounds_bottom =
            static_cast<std::int32_t>(slots[kSlotBoundsBottom]);
    }
    out.quadrant_right_down = slots[kSlotQuadrantRightDown];
    out.quadrant_right_up = slots[kSlotQuadrantRightUp];
    out.quadrant_left_down = slots[kSlotQuadrantLeftDown];
    out.quadrant_left_up = slots[kSlotQuadrantLeftUp];
    for (std::uint32_t bucket = 0; bucket < kProbeHistogramBuckets; ++bucket) {
        out.histogram[bucket] = slots[kSlotHistogram + bucket];
    }
    out.valid = out.sampled_pixels != 0;
    state_->status =
        out.valid ? ProbeStatus::complete : ProbeStatus::no_source;
    return state_->status;
}

ProbeStatus ResourceProbe::capture(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context,
    ID3D11Texture2D* const source,
    const ProbeParams& params)
{
    if (state_ == nullptr) {
        return ProbeStatus::idle;
    }

    if (state_->readback_pending) {
        static_cast<void>(collect(context));
    }
    if (!state_->armed) {
        return state_->status;
    }
    if (device == nullptr || context == nullptr) {
        state_->status = ProbeStatus::no_device;
        return state_->status;
    }
    if (source == nullptr) {
        state_->status = ProbeStatus::no_source;
        return state_->status;
    }

    if (state_->readback_pending) {
        return state_->status;
    }

    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    if ((description.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) {
        state_->armed = false;
        state_->status = ProbeStatus::source_not_shader_readable;
        return state_->status;
    }
    if (!state_->ensure_device_objects(device)) {
        state_->armed = false;
        state_->status = ProbeStatus::shader_compilation_failed;
        return state_->status;
    }
    if (!state_->ensure_source_view(device, source, description)) {
        state_->armed = false;
        state_->status = ProbeStatus::source_format_unsupported;
        return state_->status;
    }

    const auto stride =
        probe_stride_for(description.Width, description.Height);

    const UINT zero[4]{0, 0, 0, 0};
    context->ClearUnorderedAccessViewUint(state_->results_view.Get(), zero);
    {

        std::array<std::uint32_t, kSlotCount> seed{};
        seed[kSlotMinimum] = kMinimumSeed;
        seed[kSlotMaximum] = 0;
        seed[kSlotBoundsLeft] = kBoundsMinimumSeed;
        seed[kSlotBoundsTop] = kBoundsMinimumSeed;
        seed[kSlotBoundsRight] = 0;
        seed[kSlotBoundsBottom] = 0;
        context->UpdateSubresource(
            state_->results.Get(), 0, nullptr, seed.data(), 0, 0);
    }

    D3D11_MAPPED_SUBRESOURCE mapped_constants{};
    if (FAILED(context->Map(
            state_->constants.Get(),
            0,
            D3D11_MAP_WRITE_DISCARD,
            0,
            &mapped_constants)) ||
        mapped_constants.pData == nullptr) {
        state_->armed = false;
        state_->status = ProbeStatus::resource_creation_failed;
        return state_->status;
    }
    ProbeConstants values{};
    values.extent_x = description.Width;
    values.extent_y = description.Height;
    values.stride = stride;
    values.mode = static_cast<std::uint32_t>(params.channel);
    values.far_epsilon = params.far_epsilon;
    values.near_zero = params.near_zero_threshold;
    values.magnitude_scale = params.maximum_expected_magnitude;
    values.reversed_depth = params.reversed_depth ? 1U : 0U;
    std::memcpy(mapped_constants.pData, &values, sizeof(values));
    context->Unmap(state_->constants.Get(), 0);

    ComPtr<ID3D11ComputeShader> saved_shader;
    UINT saved_instance_count = 0;
    context->CSGetShader(&saved_shader, nullptr, &saved_instance_count);
    ComPtr<ID3D11ShaderResourceView> saved_source;
    context->CSGetShaderResources(0, 1, &saved_source);
    ComPtr<ID3D11UnorderedAccessView> saved_unordered;
    context->CSGetUnorderedAccessViews(0, 1, &saved_unordered);
    ComPtr<ID3D11Buffer> saved_constants;
    context->CSGetConstantBuffers(0, 1, &saved_constants);

    ID3D11ShaderResourceView* source_views[]{state_->source_view.Get()};
    ID3D11UnorderedAccessView* unordered_views[]{state_->results_view.Get()};
    ID3D11Buffer* constant_buffers[]{state_->constants.Get()};
    context->CSSetShader(state_->shader.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 1, source_views);
    context->CSSetUnorderedAccessViews(0, 1, unordered_views, nullptr);
    context->CSSetConstantBuffers(0, 1, constant_buffers);

    const auto sampled_x = (description.Width + stride - 1) / stride;
    const auto sampled_y = (description.Height + stride - 1) / stride;
    context->Dispatch(
        (sampled_x + kThreadGroupSize - 1) / kThreadGroupSize,
        (sampled_y + kThreadGroupSize - 1) / kThreadGroupSize,
        1);

    ID3D11ShaderResourceView* const no_source[]{nullptr};
    ID3D11UnorderedAccessView* const no_unordered[]{nullptr};
    context->CSSetShaderResources(0, 1, no_source);
    context->CSSetUnorderedAccessViews(0, 1, no_unordered, nullptr);
    ID3D11ShaderResourceView* restored_source[]{saved_source.Get()};
    ID3D11UnorderedAccessView* restored_unordered[]{saved_unordered.Get()};
    ID3D11Buffer* restored_constants[]{saved_constants.Get()};
    context->CSSetShader(saved_shader.Get(), nullptr, saved_instance_count);
    context->CSSetShaderResources(0, 1, restored_source);
    context->CSSetUnorderedAccessViews(0, 1, restored_unordered, nullptr);
    context->CSSetConstantBuffers(0, 1, restored_constants);

    context->CopyResource(state_->staging.Get(), state_->results.Get());

    state_->armed = false;
    state_->readback_pending = true;
    state_->frames_waiting = 0;
    state_->pending_params = params;
    state_->pending_width = description.Width;
    state_->pending_height = description.Height;
    state_->pending_stride = stride;
    state_->status = ProbeStatus::pending;
    return state_->status;
}

void ResourceProbe::shutdown() noexcept
{
    if (state_ == nullptr) {
        return;
    }
    state_->release_device_objects();
    state_->armed = false;
    state_->status = ProbeStatus::idle;
    state_->statistics = ProbeStatistics{};
}
}
