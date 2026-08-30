#include "providers/RuntimeLoader.hpp"
#include "providers/ModuleIdentity.hpp"
#include "providers/RuntimePath.hpp"
#include "providers/RuntimeSignature.hpp"

#include <Windows.h>

#include <SKSE/SKSE.h>

#include <atomic>
#include <system_error>

namespace mfgdlss::providers
{
namespace
{
namespace logger = SKSE::log;

void log_trusted_runtime_roots_once()
{
    static std::atomic<bool> reported{false};
    if (reported.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    logger::info("{}", describe_trusted_runtime_roots());
}
}

RuntimeLoader::~RuntimeLoader()
{
    unload();
}

std::filesystem::path RuntimeLoader::plugin_directory()
{
    return this_module_location().module_visible_path.parent_path();
}

std::filesystem::path RuntimeLoader::physical_plugin_directory()
{
    const auto& roots = trusted_runtime_roots();
    return roots.physical_backing_verified ? roots.physical_backing_root :
                                             std::filesystem::path{};
}

bool RuntimeLoader::load(
    const std::filesystem::path& subdirectory,
    const std::wstring& file_name,
    const bool require_valid_signature,
    const std::wstring_view expected_publisher,
    const AdditionalVerifier additional_verifier)
{
    if (module_ != nullptr) {
        return true;
    }
    failure_.clear();
    path_.clear();

    if (file_name.empty() ||
        file_name.find(L'\\') != std::wstring::npos ||
        file_name.find(L'/') != std::wstring::npos ||
        file_name.find(L':') != std::wstring::npos) {
        failure_ = "rejected: the runtime name is not a plain file name";
        return false;
    }

    log_trusted_runtime_roots_once();

    const auto base = plugin_directory().lexically_normal();
    if (base.empty()) {
        failure_ = "the plugin directory could not be resolved";
        return false;
    }

    std::error_code code;
    auto candidate = base / subdirectory / file_name;
    candidate = candidate.lexically_normal();

    if (!is_strict_descendant(base, candidate)) {
        failure_ = "rejected: the resolved runtime path escaped the plugin directory";
        return false;
    }
    const auto requested = candidate;

    if (!std::filesystem::is_regular_file(candidate, code)) {
        failure_ = "file not present";
        path_ = std::move(candidate);
        return false;
    }

    code.clear();
    auto resolved = std::filesystem::canonical(candidate, code);
    if (code) {
        failure_ = "rejected: the physical runtime path could not be resolved";
        path_ = std::move(candidate);
        return false;
    }
    resolved = std::filesystem::path{
        strip_extended_length_prefix(resolved.wstring())};

    const auto verdict =
        classify_runtime_containment(trusted_runtime_roots(), resolved);
    if (!is_accepted(verdict)) {
        failure_ = std::string{"rejected: "} + describe(verdict);
        path_ = std::move(resolved);
        return false;
    }
    candidate = std::move(resolved);

    if (require_valid_signature) {
        std::string signature_failure;
        if (!verify_runtime_signature(
                candidate, expected_publisher, signature_failure)) {
            failure_ = "signature verification failed: " + signature_failure;
            path_ = std::move(candidate);
            return false;
        }
    }

    if (additional_verifier != nullptr &&
        !additional_verifier(candidate)) {
        failure_ = "vendor-specific embedded-signature verification failed";
        path_ = std::move(candidate);
        return false;
    }

    const auto module = LoadLibraryExW(
        candidate.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
        failure_ = "Win32 error " + std::to_string(GetLastError());
        path_ = std::move(candidate);
        return false;
    }

    module_ = module;
    path_ = std::move(candidate);

    if (requested == path_) {
        logger::info("Optional runtime loaded: {}", path_.string());
    } else {
        logger::info(
            "Optional runtime loaded: requested=\"{}\" physical=\"{}\" ({})",
            requested.string(),
            path_.string(),
            describe(verdict));
    }
    return true;
}

void RuntimeLoader::unload() noexcept
{
    if (module_ != nullptr) {
        static_cast<void>(FreeLibrary(static_cast<HMODULE>(module_)));
        module_ = nullptr;
    }
}

bool RuntimeLoader::loaded() const noexcept
{
    return module_ != nullptr;
}

void* RuntimeLoader::module() const noexcept
{
    return module_;
}

const std::filesystem::path& RuntimeLoader::path() const noexcept
{
    return path_;
}

const std::string& RuntimeLoader::failure() const noexcept
{
    return failure_;
}

void* RuntimeLoader::symbol(const char* const name) const noexcept
{
    if (module_ == nullptr || name == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<void*>(
        GetProcAddress(static_cast<HMODULE>(module_), name));
}
}
