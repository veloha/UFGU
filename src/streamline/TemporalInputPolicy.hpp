#pragma once

#include "config/Settings.hpp"

namespace mfgdlss::streamline
{
struct TemporalInputPolicy
{
    bool use_dilated_motion{};
    bool use_hints{};
};

[[nodiscard]] constexpr TemporalInputPolicy select_temporal_inputs(
    const config::DlssTemporalInputs mode,
    const bool prepared,
    const bool dilation_enabled) noexcept
{
    if (!prepared) {
        return {};
    }

    switch (mode) {
    case config::DlssTemporalInputs::standard:
        return {dilation_enabled, true};
    case config::DlssTemporalInputs::no_hints:
        return {dilation_enabled, false};
    case config::DlssTemporalInputs::raw_motion:
        return {false, true};
    case config::DlssTemporalInputs::raw_motion_no_hints:
        return {false, false};
    }
    return {};
}
}
