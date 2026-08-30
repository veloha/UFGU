#pragma once

#include <Windows.h>

#include <cstdint>

namespace mfgdlss::render
{

class BaseFrameLimiter final
{
public:
    [[nodiscard]] static BaseFrameLimiter& instance() noexcept;
    ~BaseFrameLimiter();

    BaseFrameLimiter(const BaseFrameLimiter&) = delete;
    BaseFrameLimiter& operator=(const BaseFrameLimiter&) = delete;

    void wait(std::uint32_t frames_per_second, bool bypass) noexcept;
    void reset() noexcept;
    [[nodiscard]] float last_wait_milliseconds() const noexcept;
    [[nodiscard]] float average_wait_milliseconds() const noexcept;

private:
    BaseFrameLimiter() noexcept;

    HANDLE timer_{};
    std::int64_t frequency_{};
    long double deadline_ticks_{};
    std::uint32_t active_limit_{};
    float last_wait_milliseconds_{};
    float wait_accumulator_{};
    std::uint32_t wait_samples_{};
    float average_wait_milliseconds_{};
    float worst_wait_milliseconds_{};
};
}
