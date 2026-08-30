#include "render/MenuBarLayout.hpp"

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

static_assert(
    bar_scale_is_valid(1.0F) && bar_scale_is_valid(3.0F),
    "the scale range the menu clamps to is accepted here too");
static_assert(
    !bar_scale_is_valid(0.5F) && !bar_scale_is_valid(8.0F),
    "a scale outside the menu's own clamp is refused rather than drawn");

static_assert(
    compute_bar_rect(3840.0F, 3.0F).left == 0.0F &&
        compute_bar_rect(3840.0F, 3.0F).right == 3840.0F,
    "the bar spans the full width, because a partial strip would leave the "
    "game visible behind a control surface and invite a misclick");

static_assert(
    compute_bar_rect(3840.0F, 2.0F).bottom == kBarHeightUnits * 2.0F,
    "bar height scales linearly, so a 4K panel gets a proportionate bar "
    "rather than a hairline");

static_assert(
    compute_bar_dropdown_rect(1920.0F, 1.0F, 4U).top ==
        compute_bar_rect(1920.0F, 1.0F).bottom,
    "the dropdown starts exactly where the bar ends, with no seam and no "
    "overlap");

static_assert(
    compute_bar_dropdown_height(1.0F, 0U) == 0.0F,
    "a section with no rows opens to nothing rather than to an empty box");

static_assert(
    compute_bar_dropdown_height(1.0F, 12U) ==
        compute_bar_dropdown_height(1.0F, kBarMaximumDropdownRows),
    "more rows than the cap do not grow the dropdown without bound");

[[nodiscard]] constexpr bool row_inside_dropdown(
    const std::size_t rows,
    const std::size_t index) noexcept
{
    const auto panel = compute_bar_dropdown_rect(1920.0F, 1.0F, rows);
    const auto row = compute_bar_dropdown_row(1920.0F, 1.0F, index);
    return row.top >= panel.top && row.bottom <= panel.bottom &&
        row.left >= panel.left && row.right <= panel.right;
}

static_assert(
    row_inside_dropdown(4U, 0U) && row_inside_dropdown(4U, 3U),
    "the first and last rows of a section both sit inside the panel that "
    "draws them");

[[nodiscard]] constexpr bool rows_are_disjoint(
    const std::size_t first,
    const std::size_t second) noexcept
{
    const auto a = compute_bar_dropdown_row(1920.0F, 1.0F, first);
    const auto b = compute_bar_dropdown_row(1920.0F, 1.0F, second);
    return a.bottom <= b.top;
}

static_assert(
    rows_are_disjoint(0U, 1U) && rows_are_disjoint(2U, 3U),
    "consecutive rows never overlap, so a hit test cannot match two settings "
    "at once");

[[nodiscard]] constexpr bool panel_fits_screen(
    const PanelAnchor anchor) noexcept
{
    const auto panel = compute_panel_rect(anchor, 3840.0F, 2160.0F, 3.0F, 4U);
    return panel.left >= 0.0F && panel.top >= 0.0F &&
        panel.right <= 3840.0F && panel.bottom <= 2160.0F &&
        panel.right > panel.left && panel.bottom > panel.top;
}

static_assert(
    panel_fits_screen(PanelAnchor::top) &&
        panel_fits_screen(PanelAnchor::bottom) &&
        panel_fits_screen(PanelAnchor::left) &&
        panel_fits_screen(PanelAnchor::centre) &&
        panel_fits_screen(PanelAnchor::corner),
    "every anchored layout stays on screen at 4K, so no design can push its "
    "own controls out of reach");

[[nodiscard]] constexpr bool rows_sit_in_panel(
    const PanelAnchor anchor) noexcept
{
    const auto panel = compute_panel_rect(anchor, 3840.0F, 2160.0F, 3.0F, 4U);
    const auto first =
        compute_panel_row(anchor, 3840.0F, 2160.0F, 3.0F, 4U, 0U);
    const auto last =
        compute_panel_row(anchor, 3840.0F, 2160.0F, 3.0F, 4U, 3U);
    return rect_within(first, panel) && rect_within(last, panel);
}

static_assert(
    rows_sit_in_panel(PanelAnchor::top) &&
        rows_sit_in_panel(PanelAnchor::bottom) &&
        rows_sit_in_panel(PanelAnchor::left) &&
        rows_sit_in_panel(PanelAnchor::centre) &&
        rows_sit_in_panel(PanelAnchor::corner),
    "the first and last setting of a section are inside the panel in EVERY "
    "layout, which is the invariant that stops a redesign from clipping a "
    "control it still accepts input for");

