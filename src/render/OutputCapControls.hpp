#pragma once

#include "render/FramePacingRules.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace mfgdlss::render
{

inline constexpr std::array kOutputCapPresets{
    0U,
    30U,
    40U,
    45U,
    60U,
    72U,
    80U,
    90U,
    120U,
    144U,
    165U,
    240U,
    360U,
    480U};

[[nodiscard]] constexpr bool valid_output_cap(
    const std::uint32_t value) noexcept
{
    return value == 0U ||
           (value >= kMinimumPresentationCap &&
            value <= kMaximumPresentationCap);
}

[[nodiscard]] constexpr bool output_cap_is_preset(
    const std::uint32_t value) noexcept
{
    for (const auto preset : kOutputCapPresets) {
        if (preset == value) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] constexpr std::optional<std::uint32_t>
parse_output_cap_digits(const std::string_view digits) noexcept
{
    if (digits.empty() || digits.size() > 4U) {
        return std::nullopt;
    }

    std::uint32_t value{};
    for (const auto digit : digits) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        value = value * 10U +
                static_cast<std::uint32_t>(digit - '0');
    }
    return valid_output_cap(value) ?
        std::optional<std::uint32_t>{value} : std::nullopt;
}

[[nodiscard]] constexpr std::uint32_t base_for_output_preset(
    const std::uint32_t output,
    const std::uint32_t multiplier) noexcept
{
    if (multiplier < 2U || output == 0U) {
        return 0U;
    }
    const auto base = output / multiplier;
    if (base < kMinimumPresentationCap || base * multiplier != output) {
        return 0U;
    }
    return base;
}

[[nodiscard]] constexpr std::uint32_t adjust_output_cap(
    const std::uint32_t current,
    const int direction,
    const std::uint32_t custom_step = 0U) noexcept
{
    if (direction == 0) {
        return current;
    }

    if (custom_step != 0U) {
        if (direction > 0) {
            if (current == 0U) {
                return kMinimumPresentationCap;
            }
            return current >= kMaximumPresentationCap -
                                  (std::min)(
                                      custom_step,
                                      kMaximumPresentationCap) ?
                kMaximumPresentationCap : current + custom_step;
        }
        if (current <= kMinimumPresentationCap) {
            return 0U;
        }
        return current - kMinimumPresentationCap <= custom_step ?
            kMinimumPresentationCap : current - custom_step;
    }

    if (direction > 0) {
        for (const auto preset : kOutputCapPresets) {
            if (preset > current) {
                return preset;
            }
        }
        return kOutputCapPresets.front();
    }

    auto previous = kOutputCapPresets.back();
    for (const auto preset : kOutputCapPresets) {
        if (preset >= current) {
            return previous;
        }
        previous = preset;
    }
    return previous;
}

[[nodiscard]] constexpr std::uint32_t adjust_base_by_output(
    const std::uint32_t current_base,
    const std::uint32_t multiplier,
    const int direction) noexcept
{
    if (multiplier < 2U || direction == 0 ||
        current_base < kMinimumPresentationCap) {
        return current_base;
    }
    auto candidate = current_base * multiplier;
    for (std::size_t step = 0; step < kOutputCapPresets.size(); ++step) {
        candidate = adjust_output_cap(candidate, direction);
        const auto base = base_for_output_preset(candidate, multiplier);
        if (base != 0U && base != current_base) {
            return base;
        }
    }
    return current_base;
}
}
