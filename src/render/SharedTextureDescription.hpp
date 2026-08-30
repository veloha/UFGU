#pragma once

#include <d3d11.h>

namespace mfgdlss::render
{

[[nodiscard]] constexpr D3D11_TEXTURE2D_DESC
make_shared_texture_description(
    const D3D11_TEXTURE2D_DESC& source,
    const UINT bind_flags) noexcept
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = source.Width;
    description.Height = source.Height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = source.Format;
    description.SampleDesc = {1, 0};
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = bind_flags;
    description.CPUAccessFlags = 0;
    description.MiscFlags =
        D3D11_RESOURCE_MISC_SHARED |
        D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    return description;
}

[[nodiscard]] constexpr bool valid_shared_texture_source(
    const D3D11_TEXTURE2D_DESC& source) noexcept
{
    return source.Width != 0 && source.Height != 0 &&
           source.MipLevels == 1 && source.ArraySize == 1 &&
           source.SampleDesc.Count == 1 &&
           source.SampleDesc.Quality == 0 &&
           source.Format != DXGI_FORMAT_UNKNOWN;
}
}