static_assert(
    compute_panel_rect(PanelAnchor::bottom, 3840.0F, 2160.0F, 3.0F, 4U)
            .bottom == 2160.0F,
    "the bottom dock is flush with the lower edge rather than floating");

static_assert(
    compute_panel_rect(PanelAnchor::corner, 3840.0F, 2160.0F, 3.0F, 4U)
            .right < 3840.0F,
    "the corner HUD is inset from the edge, which is what distinguishes it "
    "from a dock");

static_assert(
    compute_panel_rect(PanelAnchor::left, 3840.0F, 2160.0F, 3.0F, 4U)
            .bottom == 2160.0F,
    "the drawer is full height, because a short left panel reads as a stray "
    "box rather than an edge");

[[nodiscard]] constexpr bool columns_survive(const PanelAnchor anchor) noexcept
{
    const auto row =
        compute_panel_row(anchor, 3840.0F, 2160.0F, 3.0F, 4U, 0U);
    return row_columns_do_not_collide(row, 3.0F);
}

static_assert(
    columns_survive(PanelAnchor::top) && columns_survive(PanelAnchor::bottom) &&
        columns_survive(PanelAnchor::left) &&
        columns_survive(PanelAnchor::centre) &&
        columns_survive(PanelAnchor::corner),
    "the label never runs into the value in ANY layout. The Left Drawer "
    "shipped broken at 226 with the label overlapping its own value, because "
    "the earlier assertions only checked that a row sat inside its panel and "
    "never that the row's own columns fit inside the row");

static_assert(
    menu_should_auto_close(false, 60.0F),
    "an untouched menu left open does close on its own, which is why the "
    "timeout exists");

static_assert(
    !menu_should_auto_close(true, 600.0F),
    "but a menu with UNAPPLIED changes never closes itself, no matter how "
    "long it is left. The timeout used to discard pending edits silently, so "
    "reading the detail panel for a minute lost the setting being changed");

static_assert(
    !menu_should_auto_close(false, kMenuIdleCloseSeconds),
    "the timeout is exclusive at the boundary rather than firing exactly on "
    "it");
}



int main()
{
    std::printf("=== Menu bar layout contract ===\n\n");

    const auto bar = compute_bar_rect(3840.0F, 3.0F);
    check(
        bar_contains(bar, 1920.0F, 20.0F),
        "a point inside the bar is reported inside it");
    check(
        !bar_contains(bar, 1920.0F, bar.bottom + 1.0F),
        "a point below the bar is outside it, so gameplay clicks are not "
        "swallowed by a collapsed menu");
    check(
        !bar_contains(bar, 1920.0F, bar.bottom),
        "the bottom edge is exclusive, which is what stops the bar and the "
        "dropdown from both claiming the same row of pixels");

    check(
        bar_occludes_less_than(3.0F, 2160.0F, 4U, 0.25F),
        "a collapsed menu with a section open covers less than a quarter of a "
        "4K screen, which is the entire point of the mode");
    check(
        bar_occludes_less_than(3.0F, 2160.0F, 0U, 0.08F),
        "with nothing open the bar alone covers under 8 percent, so the game "
        "stays readable behind it");

    check(
        compute_bar_dropdown_row(1920.0F, 1.0F, 0U).left >
            compute_bar_dropdown_rect(1920.0F, 1.0F, 1U).left,
        "rows are inset from the panel edge rather than running edge to edge");

    check(
        compute_bar_height(2.0F) > compute_bar_height(1.0F),
        "a larger scale yields a larger bar");

    const auto card =
        compute_panel_rect(PanelAnchor::centre, 3840.0F, 2160.0F, 3.0F, 4U);
    check(
        card.left > 0.0F && card.right < 3840.0F,
        "the centre card floats rather than spanning the screen, so it reads "
        "as a dialogue and not a takeover");
    check(
        std::fabs((card.left) - (3840.0F - card.right)) < 1.0F,
        "and it is centred, with equal margin either side");

    std::printf(
        "\n=== MenuBarLayoutTest: %d passed, %d failed ===\n",
        g_passed,
        g_failed);
    return g_failed == 0 ? 0 : 1;
}
