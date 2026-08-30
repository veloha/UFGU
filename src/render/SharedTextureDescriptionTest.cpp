#include "render/SharedTextureDescription.hpp"

#include <cstdio>

int main()
{
    using namespace mfgdlss::render;

    int failures{};
    const auto check = [&failures](const bool condition, const char* name) {
        std::printf("%s  %s\n", condition ? "PASS" : "FAIL", name);
        failures += condition ? 0 : 1;
    };

    D3D11_TEXTURE2D_DESC source{};
    source.Width = 3840;
    source.Height = 2160;
    source.MipLevels = 1;
    source.ArraySize = 1;
    source.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    source.SampleDesc = {1, 0};
    source.Usage = D3D11_USAGE_STAGING;
    source.BindFlags = D3D11_BIND_RENDER_TARGET;
    source.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    source.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    check(valid_shared_texture_source(source), "valid single-surface source");

    constexpr UINT bind_flags = D3D11_BIND_SHADER_RESOURCE;
    const auto shared =
        make_shared_texture_description(source, bind_flags);
    check(shared.Width == 3840 && shared.Height == 2160,
          "extent is preserved");
    check(shared.Format == DXGI_FORMAT_R8G8B8A8_UNORM,
          "format is preserved");
    check(shared.MipLevels == 1 && shared.ArraySize == 1 &&
              shared.SampleDesc.Count == 1 &&
              shared.SampleDesc.Quality == 0,
          "single-surface topology is explicit");
    check(shared.Usage == D3D11_USAGE_DEFAULT &&
              shared.CPUAccessFlags == 0,
          "staging and CPU flags are discarded");
    check(shared.BindFlags == D3D11_BIND_SHADER_RESOURCE,
          "only requested bind flags survive");
    check(shared.MiscFlags ==
              (D3D11_RESOURCE_MISC_SHARED |
               D3D11_RESOURCE_MISC_SHARED_NTHANDLE),
          "cross-API sharing flags are explicit");

    source.SampleDesc.Count = 4;
    check(!valid_shared_texture_source(source),
          "multisampled source is rejected");
    source.SampleDesc = {1, 0};
    source.MipLevels = 2;
    check(!valid_shared_texture_source(source),
          "multi-mip source is rejected");
    source.MipLevels = 1;
    source.Format = DXGI_FORMAT_UNKNOWN;
    check(!valid_shared_texture_source(source),
          "unknown format is rejected");

    return failures == 0 ? 0 : 1;
}
