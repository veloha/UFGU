#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mfgdlss::diagnostics
{

struct SeriesStatistics
{
    float current{};
    float average{};
    float minimum{};
    float maximum{};
    float median{};
    float percentile_95{};
    float percentile_99{};

    float low_1_percent_fps{};
    float low_0_1_percent_fps{};
    std::uint32_t sample_count{};
    float window_seconds{};

    float pacing_deviation_milliseconds{};
    float pacing_deviation_ratio{};

    std::uint32_t short_interval_count{};
    std::uint32_t long_interval_count{};
};

class FrameTimeSeries final
{
public:

    static constexpr std::size_t kCapacity = 16384;

    void push(float value_milliseconds, double timestamp_seconds) noexcept;
    void clear() noexcept;

    [[nodiscard]] SeriesStatistics statistics(
        double now_seconds,
        float window_seconds) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept;

    [[nodiscard]] std::size_t recent(
        float* out,
        std::size_t count,
        double now_seconds,
        float window_seconds) const noexcept;

private:
    struct Sample
    {
        float value{};
        double timestamp{};
    };

    std::array<Sample, kCapacity> samples_{};
    std::size_t next_{};
    std::size_t count_{};
};
}
