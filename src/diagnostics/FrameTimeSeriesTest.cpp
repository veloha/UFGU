

#include "diagnostics/FrameTimeSeries.hpp"

#include <cmath>
#include <limits>
#include <cstdio>

namespace
{
int failures{};

void check(const bool condition, const char* const description)
{
    std::printf("  %s  %s\n", condition ? "PASS" : "FAIL", description);
    failures += condition ? 0 : 1;
}

[[nodiscard]] bool near(const float value, const float expected, const float tolerance)
{
    return std::fabs(value - expected) <= tolerance;
}

using mfgdlss::diagnostics::FrameTimeSeries;
using mfgdlss::diagnostics::SeriesStatistics;

void fill(
    FrameTimeSeries& series,
    const float* const values,
    const std::size_t count,
    double& clock)
{
    for (std::size_t index = 0; index < count; ++index) {
        clock += static_cast<double>(values[index]) / 1000.0;
        series.push(values[index], clock);
    }
}

void fill_constant(
    FrameTimeSeries& series,
    const float value,
    const std::size_t count,
    double& clock)
{
    for (std::size_t index = 0; index < count; ++index) {
        clock += static_cast<double>(value) / 1000.0;
        series.push(value, clock);
    }
}
}

int main()
{
    std::printf("an empty series\n");
    {
        const FrameTimeSeries series;
        const auto stats = series.statistics(1.0, 5.0F);
        check(
            stats.sample_count == 0U && stats.average == 0.0F &&
                stats.pacing_deviation_ratio == 0.0F,
            "reports zeroes rather than dividing by an empty window");
    }

    std::printf("a perfectly paced 240 FPS window\n");
    {
        FrameTimeSeries series;
        double clock{};
        fill_constant(series, 1000.0F / 240.0F, 2000U, clock);
        const auto stats = series.statistics(clock, 5.0F);
        check(stats.sample_count > 1000U, "the window holds the samples");
        check(
            near(stats.median, 1000.0F / 240.0F, 0.001F),
            "the median interval is the 240 FPS interval");
        check(
            stats.pacing_deviation_milliseconds == 0.0F &&
                stats.pacing_deviation_ratio == 0.0F,
            "a metronome has zero pacing deviation");
        check(
            stats.short_interval_count == 0U && stats.long_interval_count == 0U,
            "a metronome has no short or long intervals");
    }

    std::printf("a 500 FPS window with a recurring drop/duplicate pair\n");
    {

        FrameTimeSeries series;
        double clock{};
        for (int block = 0; block < 100; ++block) {
            for (int index = 0; index < 10; ++index) {
                clock += 0.002;
                series.push(2.0F, clock);
            }
            clock += 0.0005;
            series.push(0.5F, clock);
            clock += 0.0035;
            series.push(3.5F, clock);
        }
        const auto stats = series.statistics(clock, 10.0F);
        check(
            near(stats.average, 2.0F, 0.05F),
            "the mean interval is still the 500 FPS interval");
        check(
            near(stats.median, 2.0F, 0.001F),
            "the median sits on the intended cadence");
        check(
            stats.short_interval_count == 100U &&
                stats.long_interval_count == 100U,
            "the duplicated and dropped populations are counted separately");
        check(
            stats.pacing_deviation_milliseconds > 0.1F &&
                stats.pacing_deviation_ratio > 0.05F,
            "the deviation flags the window despite an unchanged mean");
    }

    std::printf("an evenly split bimodal window\n");
    {

        FrameTimeSeries series;
        double clock{};
        for (int index = 0; index < 1000; ++index) {
            clock += 0.0005;
            series.push(0.5F, clock);
            clock += 0.0035;
            series.push(3.5F, clock);
        }
        const auto stats = series.statistics(clock, 10.0F);
        check(
            near(stats.median, 0.5F, 0.001F),
            "an even split puts the median on the lower mode");
        check(
            stats.short_interval_count == 0U &&
                stats.long_interval_count == 1000U,
            "every sample of the upper mode is counted long, none short");
        check(
            stats.pacing_deviation_ratio > 1.0F,
            "the deviation ratio still flags the window as badly paced");
    }

    std::printf("a steady window with a single long stall\n");
    {
        FrameTimeSeries series;
        double clock{};
        fill_constant(series, 4.0F, 999U, clock);
        clock += 0.050;
        series.push(50.0F, clock);
        const auto stats = series.statistics(clock, 30.0F);
        check(
            stats.long_interval_count == 1U && stats.short_interval_count == 0U,
            "one stall is counted as exactly one long interval");
        check(
            near(stats.median, 4.0F, 0.001F),
            "one stall does not move the median");
        check(
            stats.maximum >= 50.0F, "the stall is visible in the maximum");
        check(
            stats.low_0_1_percent_fps > 0.0F &&
                stats.low_0_1_percent_fps < stats.low_1_percent_fps,
            "the 0.1% low is slower than the 1% low");
    }

    std::printf("percentile direction\n");
    {
        FrameTimeSeries series;
        double clock{};
        float values[100]{};
        for (int index = 0; index < 100; ++index) {
            values[index] = static_cast<float>(index + 1);
        }
        fill(series, values, 100U, clock);
        const auto stats = series.statistics(clock, 3600.0F);
        check(
            stats.percentile_99 > stats.median &&
                stats.median > stats.minimum,
            "a high percentile is a slow frame, not a fast one");
        check(
            stats.low_1_percent_fps > 0.0F &&
                stats.low_1_percent_fps < 1000.0F / stats.median,
            "the 1% low is slower than the median rate");
    }

    std::printf("non-finite samples\n");
    {
        FrameTimeSeries series;
        double clock{};
        fill_constant(series, 4.0F, 10U, clock);
        clock += 0.004;
        series.push(std::nanf(""), clock);
        series.push(INFINITY, clock);
        const auto stats = series.statistics(clock, 30.0F);
        check(
            stats.sample_count == 10U,
            "non-finite samples never enter the window");
        check(
            std::isfinite(stats.pacing_deviation_ratio) &&
                std::isfinite(stats.average),
            "every derived statistic stays finite");
    }

    std::printf("window trimming\n");
    {
        FrameTimeSeries series;
        double clock{};
        fill_constant(series, 10.0F, 500U, clock);
        const auto narrow = series.statistics(clock, 1.0F);
        const auto wide = series.statistics(clock, 60.0F);
        check(
            narrow.sample_count < wide.sample_count,
            "a narrower window gathers fewer samples");
        check(
            narrow.sample_count > 0U && near(narrow.median, 10.0F, 0.001F),
            "the narrow window still describes the same cadence");
    }

    {
        FrameTimeSeries series;
        check(series.size() == 0U, "a new series is empty");
        series.push(4.0F, 0.0);
        series.push(4.0F, 1.0);
        check(series.size() == 2U, "size counts pushed samples");
        series.clear();
        check(series.size() == 0U, "clear empties the series");

        series.push(std::numeric_limits<float>::quiet_NaN(), 0.0);
        series.push(std::numeric_limits<float>::infinity(), 1.0);
        series.push(-std::numeric_limits<float>::infinity(), 2.0);
        check(
            series.size() == 0U,
            "non-finite samples are dropped rather than stored");
    }

    {
        FrameTimeSeries series;
        for (std::size_t index = 0; index < 100U; ++index) {
            series.push(
                static_cast<float>(index + 1U),
                static_cast<double>(index) * 0.001);
        }
        const auto stats = series.statistics(1.0, 100.0);
        check(stats.sample_count == 100U, "the whole window is gathered");
        check(near(stats.minimum, 1.0F, 1.0e-4F), "minimum is the smallest");
        check(near(stats.maximum, 100.0F, 1.0e-4F), "maximum is the largest");
        check(
            near(stats.median, 50.0F, 1.0e-4F),
            "nearest-rank median of 1 to 100 is the 50th value");
        check(
            near(stats.percentile_95, 95.0F, 1.0e-4F),
            "nearest-rank 95th percentile of 1 to 100 is the 95th value");
        check(
            near(stats.percentile_99, 99.0F, 1.0e-4F),
            "nearest-rank 99th percentile of 1 to 100 is the 99th value");
        check(
            stats.minimum <= stats.median && stats.median <= stats.percentile_95 &&
                stats.percentile_95 <= stats.percentile_99 &&
                stats.percentile_99 <= stats.maximum,
            "the percentiles are ordered within the observed range");
        check(
            near(stats.low_1_percent_fps, 10.0F, 1.0e-3F),
            "the 1 percent low averages the single worst interval of 100");
        check(
            near(stats.low_0_1_percent_fps, 0.0F, 1.0e-6F),
            "the 0.1 percent low needs more than 100 samples to report");
    }

    {
        FrameTimeSeries series;
        series.push(7.0F, 0.0);
        const auto stats = series.statistics(1.0, 100.0);
        check(
            near(stats.median, 7.0F, 1.0e-4F) &&
                near(stats.percentile_95, 7.0F, 1.0e-4F) &&
                near(stats.percentile_99, 7.0F, 1.0e-4F),
            "a single sample is every percentile of itself");
        check(
            near(stats.low_1_percent_fps, 0.0F, 1.0e-6F),
            "a single sample is too few for a 1 percent low");
    }

    {
        FrameTimeSeries series;
        series.push(10.0F, 0.0);
        series.push(20.0F, 1.0);
        const auto stats = series.statistics(2.0, 100.0);
        check(
            near(stats.median, 10.0F, 1.0e-4F),
            "an even split puts the nearest-rank median on the lower value");
        check(
            near(stats.percentile_95, 20.0F, 1.0e-4F),
            "the 95th percentile of two samples is the upper value");
    }

    {
        FrameTimeSeries series;
        for (std::size_t index = 0; index < 5U; ++index) {
            series.push(
                static_cast<float>(index + 1U),
                static_cast<double>(index));
        }
        float out[8]{};
        check(
            series.recent(out, 8U, 4.0, 10.0F) == 5U,
            "recent returns every sample inside the window");
        check(
            near(out[0], 1.0F, 1.0e-4F) && near(out[4], 5.0F, 1.0e-4F),
            "recent writes oldest first and newest last");

        check(
            series.recent(out, 3U, 4.0, 10.0F) == 3U,
            "recent honours the caller's capacity");
        check(
            near(out[0], 3.0F, 1.0e-4F) && near(out[2], 5.0F, 1.0e-4F),
            "a capped read keeps the newest samples");

        check(
            series.recent(out, 8U, 4.0, 2.5F) == 3U,
            "recent stops at the window cutoff");
        check(
            near(out[0], 3.0F, 1.0e-4F) && near(out[2], 5.0F, 1.0e-4F),
            "the windowed read is the newest run of samples");

        check(
            series.recent(nullptr, 8U, 4.0, 10.0F) == 0U,
            "a null destination writes nothing");
        check(
            series.recent(out, 0U, 4.0, 10.0F) == 0U,
            "a zero capacity writes nothing");

        FrameTimeSeries empty;
        check(
            empty.recent(out, 8U, 4.0, 10.0F) == 0U,
            "an empty series has nothing recent");
    }

    std::printf("FrameTimeSeriesTest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
