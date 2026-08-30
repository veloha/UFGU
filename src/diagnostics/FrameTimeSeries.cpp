#include "diagnostics/FrameTimeSeries.hpp"

#include <algorithm>
#include <cmath>

namespace mfgdlss::diagnostics
{
namespace
{

thread_local std::array<float, FrameTimeSeries::kCapacity> sorting_scratch{};
}

void FrameTimeSeries::push(
    const float value_milliseconds,
    const double timestamp_seconds) noexcept
{

    if (!std::isfinite(value_milliseconds)) {
        return;
    }
    samples_[next_] = Sample{value_milliseconds, timestamp_seconds};
    next_ = (next_ + 1U) % kCapacity;
    if (count_ < kCapacity) {
        ++count_;
    }
}

void FrameTimeSeries::clear() noexcept
{
    next_ = 0U;
    count_ = 0U;
}

std::size_t FrameTimeSeries::size() const noexcept
{
    return count_;
}

std::size_t FrameTimeSeries::recent(
    float* const out,
    const std::size_t count,
    const double now_seconds,
    const float window_seconds) const noexcept
{
    if (out == nullptr || count == 0U || count_ == 0U) {
        return 0U;
    }
    const auto cutoff = now_seconds - static_cast<double>(window_seconds);
    std::size_t written{};

    for (std::size_t step = 0; step < count_ && written < count; ++step) {
        const auto index = (next_ + kCapacity - 1U - step) % kCapacity;
        if (samples_[index].timestamp < cutoff) {
            break;
        }
        out[written] = samples_[index].value;
        ++written;
    }
    std::reverse(out, out + written);
    return written;
}

SeriesStatistics FrameTimeSeries::statistics(
    const double now_seconds,
    const float window_seconds) const noexcept
{
    SeriesStatistics result;
    result.window_seconds = window_seconds;
    if (count_ == 0U) {
        return result;
    }

    const auto cutoff = now_seconds - static_cast<double>(window_seconds);
    std::size_t gathered{};
    auto total = 0.0;
    auto minimum = 0.0F;
    auto maximum = 0.0F;
    for (std::size_t step = 0; step < count_; ++step) {
        const auto index = (next_ + kCapacity - 1U - step) % kCapacity;
        const auto& sample = samples_[index];
        if (sample.timestamp < cutoff) {
            break;
        }
        if (gathered == 0U) {
            result.current = sample.value;
            minimum = sample.value;
            maximum = sample.value;
        } else {
            minimum = (std::min)(minimum, sample.value);
            maximum = (std::max)(maximum, sample.value);
        }
        sorting_scratch[gathered] = sample.value;
        total += static_cast<double>(sample.value);
        ++gathered;
    }
    if (gathered == 0U) {
        return result;
    }

    result.sample_count = static_cast<std::uint32_t>(gathered);
    result.average = static_cast<float>(total / static_cast<double>(gathered));
    result.minimum = minimum;
    result.maximum = maximum;

    auto* const begin = sorting_scratch.data();
    auto* const end = begin + gathered;
    std::sort(begin, end);

    const auto at_percentile = [begin, gathered](const double fraction) {

        const auto rank = static_cast<std::size_t>(
            std::ceil(fraction * static_cast<double>(gathered)));
        const auto index = rank == 0U ? 0U : (std::min)(rank - 1U, gathered - 1U);
        return begin[index];
    };
    result.median = at_percentile(0.50);
    result.percentile_95 = at_percentile(0.95);
    result.percentile_99 = at_percentile(0.99);

    const auto tail_average_fps = [begin, end, gathered](const double fraction) {
        const auto tail = static_cast<std::size_t>(
            std::floor(fraction * static_cast<double>(gathered)));
        if (tail == 0U) {
            return 0.0F;
        }
        auto sum = 0.0;
        for (auto* value = end - tail; value != end; ++value) {
            sum += static_cast<double>(*value);
        }
        const auto mean_milliseconds = sum / static_cast<double>(tail);
        return mean_milliseconds > 0.0 ?
            static_cast<float>(1000.0 / mean_milliseconds) :
            0.0F;
    };
    result.low_1_percent_fps = tail_average_fps(0.01);
    result.low_0_1_percent_fps = tail_average_fps(0.001);

    if (result.median > 0.0F) {
        auto deviation_total = 0.0;
        const auto short_threshold = result.median * 0.5F;
        const auto long_threshold = result.median * 1.5F;
        for (auto* value = begin; value != end; ++value) {
            deviation_total +=
                static_cast<double>(std::fabs(*value - result.median));
            if (*value < short_threshold) {
                ++result.short_interval_count;
            } else if (*value > long_threshold) {
                ++result.long_interval_count;
            }
        }
        result.pacing_deviation_milliseconds = static_cast<float>(
            deviation_total / static_cast<double>(gathered));
        result.pacing_deviation_ratio =
            result.pacing_deviation_milliseconds / result.median;
    }
    return result;
}
}
