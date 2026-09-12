#include "streamline/StreamlineApi.hpp"

#include "streamline/ProjectIdentity.hpp"
#include "Version.hpp"

#include <sl_security.h>

#include <Windows.h>

#include <SKSE/SKSE.h>

#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace mfgdlss::streamline
{
namespace
{
namespace logger = SKSE::log;

#ifndef MFG_DLSS_STREAMLINE_APPLICATION_ID
#error MFG_DLSS_STREAMLINE_APPLICATION_ID must be supplied by CMake.
#endif

constexpr std::uint32_t kNvidiaSampleApplicationId = 231313132;

constexpr std::uint32_t kApplicationId = MFG_DLSS_STREAMLINE_APPLICATION_ID;

[[nodiscard]] std::string_view result_name(const sl::Result result) noexcept
{
    switch (result) {
    case sl::Result::eOk:
        return "ok";
    case sl::Result::eErrorDriverOutOfDate:
        return "driver-out-of-date";
    case sl::Result::eErrorOSOutOfDate:
        return "os-out-of-date";
    case sl::Result::eErrorOSDisabledHWS:
        return "hardware-scheduling-disabled";
    case sl::Result::eErrorDeviceNotCreated:
        return "device-not-created";
    case sl::Result::eErrorNoSupportedAdapterFound:
        return "no-supported-adapter";
    case sl::Result::eErrorAdapterNotSupported:
        return "adapter-not-supported";
    case sl::Result::eErrorNoPlugins:
        return "no-plugins";
    case sl::Result::eErrorInvalidIntegration:
        return "invalid-integration";
    case sl::Result::eErrorInvalidParameter:
        return "invalid-parameter";
    case sl::Result::eErrorFeatureMissing:
        return "feature-missing";
    case sl::Result::eErrorFeatureNotSupported:
        return "feature-not-supported";
    case sl::Result::eErrorFeatureFailedToLoad:
        return "feature-failed-to-load";
    case sl::Result::eErrorFeatureMissingDependency:
        return "feature-missing-dependency";
    default:
        return "other-error";
    }
}

void streamline_log(const sl::LogType type, const char* message)
{
    if (message == nullptr) {
        return;
    }
    std::string_view text{message};
    while (!text.empty() &&
           (text.back() == '\n' || text.back() == '\r' ||
            text.back() == '\t' || text.back() == ' ')) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return;
    }

    switch (type) {
    case sl::LogType::eError:
        logger::error("[Streamline] {}", text);
        break;
    case sl::LogType::eWarn:
        logger::warn("[Streamline] {}", text);
        break;
    default:
        logger::info("[Streamline] {}", text);
        break;
    }
}

[[nodiscard]] bool verify_nvidia_embedded_signature(
    const std::filesystem::path& path) noexcept
{

    return sl::security::verifyEmbeddedSignature(path.c_str());
}

}

const std::string& runtime_engine_version()
{
    static const std::string value = [] {
        const auto version = REL::Module::get().version();
        return engine_version_string(
            version.major(),
            version.minor(),
            version.patch(),
            version.build(),
            kProjectVersion);
    }();
    return value;
}

Api& Api::instance() noexcept
{
    static Api api;
    return api;
}

bool Api::initialize()
{
    if (initialized_) {
        return true;
    }
    if (!load_runtime()) {
        return false;
    }

    const wchar_t* plugin_paths[]{runtime_directory_.c_str()};
    constexpr std::array features{
        sl::kFeatureDLSS,
        sl::kFeatureDLSS_G,
        sl::kFeatureReflex,
        sl::kFeaturePCL
    };

    sl::Preferences preferences{};
    preferences.showConsole = false;
    preferences.logLevel = sl::LogLevel::eDefault;
    preferences.pathsToPlugins = plugin_paths;
    preferences.numPathsToPlugins = static_cast<std::uint32_t>(std::size(plugin_paths));
    preferences.pathToLogsAndData = log_directory_.c_str();
    preferences.logMessageCallback = streamline_log;
    preferences.flags =
        sl::PreferenceFlags::eDisableCLStateTracking |
        sl::PreferenceFlags::eDisableDebugText |
        sl::PreferenceFlags::eUseManualHooking |
        sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    preferences.featuresToLoad = features.data();
    preferences.numFeaturesToLoad = static_cast<std::uint32_t>(features.size());

    if constexpr (kApplicationId != 0) {
        preferences.applicationId = kApplicationId;
    }
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = runtime_engine_version().c_str();
    preferences.projectId = kProjectId.data();
    preferences.renderAPI = sl::RenderAPI::eD3D12;

    const auto result = init_(preferences, sl::kSDKVersion);
    if (result != sl::Result::eOk) {
        logger::error(
            "slInit failed: {} ({})",
            result_name(result),
            static_cast<int>(result));
        unload_runtime();
        return false;
    }

    initialized_ = true;
    logger::info(
        "Streamline {}.{}.{} initialized in manual-hooking mode",
        SL_VERSION_MAJOR,
        SL_VERSION_MINOR,
        SL_VERSION_PATCH);
    if constexpr (kApplicationId == 0) {
        logger::info(
            "Streamline identity: project {}, engine eCustom \"{}\" (no "
            "NVIDIA-issued application ID, which is the supported alternative "
            "and avoids inheriting another title's driver profile)",
            kProjectId,
            runtime_engine_version());
    } else {
        logger::info("Streamline application ID: {}", kApplicationId);
    }
    if constexpr (kApplicationId == kNvidiaSampleApplicationId) {
        logger::warn(
            "NVIDIA sample application ID is active; this build is for local development only");
    }
    return true;
}

