#pragma once

#include "render/FramePacingRules.hpp"

#include <cstdint>

namespace mfgdlss::render
{
[[nodiscard]] constexpr bool base_frame_limit_active(
    const std::uint32_t limit,
    const GenerationCadence cadence) noexcept
{
    return limit != 0U && cadence == GenerationCadence::fixed_multiplier;
}
}
