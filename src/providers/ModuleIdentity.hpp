#pragma once

#include "providers/RuntimePath.hpp"

#include <filesystem>
#include <string>

namespace mfgdlss::providers
{

struct ModuleLocation final
{
    std::filesystem::path module_visible_path;
    std::filesystem::path physical_backing_path;
    bool physical_backing_verified{};
    bool identity_confirmed{};
    std::string diagnosis;
};

[[nodiscard]] const ModuleLocation& this_module_location();

[[nodiscard]] const ContainmentRoots& trusted_runtime_roots();

[[nodiscard]] std::string describe_trusted_runtime_roots();

[[nodiscard]] std::wstring nt_device_path_to_dos_path(std::wstring_view nt_path);

[[nodiscard]] ModuleLocation module_location_of(void* module_base);
}