void Api::shutdown() noexcept
{
    if (initialized_ && shutdown_ != nullptr) {
        const auto result = shutdown_();
        if (result != sl::Result::eOk) {
            logger::error(
                "slShutdown failed: {} ({})",
                result_name(result),
                static_cast<int>(result));
        }
    }
    initialized_ = false;
    unload_runtime();
}

bool Api::initialized() const noexcept
{
    return initialized_;
}

const std::filesystem::path& Api::runtime_directory() const noexcept
{
    return runtime_directory_;
}

sl::Result Api::is_feature_supported(
    const sl::Feature feature,
    const sl::AdapterInfo& adapter) const noexcept
{
    return is_feature_supported_ != nullptr ?
               is_feature_supported_(feature, adapter) :
               sl::Result::eErrorNotInitialized;
}

sl::Result Api::is_feature_loaded(const sl::Feature feature, bool& loaded) const noexcept
{
    return is_feature_loaded_ != nullptr ?
               is_feature_loaded_(feature, loaded) :
               sl::Result::eErrorNotInitialized;
}

sl::Result Api::feature_requirements(
    const sl::Feature feature,
    sl::FeatureRequirements& requirements) const noexcept
{
    return get_feature_requirements_ != nullptr ?
               get_feature_requirements_(feature, requirements) :
               sl::Result::eErrorNotInitialized;
}

sl::Result Api::set_d3d_device(void* device) const noexcept
{
    return set_d3d_device_ != nullptr ?
               set_d3d_device_(device) :
               sl::Result::eErrorNotInitialized;
}

sl::Result Api::upgrade_interface(void** interface_pointer) const noexcept
{
    return upgrade_interface_ != nullptr ?
               upgrade_interface_(interface_pointer) :
               sl::Result::eErrorNotInitialized;
}

sl::Result Api::get_native_interface(
    void* proxy_interface,
    void** native_interface) const noexcept
{
    return get_native_interface_ != nullptr ?
               get_native_interface_(proxy_interface, native_interface) :
               sl::Result::eErrorNotInitialized;
}

sl::Result Api::get_feature_function(
    const sl::Feature feature,
    const char* name,
    void*& function) const noexcept
{
    return get_feature_function_ != nullptr ?
               get_feature_function_(feature, name, function) :
               sl::Result::eErrorNotInitialized;
}

sl::Result Api::get_new_frame_token(
    sl::FrameToken*& token,
    const std::uint32_t* frame_index) const noexcept
{
    return get_new_frame_token_ != nullptr ?
               get_new_frame_token_(token, frame_index) :
               sl::Result::eErrorNotInitialized;
}

sl::Result Api::set_tag_for_frame(
    const sl::FrameToken& frame,
    const sl::ViewportHandle& viewport,
    const sl::ResourceTag* tags,
    const std::uint32_t tag_count,
    sl::CommandBuffer* command_buffer) const noexcept
{
    return set_tag_for_frame_ != nullptr ?
               set_tag_for_frame_(
                   frame,
                   viewport,
                   tags,
                   tag_count,
                   command_buffer) :
               sl::Result::eErrorNotInitialized;
}

sl::Result Api::set_constants(
    const sl::Constants& constants,
    const sl::FrameToken& frame,
    const sl::ViewportHandle& viewport) const noexcept
{
    return set_constants_ != nullptr ?
               set_constants_(constants, frame, viewport) :
               sl::Result::eErrorNotInitialized;
}

sl::Result Api::evaluate_feature(
    const sl::Feature feature,
    const sl::FrameToken& frame,
    const sl::BaseStructure** inputs,
    const std::uint32_t input_count,
    sl::CommandBuffer* command_buffer) const noexcept
{
    return evaluate_feature_ != nullptr ?
               evaluate_feature_(
                   feature,
                   frame,
                   inputs,
                   input_count,
                   command_buffer) :
               sl::Result::eErrorNotInitialized;
}

