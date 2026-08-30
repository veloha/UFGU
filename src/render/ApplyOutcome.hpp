#pragma once

#include <cstdint>

namespace mfgdlss::render
{

enum class ApplyOutcome : std::uint32_t
{

    none,

    committed,

    staged_for_restart,

    refused,

    failed,

    rolled_back,
};

[[nodiscard]] constexpr bool is_accepted(const ApplyOutcome outcome) noexcept
{
    return outcome == ApplyOutcome::committed ||
           outcome == ApplyOutcome::staged_for_restart;
}

[[nodiscard]] constexpr bool may_persist_configuration(
    const ApplyOutcome outcome) noexcept
{
    return outcome == ApplyOutcome::committed ||
           outcome == ApplyOutcome::staged_for_restart;
}

[[nodiscard]] constexpr bool mutates_active_renderer(
    const ApplyOutcome outcome) noexcept
{
    return outcome == ApplyOutcome::committed ||
           outcome == ApplyOutcome::rolled_back;
}

[[nodiscard]] constexpr bool is_flagged(const ApplyOutcome outcome) noexcept
{
    return outcome == ApplyOutcome::refused ||
           outcome == ApplyOutcome::failed ||
           outcome == ApplyOutcome::rolled_back;
}

[[nodiscard]] constexpr const wchar_t* banner_prefix(
    const ApplyOutcome outcome) noexcept
{
    switch (outcome) {
    case ApplyOutcome::none:
        return L"";
    case ApplyOutcome::committed:
        return L"APPLIED: ";
    case ApplyOutcome::staged_for_restart:
        return L"RESTART REQUIRED: ";
    case ApplyOutcome::refused:
        return L"REFUSED: ";
    case ApplyOutcome::failed:
        return L"FAILED: ";
    case ApplyOutcome::rolled_back:
        return L"ROLLED BACK: ";
    }
    return L"";
}

[[nodiscard]] constexpr const char* outcome_name(
    const ApplyOutcome outcome) noexcept
{
    switch (outcome) {
    case ApplyOutcome::none:
        return "none";
    case ApplyOutcome::committed:
        return "committed";
    case ApplyOutcome::staged_for_restart:
        return "staged-for-restart";
    case ApplyOutcome::refused:
        return "refused";
    case ApplyOutcome::failed:
        return "failed";
    case ApplyOutcome::rolled_back:
        return "rolled-back";
    }
    return "unknown";
}
}
