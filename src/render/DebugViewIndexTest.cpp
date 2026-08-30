#include "render/DebugViewCore.hpp"

#include <array>
#include <cstdint>

using namespace mfgdlss::render;

int main()
{
    int failures{};
    const auto check = [&failures](const bool condition) {
        failures += condition ? 0 : 1;
    };

    constexpr std::array kAllViews{
        DebugView::off,
        DebugView::motion_vectors,
        DebugView::depth,
        DebugView::scene_reference,
        DebugView::violations,
        DebugView::generator_motion,
        DebugView::generator_depth,
        DebugView::ui_layer};

    for (std::uint32_t index = 0U; index < kAllViews.size(); ++index) {
        check(debug_view_from_index(index) == kAllViews[index]);
    }

    check(debug_view_from_index(5U) == DebugView::generator_motion);
    check(debug_view_from_index(6U) == DebugView::generator_depth);
    check(debug_view_from_index(7U) == DebugView::ui_layer);

    check(debug_view_from_index(8U) == DebugView::off);
    check(debug_view_from_index(99U) == DebugView::off);
    check(debug_view_from_index(0xFFFFFFFFU) == DebugView::off);

    check(debug_view_from_index(0U) == DebugView::off);

    static_assert(
        debug_view_from_index(7U) == DebugView::ui_layer,
        "the highest documented [Debug] View value must survive the bound");
    static_assert(
        debug_view_from_index(8U) == DebugView::off,
        "a value above the highest view must fall back to off");

    return failures == 0 ? 0 : 1;
}
