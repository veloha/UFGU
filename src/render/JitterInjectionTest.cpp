

#include "render/JitterContract.hpp"
#include "render/JitterInjection.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace
{
using namespace mfgdlss::render;

int failures = 0;
int checks = 0;

void check(const bool condition, const std::string_view what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL  %.*s\n",
            static_cast<int>(what.size()), what.data());
        return;
    }
    std::printf("PASS  %.*s\n",
        static_cast<int>(what.size()), what.data());
}

void near(
    const float actual,
    const float expected,
    const std::string_view what,
    const float tolerance = 1.0e-5F)
{
    check(std::fabs(actual - expected) <= tolerance, what);
}

using Matrix = std::array<float, 16>;

constexpr float kNear = 15.0F;
constexpr float kFar = 353840.0F;

[[nodiscard]] Matrix row_vector_projection()
{
    const auto m33 = kFar / (kFar - kNear);
    Matrix m{};
    m[mat_index(0U, 0U)] = 1.2071F;
    m[mat_index(1U, 1U)] = 2.1445F;
    m[mat_index(2U, 2U)] = m33;
    m[mat_index(2U, 3U)] = 1.0F;
    m[mat_index(3U, 2U)] = -kNear * m33;
    m[mat_index(3U, 3U)] = 0.0F;
    return m;
}

[[nodiscard]] Matrix transposed(const Matrix& source)
{
    Matrix result{};
    for (std::size_t row = 0U; row < 4U; ++row) {
        for (std::size_t column = 0U; column < 4U; ++column) {
            result[mat_index(row, column)] = source[mat_index(column, row)];
        }
    }
    return result;
}

[[nodiscard]] Matrix multiply(const Matrix& a, const Matrix& b)
{
    Matrix result{};
    for (std::size_t row = 0U; row < 4U; ++row) {
        for (std::size_t column = 0U; column < 4U; ++column) {
            float sum = 0.0F;
            for (std::size_t k = 0U; k < 4U; ++k) {
                sum += a[mat_index(row, k)] * b[mat_index(k, column)];
            }
            result[mat_index(row, column)] = sum;
        }
    }
    return result;
}

[[nodiscard]] Matrix row_vector_view()
{
    Matrix m{};
    m[mat_index(0U, 0U)] = 0.8660F;
    m[mat_index(0U, 2U)] = -0.5000F;
    m[mat_index(1U, 1U)] = 1.0000F;
    m[mat_index(2U, 0U)] = 0.5000F;
    m[mat_index(2U, 2U)] = 0.8660F;
    m[mat_index(3U, 0U)] = 120.0F;
    m[mat_index(3U, 1U)] = -40.0F;
    m[mat_index(3U, 2U)] = 900.0F;
    m[mat_index(3U, 3U)] = 1.0F;
    return m;
}

void project(
    const Matrix& m,
    const ProjectionLayout layout,
    const float x,
    const float y,
    const float z,
    float& ndc_x,
    float& ndc_y)
{
    const std::array<float, 4> v{x, y, z, 1.0F};
    std::array<float, 4> clip{};
    for (std::size_t i = 0U; i < 4U; ++i) {
        float sum = 0.0F;
        for (std::size_t k = 0U; k < 4U; ++k) {
            sum += layout == ProjectionLayout::row_vector ?
                v[k] * m[mat_index(k, i)] :
                m[mat_index(i, k)] * v[k];
        }
        clip[i] = sum;
    }
    ndc_x = clip[0] / clip[3];
    ndc_y = clip[1] / clip[3];
}

