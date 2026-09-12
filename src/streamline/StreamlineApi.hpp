#pragma once

#include "providers/RuntimeLoader.hpp"

#include <sl.h>

#include <filesystem>
#include <string>

namespace mfgdlss::streamline
{
[[nodiscard]] const std::string& runtime_engine_version();

class Api final
{
public:
    [[nodiscard]] static Api& instance() noexcept;

    [[nodiscard]] bool initialize();
    void shutdown() noexcept;

    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] const std::filesystem::path& runtime_directory() const noexcept;

    [[nodiscard]] sl::Result is_feature_supported(
        sl::Feature feature,
        const sl::AdapterInfo& adapter) const noexcept;
    [[nodiscard]] sl::Result is_feature_loaded(sl::Feature feature, bool& loaded) const noexcept;
    [[nodiscard]] sl::Result feature_requirements(
        sl::Feature feature,
        sl::FeatureRequirements& requirements) const noexcept;
    [[nodiscard]] sl::Result set_d3d_device(void* device) const noexcept;
    [[nodiscard]] sl::Result upgrade_interface(void** interface_pointer) const noexcept;
    [[nodiscard]] sl::Result get_native_interface(
        void* proxy_interface,
        void** native_interface) const noexcept;
    [[nodiscard]] sl::Result get_feature_function(
        sl::Feature feature,
        const char* name,
        void*& function) const noexcept;
    [[nodiscard]] sl::Result get_new_frame_token(
        sl::FrameToken*& token,
        const std::uint32_t* frame_index) const noexcept;
    [[nodiscard]] sl::Result set_tag_for_frame(
        const sl::FrameToken& frame,
        const sl::ViewportHandle& viewport,
        const sl::ResourceTag* tags,
        std::uint32_t tag_count,
        sl::CommandBuffer* command_buffer) const noexcept;
    [[nodiscard]] sl::Result set_constants(
        const sl::Constants& constants,
        const sl::FrameToken& frame,
        const sl::ViewportHandle& viewport) const noexcept;
    [[nodiscard]] sl::Result evaluate_feature(
        sl::Feature feature,
        const sl::FrameToken& frame,
        const sl::BaseStructure** inputs,
        std::uint32_t input_count,
        sl::CommandBuffer* command_buffer) const noexcept;

private:
    [[nodiscard]] bool load_runtime();
    void unload_runtime() noexcept;

    std::filesystem::path runtime_directory_;
    std::filesystem::path log_directory_;
    providers::RuntimeLoader runtime_loader_;

    PFun_slInit* init_{};
    PFun_slShutdown* shutdown_{};
    PFun_slIsFeatureSupported* is_feature_supported_{};
    PFun_slIsFeatureLoaded* is_feature_loaded_{};
    PFun_slGetFeatureRequirements* get_feature_requirements_{};
    PFun_slSetD3DDevice* set_d3d_device_{};
    PFun_slUpgradeInterface* upgrade_interface_{};
    PFun_slGetNativeInterface* get_native_interface_{};
    PFun_slGetFeatureFunction* get_feature_function_{};
    PFun_slGetNewFrameToken* get_new_frame_token_{};
    PFun_slSetTagForFrame* set_tag_for_frame_{};
    PFun_slSetConstants* set_constants_{};
    PFun_slEvaluateFeature* evaluate_feature_{};

    bool initialized_{};
};
}
