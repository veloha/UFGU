

#include "render/ResourceProbe.hpp"

#include "render/DebugViewCore.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace mfgdlss::render;

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

constexpr UINT kWidth = 64;
constexpr UINT kHeight = 64;
constexpr UINT kPixels = kWidth * kHeight;

[[nodiscard]] bool make_depth_texture(
    ID3D11Device* const device,
    const std::vector<std::uint32_t>& packed,
    ComPtr<ID3D11Texture2D>& texture)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = kWidth;
    description.Height = kHeight;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R24G8_TYPELESS;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags =
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;

    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = packed.data();
    initial.SysMemPitch = kWidth * sizeof(std::uint32_t);
    return SUCCEEDED(
        device->CreateTexture2D(&description, &initial, &texture));
}

[[nodiscard]] bool make_motion_texture(
    ID3D11Device* const device,
    const std::vector<float>& components,
    ComPtr<ID3D11Texture2D>& texture)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = kWidth;
    description.Height = kHeight;
    description.MipLevels = 1;
    description.ArraySize = 1;

    description.Format = DXGI_FORMAT_R32G32_FLOAT;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = components.data();
    initial.SysMemPitch = kWidth * 2 * sizeof(float);
    return SUCCEEDED(
        device->CreateTexture2D(&description, &initial, &texture));
}

[[nodiscard]] ProbeStatus run_to_completion(
    ResourceProbe& probe,
    ID3D11Device* const device,
    ID3D11DeviceContext* const context,
    ID3D11Texture2D* const source,
    const ProbeParams& params)
{
    probe.arm();
    auto status = probe.capture(device, context, source, params);
    context->Flush();
    for (std::uint32_t i = 0;
         i < kCollectionFrameBudget && status == ProbeStatus::pending;
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        status = probe.collect(context);
    }
    if (status != ProbeStatus::complete) {
        std::printf("        probe status: %s\n", describe(status));
    }
    return status;
}

[[nodiscard]] std::uint32_t pack_depth(const float value)
{
    const auto clamped = value < 0.0F ? 0.0F : (value > 1.0F ? 1.0F : value);
    const auto scaled =
        static_cast<std::uint32_t>(clamped * 16777215.0F + 0.5F);
    return scaled > 0x00FFFFFFU ? 0x00FFFFFFU : scaled;
}

void test_the_cleared_surface(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context)
{
    std::printf(
        "-- an entirely zero depth surface: the failure that was reported --\n");

    const std::vector<std::uint32_t> zeros(kPixels, 0U);
    ComPtr<ID3D11Texture2D> texture;
    if (!make_depth_texture(device, zeros, texture)) {
        check(false, "the zero depth texture was created");
        return;
    }

    ProbeParams params{};
    params.channel = ProbeChannel::depth;
    params.reversed_depth = true;
    params.far_epsilon = depth_far_epsilon_for(
        DXGI_FORMAT_R24_UNORM_X8_TYPELESS);

    ResourceProbe probe;
    check(!probe.holds_resources(),
          "a probe that has never captured holds no device objects");
    const auto status =
        run_to_completion(probe, device, context, texture.Get(), params);
    check(status == ProbeStatus::complete,
          "the capture completed through the asynchronous readback without "
          "the caller ever blocking on the GPU");

    const auto& stats = probe.statistics();
    check(stats.valid, "the statistics are marked valid");
    check(stats.stride == 1 && stats.sampled_pixels == kPixels,
          "every texel of a 64x64 surface was sampled");
    check(stats.minimum == 0.0F && stats.maximum == 0.0F,
          "min and max are both exactly 0");
    check(stats.far_pixels == kPixels,
          "ALL 4096 texels are at the far plane: under reversed-Z this is a "
          "cleared buffer, and it is precisely what the magenta frame showed");
    check(stats.bounds_empty(),
          "the non-far bounding box is empty, so there is no geometry anywhere "
          "in the buffer");
    check(stats.nonfinite_pixels == 0 && stats.out_of_range_pixels == 0,
          "nothing is non-finite or out of range: the buffer is not corrupt, "
          "it is empty");
    check(stats.histogram[0] == kPixels,
          "the whole distribution sits in the lowest nearness bucket");

    std::printf("\n");
}

