#pragma once

#include <cmath>
#include <cstdint>

namespace mfgdlss::render
{

struct JitterContract final
{
    float requested_pixels_x{};
    float requested_pixels_y{};
    float written_ndc_x{};
    float written_ndc_y{};
    float applied_ndc_x{};
    float applied_ndc_y{};
    float reported_pixels_x{};
    float reported_pixels_y{};
    std::uint32_t render_width{};
    std::uint32_t render_height{};
    bool applied_located{};
    bool applied_is_comparable{true};
};

enum class JitterVerdict : std::uint32_t
{
    consistent,
    applied_not_comparable,
    engine_ignored_the_jitter,
    engine_applied_a_different_jitter,
    reported_sign_inverted_x,
    reported_sign_inverted_y,
    reported_sign_inverted_both,
    reported_magnitude_wrong,
    indeterminate,
};

[[nodiscard]] constexpr const char* describe(const JitterVerdict verdict) noexcept
{
    switch (verdict) {
    case JitterVerdict::consistent:
        return "the engine applied the jitter that DLSS was told about";
    case JitterVerdict::applied_not_comparable:
        return "the applied jitter cannot be measured while the fold-gate "
               "patch is active, because the two matrices compared are not a "
               "jittered and unjittered pair. This line is NOT evidence "
               "either way";
    case JitterVerdict::engine_ignored_the_jitter:
        return "THE ENGINE RENDERED WITHOUT THE JITTER DLSS IS REMOVING";
    case JitterVerdict::engine_applied_a_different_jitter:
        return "the engine applied a jitter this plugin did not request";
    case JitterVerdict::reported_sign_inverted_x:
        return "the jitter reported to DLSS has the wrong sign on X";
    case JitterVerdict::reported_sign_inverted_y:
        return "the jitter reported to DLSS has the wrong sign on Y";
    case JitterVerdict::reported_sign_inverted_both:
        return "the jitter reported to DLSS has the wrong sign on both axes";
    case JitterVerdict::reported_magnitude_wrong:
        return "the jitter reported to DLSS has the wrong magnitude";
    case JitterVerdict::indeterminate:
        return "the applied jitter could not be located in the matrices";
    }
    return "unknown";
}

[[nodiscard]] constexpr float ndc_offset_to_pixels(
    const float ndc, const std::uint32_t extent) noexcept
{
    return extent == 0U ? 0.0F : ndc * static_cast<float>(extent) * 0.5F;
}

[[nodiscard]] constexpr float pixels_to_ndc_offset(
    const float pixels, const std::uint32_t extent) noexcept
{
    return extent == 0U ? 0.0F : pixels * 2.0F / static_cast<float>(extent);
}

[[nodiscard]] constexpr float applied_ndc_to_reported_pixels_x(
    const float ndc_x, const std::uint32_t width) noexcept
{
    return ndc_offset_to_pixels(ndc_x, width);
}

[[nodiscard]] constexpr float applied_ndc_to_reported_pixels_y(
    const float ndc_y, const std::uint32_t height) noexcept
{
    return -ndc_offset_to_pixels(ndc_y, height);
}

struct AppliedJitter final
{
    float ndc_x{};
    float ndc_y{};
    bool located{};
};

[[nodiscard]] inline AppliedJitter locate_applied_jitter(
    const float* const jittered,
    const float* const unjittered,
    const float epsilon = 1.0e-6F) noexcept
{
    if (jittered == nullptr || unjittered == nullptr) {
        return AppliedJitter{};
    }

    const auto row_x = jittered[8] - unjittered[8];
    const auto row_y = jittered[9] - unjittered[9];

    const auto column_x = jittered[2] - unjittered[2];
    const auto column_y = jittered[6] - unjittered[6];

    const auto row_moved =
        std::fabs(row_x) > epsilon || std::fabs(row_y) > epsilon;
    const auto column_moved =
        std::fabs(column_x) > epsilon || std::fabs(column_y) > epsilon;

    if (row_moved && !column_moved) {
        return AppliedJitter{row_x, row_y, true};
    }
    if (column_moved && !row_moved) {
        return AppliedJitter{column_x, column_y, true};
    }

    if (!row_moved && !column_moved) {
        return AppliedJitter{0.0F, 0.0F, true};
    }

    return AppliedJitter{};
}

[[nodiscard]] inline JitterVerdict classify_jitter(
    const JitterContract& contract,
    const float tolerance_pixels = 0.02F) noexcept
{
    if (!contract.applied_located || contract.render_width == 0U ||
        contract.render_height == 0U) {
        return JitterVerdict::indeterminate;
    }
    if (!contract.applied_is_comparable) {
        return JitterVerdict::applied_not_comparable;
    }

    const auto applied_x = applied_ndc_to_reported_pixels_x(
        contract.applied_ndc_x, contract.render_width);
    const auto applied_y = applied_ndc_to_reported_pixels_y(
        contract.applied_ndc_y, contract.render_height);
    const auto requested_magnitude =
        std::fabs(contract.requested_pixels_x) +
        std::fabs(contract.requested_pixels_y);

    if (std::fabs(applied_x) + std::fabs(applied_y) <= tolerance_pixels &&
        requested_magnitude > tolerance_pixels) {
        return JitterVerdict::engine_ignored_the_jitter;
    }

    const auto matches = [tolerance_pixels](const float a, const float b) {
        return std::fabs(a - b) <= tolerance_pixels;
    };

    const auto x_ok = matches(applied_x, contract.reported_pixels_x);
    const auto y_ok = matches(applied_y, contract.reported_pixels_y);
    if (x_ok && y_ok) {
        return JitterVerdict::consistent;
    }

    const auto x_inverted = matches(applied_x, -contract.reported_pixels_x);
    const auto y_inverted = matches(applied_y, -contract.reported_pixels_y);
    if (x_inverted && y_inverted) {
        return JitterVerdict::reported_sign_inverted_both;
    }
    if (x_inverted && y_ok) {
        return JitterVerdict::reported_sign_inverted_x;
    }
    if (y_inverted && x_ok) {
        return JitterVerdict::reported_sign_inverted_y;
    }

    if (!matches(applied_x, applied_ndc_to_reported_pixels_x(
                                contract.written_ndc_x,
                                contract.render_width)) ||
        !matches(applied_y, applied_ndc_to_reported_pixels_y(
                                contract.written_ndc_y,
                                contract.render_height))) {
        return JitterVerdict::engine_applied_a_different_jitter;
    }
    return JitterVerdict::reported_magnitude_wrong;
}

struct ReportedJitter final
{
    float pixels_x{};
    float pixels_y{};
    bool coherent{};
};

[[nodiscard]] inline ReportedJitter select_reported_jitter(
    const bool use_requested,
    const float requested_pixels_x,
    const float requested_pixels_y,
    const ReportedJitter measured) noexcept
{
    if (use_requested) {
        return ReportedJitter{
            -requested_pixels_x,
            -requested_pixels_y,
            true};
    }
    return measured;
}

[[nodiscard]] inline ReportedJitter reported_jitter_for(
    const AppliedJitter& applied,
    const std::uint32_t render_width,
    const std::uint32_t render_height) noexcept
{
    if (!applied.located || render_width == 0U || render_height == 0U) {

        return ReportedJitter{0.0F, 0.0F, false};
    }
    return ReportedJitter{
        applied_ndc_to_reported_pixels_x(applied.ndc_x, render_width),
        applied_ndc_to_reported_pixels_y(applied.ndc_y, render_height),
        true};
}
}