void test_layout_is_measured_not_assumed()
{
    const auto row = row_vector_projection();
    const auto column = transposed(row);

    check(
        classify_projection_layout(row.data()) ==
            ProjectionLayout::row_vector,
        "Skyrim's measured projection classifies as row-vector");
    check(
        classify_projection_layout(column.data()) ==
            ProjectionLayout::column_vector,
        "its transpose classifies as column-vector");

    Matrix orthographic{};
    orthographic[mat_index(0U, 0U)] = 0.001F;
    orthographic[mat_index(1U, 1U)] = 0.001F;
    orthographic[mat_index(2U, 2U)] = 0.0001F;
    orthographic[mat_index(3U, 3U)] = 1.0F;
    check(
        classify_projection_layout(orthographic.data()) ==
            ProjectionLayout::indeterminate,
        "an orthographic matrix is refused by the layout test");

    check(
        classify_projection_layout(nullptr) ==
            ProjectionLayout::indeterminate,
        "a null matrix is refused rather than dereferenced");

    Matrix inverse{};
    inverse[mat_index(2U, 2U)] = 0.0F;
    inverse[mat_index(2U, 3U)] = 1.0F;
    inverse[mat_index(3U, 2U)] = -0.066664F;
    inverse[mat_index(3U, 3U)] = 0.066667F;
    check(
        classify_projection_layout(inverse.data()) ==
            ProjectionLayout::indeterminate,
        "the measured inverse-projection block is refused");
}

void test_offset_reaches_ndc_at_every_depth()
{
    auto row = row_vector_projection();
    const auto layout = classify_projection_layout(row.data());
    constexpr float kOffsetX = 0.000098F;
    constexpr float kOffsetY = -0.000386F;

    float before_near_x = 0.0F;
    float before_near_y = 0.0F;
    float before_far_x = 0.0F;
    float before_far_y = 0.0F;
    project(row, layout, 3.0F, -2.0F, 20.0F,
        before_near_x, before_near_y);
    project(row, layout, 3.0F, -2.0F, 50000.0F,
        before_far_x, before_far_y);

    apply_clip_offset(row.data(), layout, kOffsetX, kOffsetY);

    float after_near_x = 0.0F;
    float after_near_y = 0.0F;
    float after_far_x = 0.0F;
    float after_far_y = 0.0F;
    project(row, layout, 3.0F, -2.0F, 20.0F, after_near_x, after_near_y);
    project(row, layout, 3.0F, -2.0F, 50000.0F, after_far_x, after_far_y);

    near(after_near_x - before_near_x, kOffsetX,
        "the NDC x offset is exact at a near point", 1.0e-7F);
    near(after_near_y - before_near_y, kOffsetY,
        "the NDC y offset is exact at a near point", 1.0e-7F);
    near(after_far_x - before_far_x, kOffsetX,
        "the same offset survives to 50000 units", 1.0e-7F);
    near(after_far_y - before_far_y, kOffsetY,
        "the same y offset survives to 50000 units", 1.0e-7F);

    const auto pristine = row_vector_projection();
    near(row[mat_index(2U, 2U)], pristine[mat_index(2U, 2U)],
        "m33 is unchanged by the jitter", 1.0e-9F);
    near(row[mat_index(3U, 2U)], pristine[mat_index(3U, 2U)],
        "m43 is unchanged by the jitter", 1.0e-9F);
}

void test_column_vector_offset_matches_row_vector()
{
    constexpr float kOffsetX = 0.000098F;
    constexpr float kOffsetY = -0.000386F;
    auto row = row_vector_projection();
    auto column = transposed(row);

    apply_clip_offset(row.data(), ProjectionLayout::row_vector,
        kOffsetX, kOffsetY);
    apply_clip_offset(column.data(), ProjectionLayout::column_vector,
        kOffsetX, kOffsetY);

    const auto column_as_row = transposed(column);
    bool identical = true;
    for (std::size_t i = 0U; i < 16U; ++i) {
        identical = identical &&
            std::fabs(row[i] - column_as_row[i]) <= 1.0e-9F;
    }
    check(identical,
        "both conventions produce the same jittered projection");
}

void test_view_projection_is_only_touched_when_it_verifies()
{
    const auto view = row_vector_view();
    const auto projection = row_vector_projection();
    const auto view_projection = multiply(view, projection);

    check(
        verify_matrix_product(
            view_projection.data(), view.data(), projection.data(),
            ProjectionLayout::row_vector),
        "the engine's own view * projection is recognised");

    auto unrelated = view_projection;
    unrelated[mat_index(1U, 2U)] += 5.0F;
    check(
        !verify_matrix_product(
            unrelated.data(), view.data(), projection.data(),
            ProjectionLayout::row_vector),
        "a matrix that is not the product is rejected");

    auto jittered_projection = projection;
    auto jittered_view_projection = view_projection;
    apply_clip_offset(jittered_projection.data(),
        ProjectionLayout::row_vector, 0.000098F, -0.000386F);
    apply_clip_offset(jittered_view_projection.data(),
        ProjectionLayout::row_vector, 0.000098F, -0.000386F);
    const auto recomposed = multiply(view, jittered_projection);
    bool composes = true;
    for (std::size_t i = 0U; i < 16U; ++i) {
        composes = composes &&
            std::fabs(recomposed[i] - jittered_view_projection[i]) <= 1.0e-3F;
    }
    check(composes,
        "jittering the view-projection equals jittering the projection first");
}