void test_a_real_depth_distribution(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context)
{
    std::printf("-- a depth surface with actual content --\n");

    std::vector<std::uint32_t> packed(kPixels, 0U);
    for (UINT y = 0; y < kHeight; ++y) {
        for (UINT x = 0; x < kWidth; ++x) {
            const auto nearness =
                x == 0 ? 0.0F :
                    static_cast<float>(x) / static_cast<float>(kWidth - 1);
            packed[static_cast<std::size_t>(y) * kWidth + x] =
                pack_depth(nearness);
        }
    }

    ComPtr<ID3D11Texture2D> texture;
    if (!make_depth_texture(device, packed, texture)) {
        check(false, "the gradient depth texture was created");
        return;
    }

    ProbeParams params{};
    params.channel = ProbeChannel::depth;
    params.reversed_depth = true;
    params.far_epsilon = depth_far_epsilon_for(
        DXGI_FORMAT_R24_UNORM_X8_TYPELESS);

    ResourceProbe probe;
    const auto status =
        run_to_completion(probe, device, context, texture.Get(), params);
    check(status == ProbeStatus::complete, "the capture completed");

    const auto& stats = probe.statistics();
    std::printf(
        "        measured: min=%.6f max=%.6f far=%u sampled=%u "
        "bounds=%d,%d..%d,%d\n",
        static_cast<double>(stats.minimum),
        static_cast<double>(stats.maximum),
        stats.far_pixels,
        stats.sampled_pixels,
        stats.bounds_left,
        stats.bounds_top,
        stats.bounds_right,
        stats.bounds_bottom);
    check(stats.far_pixels == kHeight,
          "exactly the 64 texels of column 0 are at the far plane");
    check(std::fabs(stats.maximum - 1.0F) < 0.001F,
          "the maximum reaches the near plane");
    check(stats.minimum == 0.0F, "the minimum is the far plane");
    check(stats.bounds_left == 1 && stats.bounds_right ==
              static_cast<std::int32_t>(kWidth - 1),
          "the non-far bounding box starts at column 1 and ends at the last "
          "column, which is exactly where the content is");
    check(stats.bounds_top == 0 && stats.bounds_bottom ==
              static_cast<std::int32_t>(kHeight - 1),
          "and spans every row");

    std::uint64_t histogram_total = 0;
    std::uint32_t occupied_buckets = 0;
    for (const auto count : stats.histogram) {
        histogram_total += count;
        if (count != 0) {
            ++occupied_buckets;
        }
    }
    check(histogram_total == kPixels,
          "the histogram accounts for every sampled texel");
    check(occupied_buckets > 4,
          "and spreads across the range rather than collapsing into one "
          "bucket, which is what makes it diagnostic");
    check(stats.bucket_percentile(0.5F) > 0.0F &&
              stats.bucket_percentile(0.5F) <= 1.0F,
          "a median can be derived from the histogram");

    std::printf("\n");
}

void test_motion_statistics(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context)
{
    std::printf("-- motion vector statistics --\n");

    std::vector<float> components(
        static_cast<std::size_t>(kPixels) * 2U, 0.0F);
    std::uint32_t expected_right_down = 0;
    std::uint32_t expected_left_up = 0;
    for (UINT y = 0; y < kHeight; ++y) {
        for (UINT x = 0; x < kWidth; ++x) {
            const auto index = (static_cast<std::size_t>(y) * kWidth + x) * 2U;
            if (y < kHeight / 2) {
                continue;
            }
            if (x < kWidth / 2) {
                components[index] = 0.3F;
                components[index + 1] = 0.4F;
                ++expected_right_down;
            } else {
                components[index] = -0.6F;
                components[index + 1] = -0.8F;
                ++expected_left_up;
            }
        }
    }

    ComPtr<ID3D11Texture2D> texture;
    if (!make_motion_texture(device, components, texture)) {
        check(false, "the motion texture was created");
        return;
    }

    ProbeParams params{};
    params.channel = ProbeChannel::motion;
    params.near_zero_threshold = 1.0e-5F;
    params.maximum_expected_magnitude = 1.0F;

    ResourceProbe probe;
    const auto status =
        run_to_completion(probe, device, context, texture.Get(), params);
    check(status == ProbeStatus::complete, "the capture completed");

    const auto& stats = probe.statistics();
    check(stats.near_zero_pixels == kPixels / 2,
          "exactly half the surface is measured as near-zero, so a dead zone "
          "can be chosen from data instead of guessed");
    check(std::fabs(stats.maximum - 1.0F) < 0.001F,
          "the maximum magnitude is the 1.0 that was written");
    check(stats.minimum == 0.0F, "and the minimum is the stationary 0");
    check(stats.quadrant_right_down == expected_right_down,
          "the right/down quadrant count is exact");
    check(stats.quadrant_left_up == expected_left_up,
          "the left/up quadrant count is exact");
    check(stats.quadrant_right_up == 0 && stats.quadrant_left_down == 0,
          "and no vectors are attributed to quadrants nothing was written to");
    check(stats.nonfinite_pixels == 0,
          "no non-finite vectors, which is the check that would have caught a "
          "corrupt motion resource");

    std::printf("\n");
}

