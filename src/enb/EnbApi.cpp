#include "enb/EnbApi.hpp"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <array>

namespace mfgdlss::enb
{
namespace
{
template <class Function>
[[nodiscard]] Function load_export(const HMODULE module, const char* name) noexcept
{
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

[[nodiscard]] HMODULE find_enb_module() noexcept
{
    std::array<HMODULE, 1024> modules{};
    DWORD bytes_needed{};
    if (!EnumProcessModules(
            GetCurrentProcess(),
            modules.data(),
            static_cast<DWORD>(sizeof(modules)),
            &bytes_needed)) {
        return nullptr;
    }

    const auto count = (std::min)(modules.size(), bytes_needed / sizeof(HMODULE));
    for (std::size_t index = 0; index < count; ++index) {
        if (GetProcAddress(modules[index], "ENBGetSDKVersion") != nullptr) {
            return modules[index];
        }
    }
    return nullptr;
}
}

Api& Api::instance() noexcept
{
    static Api api;
    return api;
}

bool Api::connect() noexcept
{
    if (connected()) {
        return true;
    }

    module_ = find_enb_module();
    if (module_ == nullptr) {
        return false;
    }

    const auto module = static_cast<HMODULE>(module_);
    get_sdk_version_ = load_export<GetVersion>(module, "ENBGetSDKVersion");
    get_enb_version_ = load_export<GetVersion>(module, "ENBGetVersion");
    set_callback_ = load_export<SetCallback>(module, "ENBSetCallbackFunction");
    get_render_info_ = load_export<GetRenderInfo>(module, "ENBGetRenderInfo");

    if (!connected() || sdk_version() / 1000 != 1) {
        disconnect();
        return false;
    }
    return true;
}

void Api::disconnect() noexcept
{
    module_ = nullptr;
    get_sdk_version_ = nullptr;
    get_enb_version_ = nullptr;
    set_callback_ = nullptr;
    get_render_info_ = nullptr;
}

bool Api::connected() const noexcept
{
    return module_ != nullptr &&
           get_sdk_version_ != nullptr &&
           get_enb_version_ != nullptr &&
           set_callback_ != nullptr &&
           get_render_info_ != nullptr;
}

long Api::sdk_version() const noexcept
{
    return get_sdk_version_ != nullptr ? get_sdk_version_() : 0;
}

long Api::enb_version() const noexcept
{
    return get_enb_version_ != nullptr ? get_enb_version_() : 0;
}

RenderInfo* Api::render_info() const noexcept
{
    return get_render_info_ != nullptr ? get_render_info_() : nullptr;
}

bool Api::set_callback(const CallbackFunction callback) const noexcept
{
    if (set_callback_ == nullptr) {
        return false;
    }
    set_callback_(callback);
    return true;
}
}
