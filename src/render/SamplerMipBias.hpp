#pragma once

#include <cmath>
#include <cstdint>

namespace mfgdlss::render
{
inline constexpr std::uint32_t kSamplerFilterTrilinear = 0x15U;
inline constexpr std::uint32_t kSamplerFilterAnisotropic = 0x55U;
inline constexpr std::uint32_t kSamplerFilterComparisonBit = 0x80U;
inline constexpr float kSamplerMipBiasFloor = -4.0F;

[[nodiscard]] inline float mip_bias_for_scale(
    const std::uint32_t render_extent,
    const std::uint32_t output_extent) noexcept
{
    if (render_extent == 0U || output_extent == 0U) {
        return 0.0F;
    }
    const auto ratio = static_cast<float>(render_extent) /
        static_cast<float>(output_extent);
    if (!(ratio > 0.0F) || ratio > 1.0F) {
        return 0.0F;
    }
    const auto bias = std::log2(ratio) - 1.0F;
    return bias < kSamplerMipBiasFloor ? kSamplerMipBiasFloor : bias;
}

[[nodiscard]] inline bool sampler_carries_material_mip_chain(
    const std::uint32_t filter,
    const float existing_bias,
    const float maximum_lod) noexcept
{
    if (existing_bias != 0.0F) {
        return false;
    }
    if (!(maximum_lod > 1.0F)) {
        return false;
    }
    if ((filter & kSamplerFilterComparisonBit) != 0U) {
        return false;
    }
    return filter == kSamplerFilterAnisotropic ||
        filter == kSamplerFilterTrilinear;
}

void install_sampler_mip_bias(void* d3d11_device);
}
