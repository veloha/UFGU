

#include "providers/ModuleIdentity.hpp"
#include "providers/RuntimePath.hpp"

#include <Windows.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace
{
int failures{};

void check(const bool condition, const std::string& description)
{
    std::printf("  %s  %s\n", condition ? "PASS" : "FAIL", description.c_str());
    failures += condition ? 0 : 1;
}

[[nodiscard]] std::filesystem::path visible_path_of(const HMODULE module)
{
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(
        module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path{
        mfgdlss::providers::strip_extended_length_prefix(buffer)};
}

[[nodiscard]] bool same_resolved_path(
    const std::filesystem::path& left, const std::filesystem::path& right)
{
    std::error_code code;
    auto canonical_left = std::filesystem::canonical(left, code);
    if (code) {
        return false;
    }
    code.clear();
    auto canonical_right = std::filesystem::canonical(right, code);
    if (code) {
        return false;
    }
    return std::filesystem::path{
               mfgdlss::providers::strip_extended_length_prefix(
                   canonical_left.wstring())} ==
           std::filesystem::path{
               mfgdlss::providers::strip_extended_length_prefix(
                   canonical_right.wstring())};
}

void verify_module(const HMODULE module, const std::string& label)
{
    const auto location = mfgdlss::providers::module_location_of(module);
    const auto visible = visible_path_of(module);

    check(
        !visible.empty(),
        label + ": the module reports a visible path");
    check(
        location.physical_backing_verified,
        label + ": the mapped backing file resolved (" +
            (location.diagnosis.empty() ? "no diagnosis" : location.diagnosis) +
            ")");
    if (!location.physical_backing_verified) {
        return;
    }
    check(
        location.identity_confirmed,
        label + ": visible and physical paths are the same file");
    check(
        same_resolved_path(visible, location.physical_backing_path),
        label + ": physical path \"" + location.physical_backing_path.string() +
            "\" matches visible path \"" + visible.string() + "\"");
    check(
        location.physical_backing_path.is_absolute() &&
            location.physical_backing_path.has_root_name(),
        label + ": the physical path is absolute with a drive or share");
}
}

int main()
{
    using mfgdlss::providers::classify_runtime_containment;
    using mfgdlss::providers::ContainmentVerdict;
    using mfgdlss::providers::nt_device_path_to_dos_path;
    using mfgdlss::providers::this_module_location;
    using mfgdlss::providers::trusted_runtime_roots;

    std::printf("this executable\n");
    verify_module(GetModuleHandleW(nullptr), "test executable");

    std::printf("a system module loaded by the loader\n");
    verify_module(GetModuleHandleW(L"kernel32.dll"), "kernel32");

    std::printf("this module, as the loader resolves it\n");
    {
        const auto& location = this_module_location();
        const auto& roots = trusted_runtime_roots();
        check(
            location.physical_backing_verified,
            std::string{"the containing module resolved a physical backing ("} +
                (location.diagnosis.empty() ? "no diagnosis" :
                                              location.diagnosis) +
                ")");
        check(
            roots.physical_backing_verified &&
                !roots.physical_backing_root.empty(),
            "a physical trusted root was established");
        if (roots.physical_backing_verified) {

            check(
                classify_runtime_containment(
                    roots,
                    roots.physical_backing_root / "UFGU" / "AMD" /
                        "amd_fidelityfx_upscaler_dx12.dll") ==
                    ContainmentVerdict::accepted_physical_backing_root,
                "a runtime inside the real package directory is accepted");
            check(
                classify_runtime_containment(
                    roots,
                    roots.physical_backing_root.parent_path() / "escape.dll") ==
                    ContainmentVerdict::rejected_escaped,
                "a file in the real parent directory is rejected");
            check(
                classify_runtime_containment(
                    roots, LR"(C:\Windows\System32\kernel32.dll)") ==
                    ContainmentVerdict::rejected_escaped,
                "a genuinely signed system DLL is still rejected");
        }
    }

    std::printf("NT device translation on this machine\n");
    {
        check(
            nt_device_path_to_dos_path(L"").empty(),
            "an empty NT path translates to nothing");
        check(
            nt_device_path_to_dos_path(LR"(\Device\NoSuchVolume999\a.dll)")
                .empty(),
            "an unmapped device translates to nothing");
    }

    std::printf("%s\n", mfgdlss::providers::describe_trusted_runtime_roots().c_str());
    std::printf("ModuleIdentityTest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
