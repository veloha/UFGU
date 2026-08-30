#include "render/JitterContract.hpp"

#include <cstdio>
#include <cstring>

namespace
{
int failures{};

void check(const bool condition, const char* const description)
{
    std::printf("  %s  %s\n", condition ? "PASS" : "FAIL", description);
    failures += condition ? 0 : 1;
}

constexpr std::uint32_t kWidth = 2560;
constexpr std::uint32_t kHeight = 1440;

constexpr float kPixelX = 0.25F;
constexpr float kPixelY = -0.125F;

struct Matrices final
{
    float jittered[16]{};
    float unjittered[16]{};
};

constexpr float kBaseProjection[16] = {
    1.35F, 0.0F,  0.0F, 0.0F,
    0.0F,  2.41F, 0.0F, 0.0F,
    0.0F,  0.0F,  0.0F, 1.0F,
    0.0F,  0.0F,  0.1F, 0.0F};

[[nodiscard]] Matrices make_pair(
    const float ndc_x, const float ndc_y, const bool row_vector)
{
    Matrices pair;
    for (int index = 0; index < 16; ++index) {
        pair.unjittered[index] = kBaseProjection[index];
        pair.jittered[index] = kBaseProjection[index];
    }
    if (row_vector) {
        pair.jittered[8] += ndc_x;
        pair.jittered[9] += ndc_y;
    } else {
        pair.jittered[2] += ndc_x;
        pair.jittered[6] += ndc_y;
    }
    return pair;
}

[[nodiscard]] mfgdlss::render::JitterContract make_contract(
    const Matrices& pair,
    const float reported_x,
    const float reported_y)
{
    using namespace mfgdlss::render;
    const auto applied = locate_applied_jitter(pair.jittered, pair.unjittered);
    JitterContract contract;
    contract.requested_pixels_x = kPixelX;
    contract.requested_pixels_y = kPixelY;

    contract.written_ndc_x = pixels_to_ndc_offset(kPixelX, kWidth);
    contract.written_ndc_y = -pixels_to_ndc_offset(kPixelY, kHeight);
    contract.applied_ndc_x = applied.ndc_x;
    contract.applied_ndc_y = applied.ndc_y;
    contract.applied_located = applied.located;
    contract.reported_pixels_x = reported_x;
    contract.reported_pixels_y = reported_y;
    contract.render_width = kWidth;
    contract.render_height = kHeight;
    return contract;
}
}

