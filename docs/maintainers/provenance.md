# Source provenance

This document separates build dependencies from projects consulted during
renderer research. A project listed as a research reference is not compiled,
linked, packaged or redistributed by UFGU.

## Build dependencies

| Component | Version or commit | Licence | Relationship to UFGU |
|---|---|---|---|
| CommonLibSSE-NG | `b93280e832f263dbef44e44cbe2936622a02f91a` plus the patch in `cmake/patches` | MIT | Built as the native Skyrim/SKSE integration library |
| rapidcsv | 8.99 | BSD-3-Clause | Header dependency resolved by vcpkg |
| fmt | 12.2.0 | MIT | Native formatting dependency resolved by vcpkg |
| spdlog | 1.17.0 | MIT | Native logging dependency resolved by vcpkg |
| NVIDIA Streamline and NGX | Streamline 2.12.0 | NVIDIA SDK terms and bundled third-party notices | Headers and signed redistributable runtimes acquired separately from NVIDIA |
| AMD FidelityFX | SDK 2.3.0 | AMD SDK terms and bundled third-party notices | Headers and signed D3D12 redistributable runtimes acquired separately from AMD |
| AMD Anti-Lag 2 | 2.0.0a | MIT | Header-only experimental integration; not advertised as a supported feature |
| Intel XeSS | 3.0.2 | Intel SDK terms and bundled third-party notices | Headers and signed redistributable runtimes acquired separately from Intel |
| Windows SDK and DirectX | Windows SDK 10.0.26100.0 | Microsoft terms | Native Win32, D3D11, D3D12 and DXGI interfaces |
| ENB API | Runtime ABI only | No ENB files redistributed | Optional discovery of documented ENB exports at runtime |

The public repository does not vendor the NVIDIA, AMD, Intel, Microsoft or ENB
SDK trees. The build requires their paths to be supplied explicitly. Release
packaging copies only the required redistributable binaries and their notices,
then validates that every required notice is present.

## External research references

The following public repositories were consulted to understand Skyrim renderer
behaviour, SKSE hook placement, temporal-resource contracts and interoperability:

| Project | Reviewed commit | Licence | Use |
|---|---|---|---|
| Community Shaders | `7ae9bdd62f7abafe7aa8d8bb081a49ac966e6f0a` | GPL-3.0 | External architecture and renderer-behaviour reference |
| Skyrim DRS | `50987d01af5ef1c9f2466144989837a09cf08af0` | MIT | External dynamic-resolution and hook-behaviour reference |
| Skyrim Upscaler | `fa057bb088cf399e1112c1eaba714590c881e462` | MIT | External hook-placement and runtime-integration reference |
| ENB Frame Generation | `7c4e5988e38a538a391fdc4626dcb6e4c5589022` | GPL-3.0 | External ENB interoperability reference |
| ENB Anti-Aliasing | `4a527826e73a08ae57da9e5f38244bc9bcded3e4` | GPL-3.0 | External ENB interoperability reference |
| NVIDIA Streamline | `e8aaa6eaac968711fb62473d4ae8256dde20919b` | NVIDIA repository terms | Official SDK implementation and API reference |
| NVIDIA Streamline Sample | `dd6e1803b97e5b3f218f5776d85f09e4c386c34f` | NVIDIA repository terms | Official integration sample |

No source file or binary from the listed external research projects is a UFGU
dependency. None is linked or included in a UFGU package.

## Project artwork

`fomod/images/header.png` is UFGU installer artwork maintained with the project
and distributed under the repository's MPL-2.0 licence. It contains no embedded
author, software, location or comment metadata.

## Verification performed for this source tree

The provenance review applies to every project-owned C and C++ source, header,
test, CMake input, and repository document. It includes:

- case-insensitive filename and content scans for unrelated project names,
  archive names, and private-build markers;
- normalized implementation-block comparisons against each public research
  repository;
- manual review of automated matches and dependency boundaries;
- CMake reachability analysis for production and test sources;
- clean MSVC `/W4 /WX` builds and complete native contract-test runs;
- exact public-package allowlisting, licence checks, and signed-runtime checks.

The active project-owned source, embedded HLSL, CMake, and repository
configuration inputs contain code or configuration only. Maintainer rationale
is consolidated into focused architecture, testing, release, licensing, and
verification documents rather than generated comment archives.

The current dual-runtime build completed 29 development tests. Its separate
public configuration completed 32 tests and produced an exact 36-file package
with zero Markdown files. Skyrim 1.5.97 and 1.6.1170 use separate exact runtime
profiles and have completed live acceptance on the maintained test system.

Comparisons against the listed public research projects produced no retained
project-specific implementation blocks. Short matches against official vendor
samples were ordinary DirectX API boilerplate, such as resource-transition
descriptors and adjacent DXGI format cases.

Current repository, build, package, and live-test evidence is summarized in
[verification](verification.md). Point-in-time local assessment output is kept
with private release records instead of being presented as an independent
public audit.

This is an engineering provenance record, not a legal opinion. Questions about
a specific file or implementation block should identify the file and lines so
they can be reviewed directly.
