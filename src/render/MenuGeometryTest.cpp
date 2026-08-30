#include "render/MenuGeometry.hpp"

#include <cstdio>

using mfgdlss::render::Rectangle;

int main()
{
    int failures{};
    const auto check = [&failures](const bool condition, const char* name) {
        if (!condition) {
            ++failures;
            std::printf("FAIL %s\n", name);
        }
    };

    constexpr Rectangle control{100.0F, 200.0F, 300.0F, 240.0F};

    check(control.usable(), "a control with positive extent is usable");
    check(
        control.contains(200.0F, 220.0F),
        "a point inside a real control is inside");
    check(
        control.contains(100.0F, 200.0F) &&
            control.contains(300.0F, 240.0F),
        "both corners are inclusive, which is how the menu has always hit "
        "tested");
    check(
        !control.contains(99.0F, 220.0F) &&
            !control.contains(301.0F, 220.0F) &&
            !control.contains(200.0F, 199.0F) &&
            !control.contains(200.0F, 241.0F),
        "a point outside any edge is outside");

    constexpr Rectangle unlaid_out{};

    check(
        !unlaid_out.usable(),
        "a rectangle the layout never filled in is not usable");
    check(
        !unlaid_out.contains(0.0F, 0.0F),
        "and it does not claim the origin, which is where the menu cursor "
        "clamps in the top left corner");
    check(
        !unlaid_out.contains(1.0F, 1.0F) &&
            !unlaid_out.contains(-1.0F, -1.0F),
        "nor any other point");

    constexpr Rectangle zero_height{-8.0F, 0.0F, 0.0F, 0.0F};

    check(
        !zero_height.usable(),
        "the sharpness slider hit area derived from an unlaid out value cell "
        "has width but no height, and must still be rejected");
    check(
        !zero_height.contains(0.0F, 0.0F),
        "so a click in the top left corner cannot start a sharpness drag from "
        "a section that does not show the sharpness row");

    constexpr Rectangle inverted{300.0F, 240.0F, 100.0F, 200.0F};

    check(!inverted.usable(), "an inverted rectangle is not usable");
    check(
        !inverted.contains(200.0F, 220.0F),
        "and contains nothing, rather than inverting the test");

    constexpr Rectangle degenerate_width{100.0F, 200.0F, 100.0F, 240.0F};
    constexpr Rectangle degenerate_height{100.0F, 200.0F, 300.0F, 200.0F};

    check(
        !degenerate_width.usable() && !degenerate_height.usable(),
        "a rectangle collapsed on either axis is not usable");
    check(
        !degenerate_width.contains(100.0F, 220.0F) &&
            !degenerate_height.contains(200.0F, 200.0F),
        "and a point on the collapsed line is not a hit");

    static_assert(
        !Rectangle{}.contains(0.0F, 0.0F),
        "a zeroed layout rectangle must never claim the clamped top left "
        "cursor position");
    static_assert(
        Rectangle{10.0F, 10.0F, 20.0F, 20.0F}.contains(10.0F, 10.0F),
        "a real control still hits on its top left corner");

    if (failures == 0) {
        std::printf("menu geometry rejects rectangles the layout skipped\n");
    }
    return failures == 0 ? 0 : 1;
}
