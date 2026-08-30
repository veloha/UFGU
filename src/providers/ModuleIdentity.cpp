#include "providers/ModuleIdentity.hpp"

#include <Windows.h>

#include <Psapi.h>

#include <array>
#include <optional>
#include <system_error>

namespace mfgdlss::providers
{
namespace
{

const int kModuleMarker{};

struct FileIdentity final
{
    DWORD volume_serial{};
    DWORD index_high{};
    DWORD index_low{};

    [[nodiscard]] bool operator==(const FileIdentity& other) const noexcept
    {
        return volume_serial == other.volume_serial &&
               index_high == other.index_high && index_low == other.index_low;
    }
};

[[nodiscard]] std::optional<FileIdentity> identity_of(
    const std::filesystem::path& path) noexcept
{
    if (path.empty()) {
        return std::nullopt;
    }
    const auto handle = CreateFileW(
        path.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    const auto queried = GetFileInformationByHandle(handle, &information);
    static_cast<void>(CloseHandle(handle));
    if (queried == 0) {
        return std::nullopt;
    }
    return FileIdentity{
        information.dwVolumeSerialNumber,
        information.nFileIndexHigh,
        information.nFileIndexLow};
}

[[nodiscard]] HMODULE this_module_handle() noexcept
{
    HMODULE module{};
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&kModuleMarker),
            &module) == 0) {
        return nullptr;
    }
    return module;
}

[[nodiscard]] std::filesystem::path module_visible_path_of(
    const HMODULE module) noexcept
{
    if (module == nullptr) {
        return {};
    }
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(
        module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path{strip_extended_length_prefix(buffer)};
}

[[nodiscard]] std::wstring mapped_nt_path_of(const HMODULE module) noexcept
{
    if (module == nullptr) {
        return {};
    }
    std::wstring buffer(32768, L'\0');
    const auto length = GetMappedFileNameW(
        GetCurrentProcess(),
        reinterpret_cast<void*>(module),
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return buffer;
}

[[nodiscard]] bool equal_ignoring_case(
    const std::wstring& left, const std::wstring& right) noexcept
{
    if (left.size() != right.size()) {
        return false;
    }
    return CompareStringOrdinal(
               left.c_str(),
               static_cast<int>(left.size()),
               right.c_str(),
               static_cast<int>(right.size()),
               TRUE) == CSTR_EQUAL;
}

[[nodiscard]] ModuleLocation resolve_module(const HMODULE module)
{
    ModuleLocation location;

    if (module == nullptr) {
        location.diagnosis = "the module handle could not be resolved";
        return location;
    }

    location.module_visible_path = module_visible_path_of(module);
    if (location.module_visible_path.empty()) {
        location.diagnosis = "the module file name could not be resolved";
        return location;
    }

    const auto nt_path = mapped_nt_path_of(module);
    if (nt_path.empty()) {
        location.diagnosis =
            "the mapped image path was unavailable (Win32 error " +
            std::to_string(GetLastError()) + ")";
        return location;
    }

    const auto dos_path = nt_device_path_to_dos_path(nt_path);
    if (dos_path.empty()) {
        location.diagnosis =
            "no drive letter maps to the device holding the mapped image";
        return location;
    }
    location.physical_backing_path = std::filesystem::path{dos_path};

    if (!equal_ignoring_case(
            location.physical_backing_path.filename().wstring(),
            location.module_visible_path.filename().wstring())) {
        location.diagnosis =
            "the mapped image names a different file than this module";
        return location;
    }

    std::error_code code;
    if (!std::filesystem::is_regular_file(
            location.physical_backing_path, code)) {
        location.diagnosis =
            "the mapped image path does not name a readable file";
        return location;
    }

    code.clear();
    auto canonical_physical =
        std::filesystem::canonical(location.physical_backing_path, code);
    if (!code) {
        canonical_physical = std::filesystem::path{
            strip_extended_length_prefix(canonical_physical.wstring())};
        location.physical_backing_path = std::move(canonical_physical);
    }

    const auto visible_identity = identity_of(location.module_visible_path);
    const auto physical_identity = identity_of(location.physical_backing_path);
    if (visible_identity.has_value() && physical_identity.has_value()) {
        if (!(*visible_identity == *physical_identity)) {
            location.diagnosis =
                "the visible and mapped module paths are different files";
            return location;
        }
        location.identity_confirmed = true;
    }

    location.physical_backing_verified = true;
    return location;
}
}

std::wstring nt_device_path_to_dos_path(const std::wstring_view nt_path)
{
    if (nt_path.empty()) {
        return {};
    }

    auto drives = GetLogicalDrives();
    std::array<wchar_t, 3> root{L'A', L':', L'\0'};
    std::wstring device(MAX_PATH, L'\0');
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter, drives >>= 1U) {
        if ((drives & 1U) == 0U) {
            continue;
        }
        root[0] = letter;
        const auto length = QueryDosDeviceW(
            root.data(), device.data(), static_cast<DWORD>(device.size()));
        if (length == 0) {
            continue;
        }

        const std::wstring_view device_name{device.c_str()};
        std::wstring translated;
        if (replace_nt_device_prefix(
                nt_path, device_name, root.data(), translated)) {
            return translated;
        }
    }
    return {};
}

ModuleLocation module_location_of(void* const module_base)
{
    return resolve_module(static_cast<HMODULE>(module_base));
}

const ModuleLocation& this_module_location()
{
    static const ModuleLocation location = resolve_module(this_module_handle());
    return location;
}

const ContainmentRoots& trusted_runtime_roots()
{
    static const ContainmentRoots roots = [] {
        const auto& location = this_module_location();
        ContainmentRoots value;

        if (!location.module_visible_path.empty()) {
            std::error_code code;
            auto canonical_visible = std::filesystem::canonical(
                location.module_visible_path.parent_path(), code);
            if (!code) {
                value.module_visible_root = std::filesystem::path{
                    strip_extended_length_prefix(canonical_visible.wstring())};
            }
        }

        if (location.physical_backing_verified) {
            value.physical_backing_root =
                location.physical_backing_path.parent_path();
            value.physical_backing_verified =
                !value.physical_backing_root.empty();
        }
        return value;
    }();
    return roots;
}

std::string describe_trusted_runtime_roots()
{
    const auto& location = this_module_location();
    const auto& roots = trusted_runtime_roots();
    std::string description =
        "Runtime containment roots: visible=\"" +
        location.module_visible_path.string() + "\" physical=\"" +
        location.physical_backing_path.string() + "\" physical-verified=" +
        (location.physical_backing_verified ? "true" : "false") +
        " identity-confirmed=" +
        (location.identity_confirmed ? "true" : "false") +
        " trusted-root=\"" +
        (roots.physical_backing_verified ?
             roots.physical_backing_root.string() :
             roots.module_visible_root.string()) +
        "\"";
    if (!location.diagnosis.empty()) {
        description += " reason=" + location.diagnosis;
    }
    return description;
}
}
