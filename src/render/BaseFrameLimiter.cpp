#include "render/BaseFrameLimiter.hpp"

#include <intrin.h>

#include <SKSE/SKSE.h>

#include <algorithm>
#include <cmath>

namespace mfgdlss::render
{
namespace logger = SKSE::log;
namespace
{
constexpr DWORD kHighResolutionTimer = 0x00000002;
constexpr std::uint32_t kPacingCensusWindow = 600U;
}

BaseFrameLimiter& BaseFrameLimiter::instance() noexcept
{
    static BaseFrameLimiter limiter;
    return limiter;
}

BaseFrameLimiter::BaseFrameLimiter() noexcept
{
    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency) != FALSE) {
        frequency_ = frequency.QuadPart;
    }
    timer_ = CreateWaitableTimerExW(
        nullptr,
        nullptr,
        kHighResolutionTimer,
        TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (timer_ == nullptr) {
        timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
}

BaseFrameLimiter::~BaseFrameLimiter()
{
    if (timer_ != nullptr) {
        CloseHandle(timer_);
    }
}

void BaseFrameLimiter::reset() noexcept
{
    deadline_ticks_ = 0.0L;
    active_limit_ = 0U;
    last_wait_milliseconds_ = 0.0F;
    wait_accumulator_ = 0.0F;
    wait_samples_ = 0U;
    average_wait_milliseconds_ = 0.0F;
    worst_wait_milliseconds_ = 0.0F;
}

float BaseFrameLimiter::last_wait_milliseconds() const noexcept
{
    return last_wait_milliseconds_;
}

float BaseFrameLimiter::average_wait_milliseconds() const noexcept
{
    return average_wait_milliseconds_;
}

void BaseFrameLimiter::wait(
    const std::uint32_t frames_per_second,
    const bool bypass) noexcept
{
    if (bypass || frames_per_second == 0U || frequency_ <= 0) {
        reset();
        return;
    }

    LARGE_INTEGER counter{};
    if (QueryPerformanceCounter(&counter) == FALSE) {
        reset();
        return;
    }
    const auto now = static_cast<long double>(counter.QuadPart);
    const auto interval = static_cast<long double>(frequency_) /
                          static_cast<long double>(frames_per_second);
    if (active_limit_ != frames_per_second || deadline_ticks_ == 0.0L) {
        active_limit_ = frames_per_second;
        deadline_ticks_ = now;
        return;
    }

    auto target = deadline_ticks_ + interval;

    if (now > target + interval) {
        deadline_ticks_ = now;
        return;
    }

    auto current = now;
    while (current < target) {
        const auto remaining_ticks = target - current;
        const auto remaining_seconds =
            remaining_ticks / static_cast<long double>(frequency_);

        if (timer_ != nullptr && remaining_seconds > 0.002L) {
            const auto sleep_seconds = remaining_seconds - 0.00075L;
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>((std::max)(
                1.0L, sleep_seconds * 10000000.0L));
            if (SetWaitableTimer(
                    timer_, &due, 0, nullptr, nullptr, FALSE) != FALSE) {
                static_cast<void>(WaitForSingleObject(timer_, INFINITE));
            }
        } else if (remaining_seconds > 0.00020L) {
            static_cast<void>(SwitchToThread());
        } else {
            _mm_pause();
        }
        if (QueryPerformanceCounter(&counter) == FALSE) {
            reset();
            return;
        }
        current = static_cast<long double>(counter.QuadPart);
    }
    deadline_ticks_ = target;

    const auto waited_milliseconds = static_cast<float>(
        (current - now) / static_cast<long double>(frequency_) * 1000.0L);
    last_wait_milliseconds_ = waited_milliseconds;
    wait_accumulator_ += waited_milliseconds;
    ++wait_samples_;
    average_wait_milliseconds_ =
        wait_accumulator_ / static_cast<float>(wait_samples_);
    worst_wait_milliseconds_ =
        (std::max)(worst_wait_milliseconds_, waited_milliseconds);
    if (wait_samples_ >= kPacingCensusWindow) {
        logger::info(
            "Base frame limiter census over {} waits at {} fps: mean {:.2f}ms "
            "spent waiting, worst {:.2f}ms, last {:.2f}ms. This is the pacer "
            "holding Skyrim's render rate down to the base limit. A mean near "
            "zero means the game is already slower than the limit, so the "
            "limiter is not what is shaping the cadence",
            wait_samples_,
            frames_per_second,
            average_wait_milliseconds_,
            worst_wait_milliseconds_,
            last_wait_milliseconds_);
        wait_accumulator_ = 0.0F;
        wait_samples_ = 0U;
        worst_wait_milliseconds_ = 0.0F;
    }
}
}
