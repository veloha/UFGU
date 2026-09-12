#pragma once

#include "render/RuntimeCompatibility.hpp"

#include <REL/Relocation.h>

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

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

[[nodiscard]] inline bool committed_code_allows(
    const std::uintptr_t address,
    const std::size_t size,
    const bool require_execute) noexcept
{
    if (address == 0U || size == 0U) {
        return false;
    }
    MEMORY_BASIC_INFORMATION information{};
    if (VirtualQuery(
            reinterpret_cast<LPCVOID>(address),
            &information,
            sizeof(information)) != sizeof(information) ||
        information.State != MEM_COMMIT ||
        (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const auto region_begin =
        reinterpret_cast<std::uintptr_t>(information.BaseAddress);
    const auto region_end = region_begin + information.RegionSize;
    if (!range_contains(region_begin, region_end, address, size)) {
        return false;
    }
    const auto protection = information.Protect & 0xFFU;
    const auto executable = protection == PAGE_EXECUTE_READ ||
                            protection == PAGE_EXECUTE_READWRITE ||
                            protection == PAGE_EXECUTE_WRITECOPY;
    const auto readable = executable || protection == PAGE_READONLY ||
                          protection == PAGE_READWRITE ||
                          protection == PAGE_WRITECOPY;
    return require_execute ? executable : readable;
}

[[nodiscard]] inline std::uintptr_t loaded_image_end(
    const std::uintptr_t base) noexcept
{
    if (!committed_code_allows(base, sizeof(IMAGE_DOS_HEADER), false)) {
        return 0U;
    }
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return 0U;
    }
    const auto nt_address = base + static_cast<std::uintptr_t>(dos->e_lfanew);
    if (!committed_code_allows(nt_address, sizeof(IMAGE_NT_HEADERS64), false)) {
        return 0U;
    }
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(nt_address);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0U;
    }
    return base + static_cast<std::uintptr_t>(nt->OptionalHeader.SizeOfImage);
}

[[nodiscard]] inline HookValidationResult preflight_direct_call(
    const std::uintptr_t call_address,
    const CodeSignature& signature) noexcept
{
    const auto module_begin = REL::Module::get().base();
    const auto module_end = loaded_image_end(module_begin);
    const auto window_start = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(call_address) + signature.relative_offset);
    const auto call_readable = module_end != 0U &&
        committed_code_allows(call_address, 5U, false) &&
        (signature.length == 0U ||
         committed_code_allows(window_start, signature.length, false));
    if (!call_readable) {
        return {HookValidationFailure::unreadable, 0U};
    }
    const auto call_executable = committed_code_allows(call_address, 5U, true);
    const std::span<const std::uint8_t> call_bytes(
        reinterpret_cast<const std::uint8_t*>(call_address), 5U);
    const std::span<const std::uint8_t> signature_bytes(
        reinterpret_cast<const std::uint8_t*>(window_start), signature.length);

    auto target_executable = false;
    if (call_bytes[0] == 0xE8U) {
        std::int32_t displacement{};
        std::memcpy(&displacement, call_bytes.data() + 1, sizeof(displacement));
        const auto target = static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(call_address) + 5 + displacement);
        target_executable = committed_code_allows(target, 1U, true);
    }

    return validate_direct_call(
        call_bytes,
        signature_bytes,
        call_address,
        module_begin,
        module_end,
        call_readable,
        call_executable,
        target_executable,
        signature);
}
}
