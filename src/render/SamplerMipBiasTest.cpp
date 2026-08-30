#include "render/SamplerMipBias.hpp"

#include <Windows.h>
#include <d3d11.h>

#include <cmath>
#include <cstdio>
#include <string>

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

using namespace mfgdlss::render;

[[nodiscard]] bool close_to(const float value, const float expected)
{
    return std::fabs(value - expected) < 0.001F;
}

static_assert(
    kSamplerFilterAnisotropic ==
        static_cast<std::uint32_t>(D3D11_FILTER_ANISOTROPIC),
    "the header constant must equal the real D3D11 anisotropic filter");
static_assert(
    kSamplerFilterTrilinear ==
        static_cast<std::uint32_t>(D3D11_FILTER_MIN_MAG_MIP_LINEAR),
    "the header constant must equal the real D3D11 trilinear filter");
static_assert(
    kSamplerFilterComparisonBit ==
        static_cast<std::uint32_t>(D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT),
    "the comparison bit must equal the base of the D3D11 comparison filters");

void test_the_vendor_formula_is_followed()
{
    std::printf("Both vendors publish log2(render/display) minus one\n");

    check(
        close_to(mip_bias_for_scale(3840U, 3840U), -1.0F),
        "native rendering, which is what DLAA does, still takes the minus one "
        "term");
    check(
        close_to(mip_bias_for_scale(2560U, 3840U), -1.585F),
        "a two thirds render scale gives about -1.585");
    check(
        close_to(mip_bias_for_scale(1920U, 3840U), -2.0F),
        "half render scale gives exactly -2.0");
    check(
        close_to(mip_bias_for_scale(1280U, 3840U), -2.585F),
        "a one third render scale gives about -2.585");
    std::printf("\n");
}

void test_nothing_is_biased_when_nothing_is_upscaled()
{
    std::printf("A scene not rendered below the output is left alone\n");

    check(
        mip_bias_for_scale(0U, 3840U) == 0.0F,
        "a zero render extent yields no bias rather than negative infinity");
    check(
        mip_bias_for_scale(3840U, 0U) == 0.0F,
        "a zero output extent yields no bias rather than a divide by zero");
    check(
        mip_bias_for_scale(7680U, 3840U) == 0.0F,
        "supersampling yields no bias, because the formula is only defined "
        "for rendering below the output");
    std::printf("\n");
}

void test_the_bias_cannot_run_away()
{
    std::printf("An absurd ratio is clamped rather than trusted\n");

    check(
        mip_bias_for_scale(1U, 3840U) == -4.0F,
        "a pathological ratio clamps at -4.0 instead of blurring every "
        "texture into its smallest mip");
    std::printf("\n");
}

void test_only_material_samplers_are_touched()
{
    std::printf("Shadow, UI and already-biased samplers are left alone\n");

    check(
        sampler_carries_material_mip_chain(
            static_cast<std::uint32_t>(D3D11_FILTER_ANISOTROPIC),
            0.0F,
            D3D11_FLOAT32_MAX),
        "an anisotropic sampler with a full mip chain is a material sampler");
    check(
        sampler_carries_material_mip_chain(
            static_cast<std::uint32_t>(D3D11_FILTER_MIN_MAG_MIP_LINEAR),
            0.0F,
            D3D11_FLOAT32_MAX),
        "a trilinear sampler with a full mip chain is a material sampler");

    check(
        !sampler_carries_material_mip_chain(
            static_cast<std::uint32_t>(D3D11_FILTER_ANISOTROPIC),
            -1.0F,
            D3D11_FLOAT32_MAX),
        "a sampler the engine already biased is never biased again, because "
        "stacking two biases blurs twice as far as either intended");
    check(
        !sampler_carries_material_mip_chain(
            static_cast<std::uint32_t>(D3D11_FILTER_ANISOTROPIC),
            0.0F,
            0.0F),
        "a single mip sampler has nothing to bias, which is where UI textures "
        "live");
    check(
        !sampler_carries_material_mip_chain(
            static_cast<std::uint32_t>(
                D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT),
            0.0F,
            D3D11_FLOAT32_MAX),
        "a comparison sampler is refused, which is what shadow map lookups "
        "use and where a bias would visibly break the shadows");
    check(
        !sampler_carries_material_mip_chain(
            static_cast<std::uint32_t>(D3D11_FILTER_MIN_MAG_MIP_POINT),
            0.0F,
            D3D11_FLOAT32_MAX),
        "a point sampler is refused, because point sampling is chosen when "
        "exact texels matter");
    std::printf("\n");
}
}

int main()
{
    std::printf("=== Sampler mip bias ===\n\n");
    test_the_vendor_formula_is_followed();
    test_nothing_is_biased_when_nothing_is_upscaled();
    test_the_bias_cannot_run_away();
    test_only_material_samplers_are_touched();
    std::printf(
        "=== SamplerMipBiasTest: %d passed, %d failed ===\n",
        g_passed,
        g_failed);
    return g_failed == 0 ? 0 : 1;
}
