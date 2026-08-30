#pragma once

#include <cstddef>
#include <cstdint>

namespace mfgdlss::render
{
template <class Choices>
[[nodiscard]] constexpr std::size_t usable_choice_count(
    const Choices& choices,
    const std::uint32_t ceiling) noexcept
{
    if (choices.size() == 0U) {
        return 0U;
    }
    if (ceiling == 0U) {
        return choices.size();
    }
    std::size_t count = 1U;
    for (std::size_t index = 1U; index < choices.size(); ++index) {
        if (choices[index] <= ceiling) {
            count = index + 1U;
        }
    }
    return count;
}
}
