#pragma once

#include <cstdint>

namespace mfgdlss::render
{

enum class JitterOwner : std::uint32_t
{

    engine,

    plugin_injection,

    none,
};

[[nodiscard]] constexpr const char* describe(const JitterOwner owner) noexcept
{
    switch (owner) {
    case JitterOwner::engine:
        return "Skyrim's own matrices (engine-owned)";
    case JitterOwner::plugin_injection:
        return "the plugin's constant-buffer patch (UNPROVEN at the world raster)";
    case JitterOwner::none:
        return "nothing -- the raster is unjittered";
    }
    return "unknown";
}

enum class TemporalAaPolicy : std::uint32_t
{

    engine_jitter_without_resolve,

    fully_disabled,

    restore_user_setting,
};

struct JitterOwnershipState final
{

    bool upscaler_active{};

    bool offset_requested{};

    bool engine_applied_jitter{};

    std::uint32_t frames_since_split_enabled{};

    bool split_abandoned{};
};

inline constexpr std::uint32_t kSplitProbeFrames = 30U;

[[nodiscard]] constexpr bool split_probe_exhausted(
    const JitterOwnershipState& state) noexcept
{
    return state.frames_since_split_enabled >= kSplitProbeFrames;
}

[[nodiscard]] constexpr TemporalAaPolicy select_temporal_aa_policy(
    const JitterOwnershipState& state) noexcept
{
    if (!state.upscaler_active) {
        return TemporalAaPolicy::restore_user_setting;
    }
    if (state.split_abandoned) {
        return TemporalAaPolicy::fully_disabled;
    }
    if (state.engine_applied_jitter) {
        return TemporalAaPolicy::engine_jitter_without_resolve;
    }
    return split_probe_exhausted(state) ?
        TemporalAaPolicy::fully_disabled :
        TemporalAaPolicy::engine_jitter_without_resolve;
}

[[nodiscard]] constexpr JitterOwner select_jitter_owner(
    const JitterOwnershipState& state,
    const bool injection_succeeded) noexcept
{
    if (!state.upscaler_active || !state.offset_requested) {
        return JitterOwner::none;
    }
    if (state.engine_applied_jitter) {
        return JitterOwner::engine;
    }
    return injection_succeeded ?
        JitterOwner::plugin_injection :
        JitterOwner::none;
}

[[nodiscard]] constexpr bool injection_permitted(
    const JitterOwnershipState&) noexcept
{
    return false;
}

class BoundedLogBudget final
{
public:
    explicit constexpr BoundedLogBudget(const std::uint32_t budget) noexcept :
        budget_{budget}
    {
    }

    [[nodiscard]] constexpr bool admit(const std::uint32_t state) noexcept
    {
        if (spent_ >= budget_) {
            return false;
        }
        if (has_logged_ && state == last_state_) {
            return false;
        }
        has_logged_ = true;
        last_state_ = state;
        ++spent_;
        return true;
    }

    [[nodiscard]] constexpr std::uint32_t spent() const noexcept
    {
        return spent_;
    }

    [[nodiscard]] constexpr bool exhausted() const noexcept
    {
        return spent_ >= budget_;
    }

    constexpr void reset() noexcept
    {
        spent_ = 0U;
        has_logged_ = false;
        last_state_ = 0U;
    }

private:
    std::uint32_t budget_{};
    std::uint32_t spent_{};
    std::uint32_t last_state_{};
    bool has_logged_{};
};
}
