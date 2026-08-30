#pragma once

#include <cstdint>
#include <functional>

namespace mfgdlss::render
{

enum class ProfileChangeOutcome : std::uint32_t
{

    applied = 0,

    applied_without_profile_change,

    refused_by_preflight,

    failed_runtime_settings,

    failed_persist,

    failed_reconfiguration,

    aborted_incomplete_callbacks,
};

[[nodiscard]] const char* describe(ProfileChangeOutcome outcome) noexcept;

[[nodiscard]] bool mutated_anything(ProfileChangeOutcome outcome) noexcept;

[[nodiscard]] bool succeeded(ProfileChangeOutcome outcome) noexcept;

struct ProfileChangeRequest
{

    std::uint32_t requested_mode{};

    std::uint32_t active_runtime_mode{};
};

struct ProfileChangeCallbacks
{

    std::function<bool(std::uint32_t requested_mode, const char*& reason)>
        preflight;

    std::function<void()> stage_transaction;

    std::function<bool()> apply_runtime_settings;

    std::function<bool()> persist_settings;

    std::function<void()> reset_history;

    std::function<bool()> reconfigure;

    std::function<void()> rollback;

    std::function<void(const char* reason)> report_refusal;
};

[[nodiscard]] ProfileChangeOutcome run_profile_change(
    const ProfileChangeRequest& request,
    const ProfileChangeCallbacks& callbacks);

}
