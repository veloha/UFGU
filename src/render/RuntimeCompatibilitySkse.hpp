#pragma once

#include "render/RuntimeCompatibility.hpp"

#include <REL/Relocation.h>

namespace mfgdlss::render
{
[[nodiscard]] inline const RuntimeProfile* active_runtime_profile() noexcept
{
    const auto version = REL::Module::get().version();
    return runtime_profile_for({
        version.major(),
        version.minor(),
        version.patch(),
        version.build(),
    });
}
}
