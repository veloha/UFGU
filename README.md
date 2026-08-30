# Universal Frame Generation & Upscaling

Universal Frame Generation & Upscaling, or UFGU, is a native C++20 SKSE
rendering plugin for Skyrim Special Edition. It provides NVIDIA DLSS and DLAA,
AMD FidelityFX upscaling, Intel XeSS, and provider-specific frame generation
through one in-game menu.

UFGU is currently a beta project. Skyrim 1.5.97 and 1.6.1170 are supported as
separate, exact runtime profiles. GPU capabilities still determine which
providers, quality modes, and frame-generation multipliers can be enabled.

## Features

- NVIDIA DLSS, DLAA, Frame Generation, Multi Frame Generation, and Reflex
- AMD FidelityFX upscaling and cross-vendor 2x Frame Generation
- Intel XeSS and XeSS Frame Generation
- Fixed and Dynamic NVIDIA frame-generation cadence
- Native-resolution UI composition after scene upscaling
- Built-in real-frame and final-output FPS controls
- Module-relative vendor runtime loading with path and signature validation
- Native Direct2D, DirectWrite, SKSE, and Scaleform UI with no WinForms, .NET,
  C++/CLI, or helper process

## Supported runtimes

| Skyrim | SKSE | Status |
|---|---:|---|
| 1.5.97 | 2.0.20 | Supported and live tested |
| 1.6.1170 | 2.2.6 | Supported and live tested |
| Skyrim VR | Not applicable | Not supported |

See [compatibility](docs/users/compatibility.md) for GPU, ENB, ReShade, and
mod-conflict boundaries.

## Installation

Install the release archive with Mod Organizer 2 and use its FOMOD. Do not copy
the provider DLLs into Skyrim's game directory. UFGU loads them from its private
`SKSE/Plugins/UFGU` folders.

Launch Skyrim through the SKSE entry for the exact game copy used by the mod
manager. Open UFGU with `PageDown`, choose a supported provider and mode, apply
the settings, and restart when the menu requests it.

Read the [installation guide](docs/users/installation.md) before combining UFGU
with ENB, ReShade, display overrides, or another renderer plugin.

## Frame-generation pacing

For Fixed Frame Generation, set **Base FPS Cap** in UFGU to a rate the game can
maintain in its heaviest areas. A stable real-frame rate matters more than a
high target. Use only one base limiter.

UFGU automatically bypasses its built-in base cap while Dynamic MFG controls
cadence. Disable any external base limiter when using Dynamic MFG.

`[Reflex] FrameLimit` is the final-output cap, not the base cap. Leave it at `0`
unless a specific test requires UFGU to limit final presentation.

### If the image looks smeared or woven while panning

Check whether your final output rate equals your monitor's refresh rate. If it
does, and V-Sync is being forced off by the driver, the frame tears and the tear
line stops drifting, so it sits on screen as a stationary band. It reads as
smearing or a woven pattern in grass and foliage rather than as ordinary
tearing, because a tear is a horizontal seam between two moments and dense
detail shows it most.

On a 240 Hz display a 240 FPS output showed this at 6x, 4x and 3x alike, while
270, 360 and 480 were clean. Either remedy works:

- Move the base cap so the output is not exactly your refresh rate. Final output
  is Base FPS Cap times the multiplier.
- Set Vertical sync to `Use the 3D application setting` in the NVIDIA Control
  Panel. UFGU already presents with V-Sync requested, so this lets that request
  take effect instead of being overridden.

UFGU warns in its log when the output rate equals the reported refresh rate.

## Documentation

- [Documentation index](docs/README.md)
- [Installation](docs/users/installation.md)
- [Configuration](docs/users/configuration.md)
- [Compatibility](docs/users/compatibility.md)
- [Troubleshooting](docs/users/troubleshooting.md)
- [Known issues](docs/users/known-issues.md)
- [Building](docs/developers/building.md)
- [Architecture](docs/developers/architecture.md)
- [Testing](docs/developers/testing.md)
- [Dependencies](docs/developers/dependencies.md)
- [Verification](docs/maintainers/verification.md)
- [Support](SUPPORT.md)
- [Code of conduct](CODE_OF_CONDUCT.md)

## Development status

The verified source builds with MSVC using `/W4 /WX`. The current development
suite has 29 tests. A public-package build adds configuration, signed-runtime,
GPU smoke, and exact-manifest contracts for 32 tests total. The validated FOMOD
package contains 36 reviewed files and zero Markdown files.

Both supported runtime profiles have completed live acceptance on the maintained
test system. Physical GPU coverage and remaining compatibility limits are
recorded in [known issues](docs/users/known-issues.md), while publication checks
are recorded in [verification](docs/maintainers/verification.md).

## Contributing and licence

Read [CONTRIBUTING.md](CONTRIBUTING.md) before submitting a change or report.
General support boundaries are in [SUPPORT.md](SUPPORT.md).
Security issues belong in the private channel described in
[SECURITY.md](SECURITY.md).

Project-owned source is licensed under the Mozilla Public License 2.0. Vendor
SDKs, signed runtimes, and third-party components retain their own terms. See
[licensing](docs/maintainers/licensing.md) and
[provenance](docs/maintainers/provenance.md).