bool Api::load_runtime()
{
    if (!runtime_loader_.load(
            std::filesystem::path{L"UFGU"} / L"Streamline",
            L"sl.interposer.dll",
            true,
            L"NVIDIA Corporation",
            verify_nvidia_embedded_signature)) {
        logger::error(
            "Unable to load the contained NVIDIA Streamline interposer: {}",
            runtime_loader_.failure());
        return false;
    }

    runtime_directory_ = runtime_loader_.path().parent_path();
    {
        std::error_code inventory_error{};
        const auto imgui_path = runtime_directory_ / L"sl.imgui.dll";
        const auto development_marker =
            std::filesystem::exists(imgui_path, inventory_error);
        std::wstring sizes;
        for (const auto* const name : {
                 L"nvngx_dlssg.dll",
                 L"sl.dlss_g.dll",
                 L"sl.common.dll",
                 L"sl.interposer.dll"}) {
            const auto entry = runtime_directory_ / name;
            std::error_code size_error{};
            const auto bytes =
                std::filesystem::file_size(entry, size_error);
            if (!sizes.empty()) {
                sizes += L", ";
            }
            sizes += name;
            sizes += L"=";
            sizes += size_error ?
                std::wstring{L"missing"} : std::to_wstring(bytes);
        }
        const auto narrow = [](const std::wstring& wide) {
            std::string out;
            out.reserve(wide.size());
            for (const auto character : wide) {
                out.push_back(static_cast<char>(character));
            }
            return out;
        };
        if (development_marker) {
            logger::warn(
                "DEVELOPMENT STREAMLINE RUNTIME DEPLOYED. sl.imgui.dll is "
                "present, and it ships ONLY in the SDK's development "
                "bin. A development build is instrumented and slower "
                "than production, so no frame timing from this session "
                "should be compared against a production one. Replace "
                "the Streamline DLLs from the SDK's bin/x64 rather than "
                "bin/x64/development. Deployed sizes: {}",
                narrow(sizes));
        } else {
            logger::info(
                "Streamline runtime inventory, production build with no "
                "sl.imgui.dll present: {}",
                narrow(sizes));
        }
    }
    log_directory_ = SKSE::log::log_directory().value_or(
        providers::RuntimeLoader::plugin_directory());

    init_ = runtime_loader_.function<PFun_slInit*>("slInit");
    shutdown_ = runtime_loader_.function<PFun_slShutdown*>("slShutdown");
    is_feature_supported_ =
        runtime_loader_.function<PFun_slIsFeatureSupported*>(
            "slIsFeatureSupported");
    is_feature_loaded_ =
        runtime_loader_.function<PFun_slIsFeatureLoaded*>(
            "slIsFeatureLoaded");
    get_feature_requirements_ =
        runtime_loader_.function<PFun_slGetFeatureRequirements*>(
            "slGetFeatureRequirements");
    set_d3d_device_ =
        runtime_loader_.function<PFun_slSetD3DDevice*>("slSetD3DDevice");
    upgrade_interface_ =
        runtime_loader_.function<PFun_slUpgradeInterface*>(
            "slUpgradeInterface");
    get_native_interface_ =
        runtime_loader_.function<PFun_slGetNativeInterface*>(
            "slGetNativeInterface");
    get_feature_function_ =
        runtime_loader_.function<PFun_slGetFeatureFunction*>(
            "slGetFeatureFunction");
    get_new_frame_token_ =
        runtime_loader_.function<PFun_slGetNewFrameToken*>(
            "slGetNewFrameToken");
    set_tag_for_frame_ =
        runtime_loader_.function<PFun_slSetTagForFrame*>(
            "slSetTagForFrame");
    set_constants_ =
        runtime_loader_.function<PFun_slSetConstants*>("slSetConstants");
    evaluate_feature_ =
        runtime_loader_.function<PFun_slEvaluateFeature*>(
            "slEvaluateFeature");

    if (init_ == nullptr ||
        shutdown_ == nullptr ||
        is_feature_supported_ == nullptr ||
        is_feature_loaded_ == nullptr ||
        get_feature_requirements_ == nullptr ||
        set_d3d_device_ == nullptr ||
        upgrade_interface_ == nullptr ||
        get_native_interface_ == nullptr ||
        get_feature_function_ == nullptr ||
        get_new_frame_token_ == nullptr ||
        set_tag_for_frame_ == nullptr ||
        set_constants_ == nullptr ||
        evaluate_feature_ == nullptr) {
        logger::error("Streamline interposer is missing one or more required exports");
        unload_runtime();
        return false;
    }
    return true;
}

void Api::unload_runtime() noexcept
{
    init_ = nullptr;
    shutdown_ = nullptr;
    is_feature_supported_ = nullptr;
    is_feature_loaded_ = nullptr;
    get_feature_requirements_ = nullptr;
    set_d3d_device_ = nullptr;
    upgrade_interface_ = nullptr;
    get_native_interface_ = nullptr;
    get_feature_function_ = nullptr;
    get_new_frame_token_ = nullptr;
    set_tag_for_frame_ = nullptr;
    set_constants_ = nullptr;
    evaluate_feature_ = nullptr;

    runtime_loader_.unload();
    runtime_directory_.clear();
}
}
