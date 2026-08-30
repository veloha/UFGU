#include "providers/RuntimePath.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

namespace
{
int failures{};

void check(const bool condition, const char* const description)
{
    std::printf("  %s  %s\n", condition ? "PASS" : "FAIL", description);
    failures += condition ? 0 : 1;
}

using mfgdlss::providers::ContainmentRoots;
using mfgdlss::providers::ContainmentVerdict;

const std::filesystem::path kVirtualRoot{
    LR"(C:\Modding\Instances\Example\Stock Game\Data\SKSE\Plugins)"};
const std::filesystem::path kPhysicalRoot{
    LR"(C:\Modding\Instances\Example\Mods\UFGU\SKSE\Plugins)"};
const std::filesystem::path kOtherModRoot{
    LR"(C:\Modding\Instances\Example\Mods\Other Mod\SKSE\Plugins)"};

[[nodiscard]] ContainmentRoots split_roots()
{
    return ContainmentRoots{kOtherModRoot, kPhysicalRoot, true};
}

[[nodiscard]] ContainmentRoots plain_roots()
{
    return ContainmentRoots{kPhysicalRoot, kPhysicalRoot, true};
}

[[nodiscard]] ContainmentRoots fallback_roots()
{
    return ContainmentRoots{kVirtualRoot, {}, false};
}
}

