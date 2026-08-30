#pragma once

namespace mfgdlss::render
{

struct Rectangle
{
    float left{};
    float top{};
    float right{};
    float bottom{};

    [[nodiscard]] constexpr bool usable() const noexcept
    {
        return right > left && bottom > top;
    }

    [[nodiscard]] constexpr bool contains(
        const float x,
        const float y) const noexcept
    {
        return usable() &&
               x >= left && x <= right &&
               y >= top && y <= bottom;
    }
};
}