void test_injection_refuses_what_it_cannot_identify()
{
    auto projection = row_vector_projection();
    const auto view = row_vector_view();
    auto view_projection = multiply(view, projection);

    const auto ok = inject_raster_jitter(
        projection.data(), view_projection.data(), view.data(),
        kNear, kFar, 0.000098F, -0.000386F);
    check(
        ok.verdict ==
            InjectionVerdict::injected_projection_and_view_projection,
        "a verified pair is injected into both matrices");
    check(injected(ok.verdict), "and reports as injected");

    auto cascade = row_vector_projection();
    const auto refused_camera = inject_raster_jitter(
        cascade.data(), nullptr, nullptr,
        4000.0F, kFar, 0.000098F, -0.000386F);
    check(
        refused_camera.verdict ==
            InjectionVerdict::refused_not_the_camera_projection,
        "a projection whose near plane is not the camera's is refused");
    check(!injected(refused_camera.verdict), "and nothing is written");

    Matrix orthographic{};
    orthographic[mat_index(0U, 0U)] = 0.001F;
    orthographic[mat_index(1U, 1U)] = 0.001F;
    orthographic[mat_index(3U, 3U)] = 1.0F;
    const auto pristine_ortho = orthographic;
    const auto refused_layout = inject_raster_jitter(
        orthographic.data(), nullptr, nullptr,
        kNear, kFar, 0.000098F, -0.000386F);
    check(
        refused_layout.verdict ==
            InjectionVerdict::refused_layout_indeterminate,
        "an orthographic matrix is refused by the injector");
    bool untouched = true;
    for (std::size_t i = 0U; i < 16U; ++i) {
        untouched = untouched &&
            orthographic[i] == pristine_ortho[i];
    }
    check(untouched, "a refused matrix is left byte-for-byte alone");

    auto zero_case = row_vector_projection();
    const auto pristine_zero = zero_case;
    const auto skipped = inject_raster_jitter(
        zero_case.data(), nullptr, nullptr, kNear, kFar, 0.0F, 0.0F);
    check(
        skipped.verdict == InjectionVerdict::skipped_zero_offset,
        "a zero offset is skipped rather than written");
    bool zero_untouched = true;
    for (std::size_t i = 0U; i < 16U; ++i) {
        zero_untouched = zero_untouched &&
            zero_case[i] == pristine_zero[i];
    }
    check(zero_untouched, "and leaves the matrix untouched");

    const auto null_case = inject_raster_jitter(
        nullptr, nullptr, nullptr, kNear, kFar, 0.000098F, -0.000386F);
    check(
        null_case.verdict == InjectionVerdict::refused_buffer_unavailable,
        "a null buffer is refused rather than dereferenced");

    auto nan_case = row_vector_projection();
    const auto nan_result = inject_raster_jitter(
        nan_case.data(), nullptr, nullptr, kNear, kFar,
        std::nanf(""), 0.0F);
    check(
        !injected(nan_result.verdict),
        "a non-finite offset is refused");
}