int main()
{
    using mfgdlss::providers::classify_runtime_containment;
    using mfgdlss::providers::is_accepted;
    using mfgdlss::providers::is_strict_descendant;
    using mfgdlss::providers::replace_nt_device_prefix;
    using mfgdlss::providers::strip_extended_length_prefix;

    const std::filesystem::path base{LR"(C:\Games\Skyrim\Data\SKSE\Plugins)"};

    std::printf("lexical containment\n");
    check(
        is_strict_descendant(
            base, base / "Universal-Upscaling" / "Intel" / "libxess.dll"),
        "a vendor runtime below the plugin directory is accepted");
    check(
        !is_strict_descendant(
            base, LR"(C:\Games\Skyrim\Data\SKSE\Plugins-evil\libxess.dll)"),
        "a sibling whose string merely shares the prefix is rejected");
    check(
        !is_strict_descendant(
            base, base / "Universal-Upscaling" / ".." / ".." / "outside.dll"),
        "normalised parent traversal outside the tree is rejected");
    check(
        !is_strict_descendant(base, base),
        "the directory itself is not a runtime file descendant");
    check(
        !is_strict_descendant(base, LR"(D:\VendorRuntimes\libxess.dll)"),
        "a path on another volume is rejected");
    check(
        !is_strict_descendant({}, base / "runtime.dll"),
        "an unresolved base directory is rejected");

    std::printf("split virtual/physical roots (the Mod Organizer case)\n");
    {
        const auto roots = split_roots();
        check(
            classify_runtime_containment(
                roots,
                kPhysicalRoot / "UFGU" / "AMD" /
                    "amd_fidelityfx_upscaler_dx12.dll") ==
                ContainmentVerdict::accepted_physical_backing_root,
            "the packaged AMD runtime resolves below the physical root");
        check(
            classify_runtime_containment(
                roots,
                kPhysicalRoot / "UFGU" / "Intel" / "libxess.dll") ==
                ContainmentVerdict::accepted_physical_backing_root,
            "the packaged Intel runtime resolves below the physical root");
        check(
            classify_runtime_containment(
                roots,
                kVirtualRoot / "UFGU" / "AMD" /
                    "amd_fidelityfx_upscaler_dx12.dll") ==
                ContainmentVerdict::rejected_escaped,
            "the unresolved virtual spelling is not itself a trusted location");
        check(
            classify_runtime_containment(
                roots,
                kOtherModRoot / "UFGU" / "AMD" /
                    "amd_fidelityfx_upscaler_dx12.dll") ==
                ContainmentVerdict::rejected_escaped,
            "another mod's folder is refused even though the VFS named it");
    }

    std::printf("ordinary installation, no virtualisation\n");
    {
        const auto roots = plain_roots();
        check(
            classify_runtime_containment(
                roots, kPhysicalRoot / "UFGU" / "AMD" / "runtime.dll") ==
                ContainmentVerdict::accepted_physical_backing_root,
            "a packaged runtime is accepted when both roots agree");
        check(
            classify_runtime_containment(
                roots, LR"(C:\Windows\System32\legit_signed.dll)") ==
                ContainmentVerdict::rejected_escaped,
            "an arbitrary signed system file outside the package is rejected");
        check(
            classify_runtime_containment(
                roots,
                LR"(C:\Modding\Instances\Example\Mods\UFGU\SKSE\Plugins-evil\x.dll)") ==
                ContainmentVerdict::rejected_escaped,
            "a sibling-prefix directory beside the physical root is rejected");
    }

    std::printf("junction and reparse-point escape\n");
    {
        const auto roots = split_roots();

        check(
            classify_runtime_containment(
                roots,
                LR"(C:\Attacker\payload\amd_fidelityfx_upscaler_dx12.dll)") ==
                ContainmentVerdict::rejected_escaped,
            "a junction resolving outside the package is rejected");
        check(
            classify_runtime_containment(
                roots,
                kPhysicalRoot / "UFGU" / "AMD" / ".." / ".." / ".." /
                    "elsewhere.dll") == ContainmentVerdict::rejected_escaped,
            "a reparse target normalising above the root is rejected");
    }

    std::printf("fallback when the mapped image cannot be read\n");
    {
        const auto roots = fallback_roots();
        check(
            classify_runtime_containment(
                roots, kVirtualRoot / "UFGU" / "AMD" / "runtime.dll") ==
                ContainmentVerdict::accepted_module_visible_root,
            "the visible root is used only when no physical root exists");
        check(
            classify_runtime_containment(
                roots, kPhysicalRoot / "UFGU" / "AMD" / "runtime.dll") ==
                ContainmentVerdict::rejected_escaped,
            "the fallback root does not silently accept another tree");
        check(
            classify_runtime_containment(ContainmentRoots{}, base / "x.dll") ==
                ContainmentVerdict::rejected_no_trusted_root,
            "no root at all fails closed rather than accepting");
        check(
            !is_accepted(ContainmentVerdict::rejected_no_trusted_root) &&
                !is_accepted(ContainmentVerdict::rejected_escaped) &&
                is_accepted(
                    ContainmentVerdict::accepted_physical_backing_root) &&
                is_accepted(ContainmentVerdict::accepted_module_visible_root),
            "acceptance is reported for exactly the two accepting verdicts");
    }

    std::printf("NT device path translation\n");
    {
        std::wstring out;
        check(
            replace_nt_device_prefix(
                LR"(\Device\HarddiskVolume3\Example\a.dll)",
                LR"(\Device\HarddiskVolume3)",
                L"C:",
                out) &&
                out == LR"(C:\Example\a.dll)",
            "a device path is rewritten to its drive letter");
        out.clear();
        check(
            !replace_nt_device_prefix(
                LR"(\Device\HarddiskVolume11\Example\a.dll)",
                LR"(\Device\HarddiskVolume1)",
                L"C:",
                out),
            "HarddiskVolume1 does not match HarddiskVolume11");
        check(
            replace_nt_device_prefix(
                LR"(\Device\HarddiskVolume11\Example\a.dll)",
                LR"(\Device\HarddiskVolume11)",
                L"D:",
                out) &&
                out == LR"(D:\Example\a.dll)",
            "the longer volume name still matches itself");
        check(
            !replace_nt_device_prefix(
                LR"(\Device\HarddiskVolume3)",
                LR"(\Device\HarddiskVolume3)",
                L"C:",
                out),
            "the bare device with no trailing path is rejected");
        check(
            !replace_nt_device_prefix(
                LR"(\Device\Other\a.dll)",
                LR"(\Device\HarddiskVolume3)",
                L"C:",
                out),
            "an unrelated device is rejected");
        check(
            !replace_nt_device_prefix(LR"(\Device\X\a.dll)", {}, L"C:", out),
            "an empty device name is rejected");
    }

    std::printf("extended-length prefix normalisation\n");
    check(
        strip_extended_length_prefix(LR"(\\?\C:\Games\a.dll)") ==
            LR"(C:\Games\a.dll)",
        "a local extended-length prefix is removed");
    check(
        strip_extended_length_prefix(LR"(\\?\UNC\server\share\a.dll)") ==
            LR"(\\server\share\a.dll)",
        "a UNC extended-length prefix becomes a normal UNC path");
    check(
        strip_extended_length_prefix(LR"(C:\Games\a.dll)") ==
            LR"(C:\Games\a.dll)",
        "a plain path is left alone");
    check(
        strip_extended_length_prefix(L"").empty(),
        "an empty path is left alone");
    check(
        std::filesystem::path{strip_extended_length_prefix(
                                  LR"(\\?\C:\Modding\Mods\Example\SKSE\Plugins\x.dll)")}
                .parent_path() ==
            std::filesystem::path{LR"(C:\Modding\Mods\Example\SKSE\Plugins)"},
        "a normalised extended-length path yields a comparable parent");

    std::printf("RuntimePathTest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