void test_inertness_and_state(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context)
{
    std::printf("-- what the probe does when it is not armed --\n");

    const std::vector<std::uint32_t> zeros(kPixels, 0U);
    ComPtr<ID3D11Texture2D> texture;
    if (!make_depth_texture(device, zeros, texture)) {
        check(false, "the texture was created");
        return;
    }

    ProbeParams params{};
    params.channel = ProbeChannel::depth;
    params.far_epsilon = 0.0001F;

    ResourceProbe probe;
    check(!probe.armed(), "a fresh probe is not armed");
    const auto idle = probe.capture(device, context, texture.Get(), params);
    check(idle == ProbeStatus::idle,
          "capturing while unarmed reports idle");
    check(!probe.holds_resources(),
          "and allocates nothing: an unarmed probe costs one branch");

    ComPtr<ID3D11ShaderResourceView> sentinel_view;
    D3D11_SHADER_RESOURCE_VIEW_DESC view_description{};
    view_description.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    view_description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view_description.Texture2D.MipLevels = 1;
    if (SUCCEEDED(device->CreateShaderResourceView(
            texture.Get(), &view_description, &sentinel_view))) {
        ID3D11ShaderResourceView* bind[]{sentinel_view.Get()};
        context->CSSetShaderResources(0, 1, bind);

        static_cast<void>(
            run_to_completion(probe, device, context, texture.Get(), params));

        ComPtr<ID3D11ShaderResourceView> after;
        context->CSGetShaderResources(0, 1, &after);
        check(after.Get() == sentinel_view.Get(),
              "the compute shader-resource slot the probe borrowed is restored "
              "to exactly what the caller had bound");

        ComPtr<ID3D11UnorderedAccessView> unordered_after;
        context->CSGetUnorderedAccessViews(0, 1, &unordered_after);
        check(unordered_after.Get() == nullptr,
              "and the probe's own unordered-access view is not left bound");

        ID3D11ShaderResourceView* const clear[]{nullptr};
        context->CSSetShaderResources(0, 1, clear);
    }

    probe.shutdown();
    check(!probe.holds_resources(),
          "shutdown releases every device object");
    check(probe.status() == ProbeStatus::idle,
          "and returns the probe to idle");

    std::printf("\n");
}

void test_stride_arithmetic()
{
    std::printf("-- bounding the amount of work --\n");

    check(probe_stride_for(64, 64) == 1,
          "a small surface is walked texel by texel");
    check(probe_stride_for(512, 512) == 1,
          "262,144 texels is exactly the budget and needs no stride");
    const auto live = probe_stride_for(2560, 1440);
    check(live == 4,
          "the live 2560x1440 depth surface is walked with a stride of 4");
    const auto sampled =
        ((2560U + live - 1) / live) * ((1440U + live - 1) / live);
    check(sampled <= kMaximumSamples,
          "which keeps the sample count inside the budget");
    check(sampled > kMaximumSamples / 4,
          "while still using most of it, so the measurement stays "
          "representative");
    std::printf("        2560x1440 -> stride %u, %u samples (budget %u)\n",
                live, sampled, kMaximumSamples);
    check(probe_stride_for(7680, 4320) > live,
          "a larger surface gets a larger stride rather than more work");
    check(probe_stride_for(0, 0) == 1,
          "a degenerate extent does not divide by zero");

    std::printf("\n");
}
}

int main()
{
    std::printf("=== Bounded GPU-backed resource measurement ===\n\n");

    test_stride_arithmetic();

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    const auto hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_WARP,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &device,
        &level,
        &context);
    if (FAILED(hr) || device == nullptr || context == nullptr) {
        std::printf("FATAL: no WARP D3D11 device (0x%08X)\n",
                    static_cast<unsigned>(hr));
        return 2;
    }
    std::printf("WARP D3D11 device created, feature level 0x%X\n\n",
                static_cast<unsigned>(level));

    test_the_cleared_surface(device.Get(), context.Get());
    test_a_real_depth_distribution(device.Get(), context.Get());
    test_motion_statistics(device.Get(), context.Get());
    test_inertness_and_state(device.Get(), context.Get());

    std::printf(
        "=== ResourceProbeTest: %d passed, %d failed ===\n",
        g_passed,
        g_failed);
    return g_failed == 0 ? 0 : 1;
}