void test_the_whole_chain_agrees()
{
    constexpr std::uint32_t kWidth = 2560U;
    constexpr std::uint32_t kHeight = 1440U;

    constexpr float kHaltonX = -0.3750F;
    constexpr float kHaltonY = -0.0556F;

    const auto ndc_x = -pixels_to_ndc_offset(kHaltonX, kWidth);
    const auto ndc_y = pixels_to_ndc_offset(kHaltonY, kHeight);
    near(ndc_x, 0.000293F, "the logged written NDC x is reproduced", 1.0e-6F);
    near(ndc_y, -0.000077F, "the logged written NDC y is reproduced", 1.0e-6F);

    const auto pristine = row_vector_projection();
    auto patched = pristine;
    const auto result = inject_raster_jitter(
        patched.data(), nullptr, nullptr, kNear, kFar, ndc_x, ndc_y);
    check(injected(result.verdict), "the offset is injected");

    const auto relocated = locate_applied_jitter(
        patched.data(), pristine.data());
    check(relocated.located, "the injected offset is independently located");
    near(relocated.ndc_x, ndc_x,
        "and its x matches what was injected", 1.0e-7F);
    near(relocated.ndc_y, ndc_y,
        "and its y matches what was injected", 1.0e-7F);

    const auto reported = reported_jitter_for(relocated, kWidth, kHeight);
    check(reported.coherent, "the reported jitter is coherent");

    near(reported.pixels_x, -kHaltonX,
        "the reported x equals the negated Halton x", 1.0e-4F);
    near(reported.pixels_y, -kHaltonY,
        "the reported y equals the negated Halton y", 1.0e-4F);

    JitterContract contract{};
    contract.requested_pixels_x = kHaltonX;
    contract.requested_pixels_y = kHaltonY;
    contract.written_ndc_x = ndc_x;
    contract.written_ndc_y = ndc_y;
    contract.applied_ndc_x = relocated.ndc_x;
    contract.applied_ndc_y = relocated.ndc_y;
    contract.reported_pixels_x = reported.pixels_x;
    contract.reported_pixels_y = reported.pixels_y;
    contract.render_width = kWidth;
    contract.render_height = kHeight;
    contract.applied_located = true;
    check(
        classify_jitter(contract) == JitterVerdict::consistent,
        "THE RASTER AND THE UPSCALER NOW AGREE: the contract is consistent");

    JitterContract broken = contract;
    broken.applied_ndc_x = 0.0F;
    broken.applied_ndc_y = 0.0F;
    broken.reported_pixels_x = -kHaltonX;
    broken.reported_pixels_y = -kHaltonY;
    check(
        classify_jitter(broken) ==
            JitterVerdict::engine_ignored_the_jitter,
        "and an unjittered raster is still detected as the failure it was");
}

void test_depth_contract_travels_as_one_value()
{
    constexpr DepthContractSnapshot standard_finite{
        DepthOrientation::standard, DepthRange::finite};
    check(standard_finite.determinate(),
        "Skyrim's measured contract is determinate");
    check(!standard_finite.reversed(),
        "and is not reversed -- the value the first FSR context got wrong");
    check(!standard_finite.infinite(),
        "and has a finite far plane");

    constexpr DepthContractSnapshot half_measured{
        DepthOrientation::standard, DepthRange::indeterminate};
    check(!half_measured.determinate(),
        "a half-measured contract is refused, not silently defaulted");

    constexpr DepthContractSnapshot unmeasured{};
    check(!unmeasured.determinate(),
        "an unmeasured contract is refused");
    check(!unmeasured.reversed(),
        "and never claims reversed depth, which is how the 00:58:20 context "
        "was created DEPTH_INVERTED against a standard-Z projection");
}
}

