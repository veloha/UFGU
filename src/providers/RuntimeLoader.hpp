#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace mfgdlss::providers
{

class RuntimeLoader final
{
public:
    using AdditionalVerifier = bool (*)(
        const std::filesystem::path&) noexcept;

    RuntimeLoader() = default;
    RuntimeLoader(const RuntimeLoader&) = delete;
    RuntimeLoader& operator=(const RuntimeLoader&) = delete;
    RuntimeLoader(RuntimeLoader&&) = delete;
    RuntimeLoader& operator=(RuntimeLoader&&) = delete;
    ~RuntimeLoader();

    [[nodiscard]] bool load(
        const std::filesystem::path& subdirectory,
        const std::wstring& file_name,
        bool require_valid_signature,
        std::wstring_view expected_publisher,
        AdditionalVerifier additional_verifier = nullptr);

    void unload() noexcept;

    [[nodiscard]] bool loaded() const noexcept;
    [[nodiscard]] void* module() const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

    [[nodiscard]] const std::string& failure() const noexcept;

    [[nodiscard]] void* symbol(const char* name) const noexcept;

    template <typename Function>
    [[nodiscard]] Function function(const char* name) const noexcept
    {
        return reinterpret_cast<Function>(symbol(name));
    }

    [[nodiscard]] static std::filesystem::path plugin_directory();

    [[nodiscard]] static std::filesystem::path physical_plugin_directory();

private:
    std::filesystem::path path_;
    std::string failure_;
    void* module_{};
};
}
