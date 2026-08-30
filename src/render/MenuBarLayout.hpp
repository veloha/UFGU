#pragma once

#include <cstddef>

namespace mfgdlss::render
{
struct BarRect final
{
    float left{};
    float top{};
    float right{};
    float bottom{};
};

enum class PanelAnchor
{
    top,
    bottom,
    left,
    centre,
    corner
};

inline constexpr float kBarHeightUnits = 42.0F;
inline constexpr float kBarSidePaddingUnits = 22.0F;
inline constexpr float kBarDropdownRowUnits = 30.0F;
inline constexpr float kBarDropdownPaddingUnits = 8.0F;
inline constexpr float kBarMinimumScale = 0.9F;
inline constexpr float kBarMaximumScale = 4.0F;
inline constexpr std::size_t kBarMaximumDropdownRows = 8U;

inline constexpr float kDrawerWidthUnits = 360.0F;
inline constexpr float kCardWidthUnits = 560.0F;
inline constexpr float kCornerWidthUnits = 420.0F;
inline constexpr float kEdgeMarginUnits = 26.0F;

[[nodiscard]] constexpr bool bar_scale_is_valid(const float scale) noexcept
{
    return scale >= kBarMinimumScale && scale <= kBarMaximumScale;
}

[[nodiscard]] constexpr float compute_bar_height(const float scale) noexcept
{
    return kBarHeightUnits * scale;
}

[[nodiscard]] constexpr BarRect compute_bar_rect(
    const float width,
    const float scale) noexcept
{
    return BarRect{0.0F, 0.0F, width, compute_bar_height(scale)};
}

[[nodiscard]] constexpr std::size_t capped_rows(
    const std::size_t rows) noexcept
{
    return rows > kBarMaximumDropdownRows ? kBarMaximumDropdownRows : rows;
}

[[nodiscard]] constexpr float compute_bar_dropdown_height(
    const float scale,
    const std::size_t rows) noexcept
{
    if (rows == 0U) {
        return 0.0F;
    }
    return (static_cast<float>(capped_rows(rows)) * kBarDropdownRowUnits +
            2.0F * kBarDropdownPaddingUnits) *
        scale;
}

[[nodiscard]] constexpr BarRect compute_bar_dropdown_rect(
    const float width,
    const float scale,
    const std::size_t rows) noexcept
{
    const auto top = compute_bar_height(scale);
    return BarRect{
        0.0F, top, width, top + compute_bar_dropdown_height(scale, rows)};
}

[[nodiscard]] constexpr BarRect compute_bar_dropdown_row(
    const float width,
    const float scale,
    const std::size_t index) noexcept
{
    const auto top = compute_bar_height(scale) +
        kBarDropdownPaddingUnits * scale +
        static_cast<float>(index) * kBarDropdownRowUnits * scale;
    return BarRect{
        kBarSidePaddingUnits * scale,
        top,
        width - kBarSidePaddingUnits * scale,
        top + kBarDropdownRowUnits * scale};
}

[[nodiscard]] constexpr float compute_panel_width(
    const PanelAnchor anchor,
    const float screen_width,
    const float scale) noexcept
{
    switch (anchor) {
    case PanelAnchor::left:
        return kDrawerWidthUnits * scale;
    case PanelAnchor::centre:
        return kCardWidthUnits * scale;
    case PanelAnchor::corner:
        return kCornerWidthUnits * scale;
    case PanelAnchor::top:
    case PanelAnchor::bottom:
        break;
    }
    return screen_width;
}

[[nodiscard]] constexpr float compute_panel_height(
    const PanelAnchor anchor,
    const float screen_height,
    const float scale,
    const std::size_t rows) noexcept
{
    if (anchor == PanelAnchor::left) {
        return screen_height;
    }
    const auto content = compute_bar_height(scale) +
        compute_bar_dropdown_height(scale, rows);
    return content > screen_height ? screen_height : content;
}

[[nodiscard]] constexpr BarRect compute_panel_rect(
    const PanelAnchor anchor,
    const float screen_width,
    const float screen_height,
    const float scale,
    const std::size_t rows) noexcept
{
    const auto width = compute_panel_width(anchor, screen_width, scale);
    const auto height =
        compute_panel_height(anchor, screen_height, scale, rows);
    const auto margin = kEdgeMarginUnits * scale;
    switch (anchor) {
    case PanelAnchor::top:
        return BarRect{0.0F, 0.0F, screen_width, height};
    case PanelAnchor::bottom:
        return BarRect{
            0.0F, screen_height - height, screen_width, screen_height};
    case PanelAnchor::left:
        return BarRect{0.0F, 0.0F, width, screen_height};
    case PanelAnchor::centre: {
        const auto left = (screen_width - width) * 0.5F;
        const auto top = (screen_height - height) * 0.5F;
        return BarRect{left, top, left + width, top + height};
    }
    case PanelAnchor::corner:
        return BarRect{
            screen_width - margin - width,
            screen_height - margin - height,
            screen_width - margin,
            screen_height - margin};
    }
    return BarRect{0.0F, 0.0F, screen_width, height};
}

[[nodiscard]] constexpr BarRect compute_panel_row(
    const PanelAnchor anchor,
    const float screen_width,
    const float screen_height,
    const float scale,
    const std::size_t rows,
    const std::size_t index) noexcept
{
    const auto panel =
        compute_panel_rect(anchor, screen_width, screen_height, scale, rows);
    const auto inset = kBarSidePaddingUnits * scale;
    const auto top = panel.top + compute_bar_height(scale) +
        kBarDropdownPaddingUnits * scale +
        static_cast<float>(index) * kBarDropdownRowUnits * scale;
    return BarRect{
        panel.left + inset,
        top,
        panel.right - inset,
        top + kBarDropdownRowUnits * scale};
}

inline constexpr float kRowArrowUnits = 22.0F;
inline constexpr float kRowValuePreferredUnits = 176.0F;
inline constexpr float kRowValueMaximumFraction = 0.42F;
inline constexpr float kRowInnerPaddingUnits = 16.0F;

struct RowColumns final
{
    float label_left{};
    float label_right{};
    float left_arrow_left{};
    float value_left{};
    float value_right{};
    float right_arrow_right{};
};

[[nodiscard]] constexpr RowColumns compute_row_columns(
    const BarRect& row,
    const float scale) noexcept
{
    const auto usable = row.right - row.left - kRowInnerPaddingUnits * scale;
    const auto arrow = kRowArrowUnits * scale;
    auto value = kRowValuePreferredUnits * scale;
    const auto budget = usable * kRowValueMaximumFraction;
    if (value > budget) {
        value = budget;
    }
    if (value < 0.0F) {
        value = 0.0F;
    }
    const auto content_right = row.right - kRowInnerPaddingUnits * scale;
    RowColumns columns{};
    columns.right_arrow_right = content_right;
    columns.value_right = content_right - arrow;
    columns.value_left = columns.value_right - value;
    columns.left_arrow_left = columns.value_left - arrow;
    columns.label_left = row.left;
    columns.label_right = columns.left_arrow_left;
    return columns;
}

[[nodiscard]] constexpr bool row_columns_do_not_collide(
    const BarRect& row,
    const float scale) noexcept
{
    const auto columns = compute_row_columns(row, scale);
    return columns.label_right > columns.label_left &&
        columns.label_right <= columns.left_arrow_left &&
        columns.value_left < columns.value_right &&
        columns.value_right <= columns.right_arrow_right;
}

inline constexpr float kMenuIdleCloseSeconds = 45.0F;

[[nodiscard]] constexpr bool menu_should_auto_close(
    const bool has_unapplied_changes,
    const float idle_seconds) noexcept
{
    return !has_unapplied_changes && idle_seconds > kMenuIdleCloseSeconds;
}

[[nodiscard]] constexpr bool bar_contains(
    const BarRect& rectangle,
    const float x,
    const float y) noexcept
{
    return x >= rectangle.left && x < rectangle.right && y >= rectangle.top &&
        y < rectangle.bottom;
}

[[nodiscard]] constexpr bool rect_within(
    const BarRect& inner,
    const BarRect& outer) noexcept
{
    return inner.left >= outer.left && inner.right <= outer.right &&
        inner.top >= outer.top && inner.bottom <= outer.bottom;
}

[[nodiscard]] constexpr bool bar_occludes_less_than(
    const float scale,
    const float screen_height,
    const std::size_t rows,
    const float fraction) noexcept
{
    if (screen_height <= 0.0F) {
        return false;
    }
    const auto used =
        compute_bar_height(scale) + compute_bar_dropdown_height(scale, rows);
    return used <= screen_height * fraction;
}
}
