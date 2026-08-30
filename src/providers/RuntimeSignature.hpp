#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace mfgdlss::providers
{

[[nodiscard]] bool verify_runtime_signature(
    const std::filesystem::path& path,
    std::wstring_view expected_publisher,
    std::string& failure);
}
