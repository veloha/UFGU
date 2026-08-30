#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace mfgdlss::providers
{

[[nodiscard]] inline bool is_strict_descendant(
    const std::filesystem::path& base,
    const std::filesystem::path& candidate)
{
    if (base.empty() || candidate.empty()) {
        return false;
    }

    const auto relative = candidate.lexically_normal().lexically_relative(
        base.lexically_normal());
    if (relative.empty() || relative.is_absolute() ||
        relative == std::filesystem::path{"."}) {
        return false;
    }

    for (const auto& component : relative) {
        if (component == std::filesystem::path{".."}) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline std::wstring strip_extended_length_prefix(
    const std::wstring_view path)
{
    constexpr std::wstring_view unc_prefix{LR"(\\?\UNC\)"};
    constexpr std::wstring_view local_prefix{LR"(\\?\)"};
    if (path.size() >= unc_prefix.size() &&
        path.substr(0, unc_prefix.size()) == unc_prefix) {
        return LR"(\\)" + std::wstring{path.substr(unc_prefix.size())};
    }
    if (path.size() >= local_prefix.size() &&
        path.substr(0, local_prefix.size()) == local_prefix) {
        return std::wstring{path.substr(local_prefix.size())};
    }
    return std::wstring{path};
}

[[nodiscard]] inline bool replace_nt_device_prefix(
    const std::wstring_view nt_path,
    const std::wstring_view device_name,
    const std::wstring_view dos_root,
    std::wstring& out)
{
    if (device_name.empty() || nt_path.size() <= device_name.size()) {
        return false;
    }
    if (nt_path.substr(0, device_name.size()) != device_name) {
        return false;
    }
    if (nt_path[device_name.size()] != L'\\') {
        return false;
    }
    out = std::wstring{dos_root} +
          std::wstring{nt_path.substr(device_name.size())};
    return true;
}

struct ContainmentRoots final
{
    std::filesystem::path module_visible_root;
    std::filesystem::path physical_backing_root;
    bool physical_backing_verified{};
};

enum class ContainmentVerdict : std::uint32_t
{
    accepted_physical_backing_root,
    accepted_module_visible_root,
    rejected_no_trusted_root,
    rejected_escaped,
};

[[nodiscard]] constexpr bool is_accepted(
    const ContainmentVerdict verdict) noexcept
{
    return verdict == ContainmentVerdict::accepted_physical_backing_root ||
           verdict == ContainmentVerdict::accepted_module_visible_root;
}

[[nodiscard]] constexpr const char* describe(
    const ContainmentVerdict verdict) noexcept
{
    switch (verdict) {
    case ContainmentVerdict::accepted_physical_backing_root:
        return "below the physical directory backing this plugin";
    case ContainmentVerdict::accepted_module_visible_root:
        return "below the directory this plugin reports itself in";
    case ContainmentVerdict::rejected_no_trusted_root:
        return "no trusted plugin root could be established";
    case ContainmentVerdict::rejected_escaped:
        return "the runtime is not inside this plugin's own package";
    }
    return "unknown";
}

[[nodiscard]] inline ContainmentVerdict classify_runtime_containment(
    const ContainmentRoots& roots,
    const std::filesystem::path& resolved_runtime)
{
    if (roots.physical_backing_verified &&
        !roots.physical_backing_root.empty()) {
        return is_strict_descendant(
                   roots.physical_backing_root, resolved_runtime) ?
                   ContainmentVerdict::accepted_physical_backing_root :
                   ContainmentVerdict::rejected_escaped;
    }
    if (!roots.module_visible_root.empty()) {
        return is_strict_descendant(
                   roots.module_visible_root, resolved_runtime) ?
                   ContainmentVerdict::accepted_module_visible_root :
                   ContainmentVerdict::rejected_escaped;
    }
    return ContainmentVerdict::rejected_no_trusted_root;
}
}