int main()
{
    using namespace mfgdlss::render;

    std::printf("pixel <-> NDC round trip\n");
    check(
        std::fabs(ndc_offset_to_pixels(
                      pixels_to_ndc_offset(kPixelX, kWidth), kWidth) -
                  kPixelX) < 1.0e-5F,
        "an X offset survives the round trip");
    check(
        std::fabs(ndc_offset_to_pixels(
                      pixels_to_ndc_offset(kPixelY, kHeight), kHeight) -
                  kPixelY) < 1.0e-5F,
        "a Y offset survives the round trip");
    check(
        ndc_offset_to_pixels(0.5F, 0U) == 0.0F &&
            pixels_to_ndc_offset(0.5F, 0U) == 0.0F,
        "a zero extent yields zero rather than dividing by it");

    std::printf("locating the applied jitter without assuming a convention\n");
    {
        const auto ndc_x = pixels_to_ndc_offset(kPixelX, kWidth);
        const auto ndc_y = pixels_to_ndc_offset(kPixelY, kHeight);

        const auto row = make_pair(ndc_x, ndc_y, true);
        const auto row_found = locate_applied_jitter(row.jittered, row.unjittered);
        check(
            row_found.located && std::fabs(row_found.ndc_x - ndc_x) < 1.0e-6F &&
                std::fabs(row_found.ndc_y - ndc_y) < 1.0e-6F,
            "a row-vector projection pair is located");

        const auto column = make_pair(ndc_x, ndc_y, false);
        const auto column_found =
            locate_applied_jitter(column.jittered, column.unjittered);
        check(
            column_found.located &&
                std::fabs(column_found.ndc_x - ndc_x) < 1.0e-6F &&
                std::fabs(column_found.ndc_y - ndc_y) < 1.0e-6F,
            "a column-vector projection pair is located");

        float same[16]{};
        const auto none = locate_applied_jitter(same, same);
        check(
            none.located && none.ndc_x == 0.0F && none.ndc_y == 0.0F,
            "identical matrices report a located zero, not a failure");

        auto ambiguous = make_pair(ndc_x, ndc_y, true);
        ambiguous.jittered[2] += ndc_x;
        const auto both = locate_applied_jitter(
            ambiguous.jittered, ambiguous.unjittered);
        check(
            !both.located,
            "a pair differing in both slots refuses to guess");
    }

    std::printf("axis convention: X carries through, Y flips\n");
    {

        const auto ndc_x = pixels_to_ndc_offset(kPixelX, kWidth);
        const auto ndc_y = pixels_to_ndc_offset(kPixelY, kHeight);
        check(
            std::fabs(applied_ndc_to_reported_pixels_x(ndc_x, kWidth) - kPixelX) <
                1.0e-6F,
            "X converts without a sign change");
        check(
            std::fabs(
                applied_ndc_to_reported_pixels_y(ndc_y, kHeight) + kPixelY) <
                1.0e-6F,
            "Y converts with a sign change");
        check(
            applied_ndc_to_reported_pixels_x(1.0F, 0U) == 0.0F &&
                applied_ndc_to_reported_pixels_y(1.0F, 0U) == 0.0F,
            "a zero extent yields zero on both axes");
    }

    std::printf("the reported value is derived from the measurement\n");
    {
        const auto ndc_x = pixels_to_ndc_offset(kPixelX, kWidth);
        const auto ndc_y = pixels_to_ndc_offset(kPixelY, kHeight);
        const auto pair = make_pair(ndc_x, ndc_y, true);
        const auto applied =
            locate_applied_jitter(pair.jittered, pair.unjittered);
        const auto reported = reported_jitter_for(applied, kWidth, kHeight);
        check(
            reported.coherent &&
                std::fabs(reported.pixels_x - kPixelX) < 1.0e-6F &&
                std::fabs(reported.pixels_y + kPixelY) < 1.0e-6F,
            "a jittered frame reports the jitter it was rendered with");

        float flat[16]{};
        for (int index = 0; index < 16; ++index) {
            flat[index] = kBaseProjection[index];
        }
        Matrices unmoved;
        std::memcpy(unmoved.jittered, flat, sizeof(flat));
        std::memcpy(unmoved.unjittered, flat, sizeof(flat));
        const auto none =
            locate_applied_jitter(unmoved.jittered, unmoved.unjittered);
        const auto zero = reported_jitter_for(none, kWidth, kHeight);
        check(
            zero.coherent && zero.pixels_x == 0.0F && zero.pixels_y == 0.0F,
            "an unjittered frame reports zero, not the requested offset");

        const auto unlocated = reported_jitter_for(
            mfgdlss::render::AppliedJitter{}, kWidth, kHeight);
        check(
            !unlocated.coherent && unlocated.pixels_x == 0.0F &&
                unlocated.pixels_y == 0.0F,
            "an unmeasurable frame reports zero and says it is not coherent");
        check(
            !reported_jitter_for(applied, 0U, kHeight).coherent,
            "a zero render extent is not coherent");
    }

    std::printf("one configured jitter contract feeds every consumer\n");
    {
        const ReportedJitter measured{0.125F, -0.25F, true};
        const auto requested =
            select_reported_jitter(true, kPixelX, kPixelY, measured);
        check(
            requested.coherent &&
                std::fabs(requested.pixels_x + kPixelX) < 1.0e-6F &&
                std::fabs(requested.pixels_y + kPixelY) < 1.0e-6F,
            "the requested policy converts the Halton axes exactly once");

        const auto selected_measured =
            select_reported_jitter(false, kPixelX, kPixelY, measured);
        check(
            selected_measured.coherent &&
                std::fabs(selected_measured.pixels_x - measured.pixels_x) <
                    1.0e-6F &&
                std::fabs(selected_measured.pixels_y - measured.pixels_y) <
                    1.0e-6F,
            "the measured policy preserves the measured provider convention");
    }

    std::printf("verdicts\n");
    {
        const auto ndc_x = pixels_to_ndc_offset(kPixelX, kWidth);
        const auto ndc_y = -pixels_to_ndc_offset(kPixelY, kHeight);
        const auto pair = make_pair(ndc_x, ndc_y, true);

        check(
            classify_jitter(make_contract(pair, kPixelX, kPixelY)) ==
                JitterVerdict::consistent,
            "matching applied and reported jitter is consistent");
        check(
            classify_jitter(make_contract(pair, -kPixelX, -kPixelY)) ==
                JitterVerdict::reported_sign_inverted_both,
            "both axes negated is reported as a both-axis sign inversion");
        check(
            classify_jitter(make_contract(pair, -kPixelX, kPixelY)) ==
                JitterVerdict::reported_sign_inverted_x,
            "a negated X is reported as an X sign inversion");
        check(
            classify_jitter(make_contract(pair, kPixelX, -kPixelY)) ==
                JitterVerdict::reported_sign_inverted_y,
            "a negated Y is reported as a Y sign inversion");

        float flat[16]{};
        for (int index = 0; index < 16; ++index) {
            flat[index] = static_cast<float>(index) * 0.5F;
        }
        Matrices unmoved;
        std::memcpy(unmoved.jittered, flat, sizeof(flat));
        std::memcpy(unmoved.unjittered, flat, sizeof(flat));
        check(
            classify_jitter(make_contract(unmoved, kPixelX, kPixelY)) ==
                JitterVerdict::engine_ignored_the_jitter,
            "an unjittered engine pair with a requested offset is caught");

        const auto half = make_pair(ndc_x * 0.5F, ndc_y * 0.5F, true);
        check(
            classify_jitter(make_contract(half, kPixelX, kPixelY)) ==
                JitterVerdict::engine_applied_a_different_jitter,
            "a halved applied jitter is reported as a different jitter");

        auto indeterminate = make_contract(pair, kPixelX, kPixelY);
        indeterminate.applied_located = false;
        check(
            classify_jitter(indeterminate) == JitterVerdict::indeterminate,
            "an unlocated jitter is indeterminate rather than consistent");

        auto zero_extent = make_contract(pair, kPixelX, kPixelY);
        zero_extent.render_width = 0U;
        check(
            classify_jitter(zero_extent) == JitterVerdict::indeterminate,
            "a zero render extent is indeterminate");

        auto patched = make_contract(unmoved, kPixelX, kPixelY);
        patched.applied_is_comparable = false;
        check(
            classify_jitter(patched) == JitterVerdict::applied_not_comparable,
            "while the fold-gate patch is active the same unjittered pair is "
            "reported as not comparable, never as the engine ignoring the "
            "jitter, because the two matrices are not a jittered and "
            "unjittered pair and the accusation would be a false alarm");

        auto patched_consistent = make_contract(pair, kPixelX, kPixelY);
        patched_consistent.applied_is_comparable = false;
        check(
            classify_jitter(patched_consistent) ==
                JitterVerdict::applied_not_comparable,
            "and it withholds the consistent verdict too, because an "
            "unmeasurable comparison cannot confirm a pass any more than it "
            "can confirm a failure");
    }

    std::printf("a genuinely stationary frame\n");
    {

        float flat[16]{};
        Matrices unmoved;
        std::memcpy(unmoved.jittered, flat, sizeof(flat));
        std::memcpy(unmoved.unjittered, flat, sizeof(flat));
        auto contract = make_contract(unmoved, 0.0F, 0.0F);
        contract.requested_pixels_x = 0.0F;
        contract.requested_pixels_y = 0.0F;
        contract.written_ndc_x = 0.0F;
        contract.written_ndc_y = 0.0F;
        check(
            classify_jitter(contract) == JitterVerdict::consistent,
            "disabled jitter everywhere is consistent, not a fault");
    }

    std::printf("JitterContractTest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