int main()
{
    test_layout_is_measured_not_assumed();
    test_offset_reaches_ndc_at_every_depth();
    test_column_vector_offset_matches_row_vector();
    test_view_projection_is_only_touched_when_it_verifies();
    test_injection_refuses_what_it_cannot_identify();
    test_the_whole_chain_agrees();
    test_depth_contract_travels_as_one_value();

    {
        using mfgdlss::render::detail::near_unit;
        using mfgdlss::render::detail::near_zero;

        check(near_zero(0.0F), "zero is near zero");
        check(near_zero(1.0e-6F), "a tiny positive value is near zero");
        check(near_zero(-1.0e-6F), "a tiny negative value is near zero");
        check(near_zero(1.0e-5F), "the default epsilon is inclusive");
        check(!near_zero(2.0e-5F), "twice the epsilon is not near zero");
        check(!near_zero(-2.0e-5F), "sign does not rescue a large value");
        check(near_zero(0.5F, 1.0F), "a caller-supplied epsilon is honoured");

        check(near_unit(1.0F), "one is near unit");
        check(
            near_unit(-1.0F),
            "near_unit compares magnitude, so minus one is near unit");
        check(near_unit(0.9995F), "just under one is near unit");
        check(near_unit(1.0005F), "just over one is near unit");
        check(!near_unit(1.01F), "a percent off is not near unit");
        check(!near_unit(0.0F), "zero is not near unit");
        check(near_unit(1.5F, 1.0F), "a caller-supplied epsilon is honoured");
    }

    {
        DepthProjection block{};
        const auto row = row_vector_projection();
        check(
            !extract_depth_block(
                nullptr, ProjectionLayout::row_vector, kNear, kFar, block),
            "a null matrix yields no depth block");
        check(
            !extract_depth_block(
                row.data(), ProjectionLayout::indeterminate, kNear, kFar,
                block),
            "an indeterminate layout yields no depth block");

        DepthProjection as_row{};
        check(
            extract_depth_block(
                row.data(), ProjectionLayout::row_vector, kNear, kFar, as_row),
            "a row-vector matrix yields a depth block");
        near(as_row.near_plane, kNear, "the near plane is carried through");
        near(as_row.far_plane, kFar, "the far plane is carried through");
        near(
            as_row.m33, row[mat_index(2U, 2U)],
            "m33 comes from row 2 column 2");
        near(
            as_row.m34, row[mat_index(2U, 3U)],
            "row-vector m34 comes from row 2 column 3");
        near(
            as_row.m43, row[mat_index(3U, 2U)],
            "row-vector m43 comes from row 3 column 2");

        DepthProjection as_column{};
        check(
            extract_depth_block(
                row.data(), ProjectionLayout::column_vector, kNear, kFar,
                as_column),
            "the same memory reads as a column-vector depth block");
        near(
            as_column.m34, as_row.m43,
            "a column-vector layout swaps m34 with m43");
        near(
            as_column.m43, as_row.m34,
            "a column-vector layout swaps m43 with m34");
        near(
            as_column.m33, as_row.m33,
            "m33 sits on the diagonal and does not swap");
    }

    {
        const auto row = row_vector_projection();
        check(
            projection_matches_camera(
                row.data(), ProjectionLayout::row_vector, kNear, kFar),
            "Skyrim's own projection matches its camera planes");
        check(
            !projection_matches_camera(
                nullptr, ProjectionLayout::row_vector, kNear, kFar),
            "a null matrix matches nothing");
        check(
            !projection_matches_camera(
                row.data(), ProjectionLayout::indeterminate, kNear, kFar),
            "an indeterminate layout matches nothing");
        check(
            !projection_matches_camera(
                row.data(), ProjectionLayout::row_vector, 0.0F, kFar),
            "a non-positive near plane is refused");
        check(
            !projection_matches_camera(
                row.data(), ProjectionLayout::row_vector, kNear, kNear),
            "a far plane at the near plane is refused");
        check(
            !projection_matches_camera(
                row.data(), ProjectionLayout::row_vector, kFar, kNear),
            "an inverted depth range is refused");
        check(
            !projection_matches_camera(
                row.data(), ProjectionLayout::row_vector, kNear * 100.0F,
                kFar),
            "a near plane the matrix was not built for is refused");
    }

    {
        constexpr InjectionVerdict all[]{
            InjectionVerdict::injected_projection_and_view_projection,
            InjectionVerdict::injected_projection_only,
            InjectionVerdict::skipped_zero_offset,
            InjectionVerdict::refused_layout_indeterminate,
            InjectionVerdict::refused_not_the_camera_projection,
            InjectionVerdict::refused_buffer_unavailable};
        bool described = true;
        for (const auto verdict : all) {
            const auto* const text = describe(verdict);
            described = described && text != nullptr && text[0] != '\0';
        }
        check(described, "every injection verdict has a description");
        check(
            injected(
                InjectionVerdict::injected_projection_and_view_projection) &&
                injected(InjectionVerdict::injected_projection_only),
            "both injecting verdicts report as injected");
        check(
            !injected(InjectionVerdict::skipped_zero_offset) &&
                !injected(InjectionVerdict::refused_layout_indeterminate) &&
                !injected(
                    InjectionVerdict::refused_not_the_camera_projection) &&
                !injected(InjectionVerdict::refused_buffer_unavailable),
            "neither a skip nor any refusal reports as injected");
    }

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
