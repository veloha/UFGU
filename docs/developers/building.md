# Building UFGU

UFGU is a native Windows C++20 SKSE plugin. Use a fresh build directory so a
previous candidate cannot contribute stale objects or package files.

## Pinned inputs

- CMake 4.3.1-msvc1 or a newer compatible release
- MSVC 19.51.36252 with toolset 14.51.36231
- Windows SDK 10.0.26100.0
- vcpkg commit `99e82d9c9f0b281ec11fba48cc8434574a2b6e66`
- vcpkg triplet `x64-windows-static-md`
- CommonLibSSE-NG commit `b93280e832f263dbef44e44cbe2936622a02f91a`
- NVIDIA Streamline SDK 2.12.0
- AMD FidelityFX SDK 2.3.0
- AMD Anti-Lag 2 SDK 2.0.0a headers
- Intel XeSS SDK 3.0.2

The vcpkg manifest resolves rapidcsv 8.99, fmt 12.2.0, and spdlog 1.17.0 from
the pinned baseline.

Apply
`cmake/patches/CommonLibSSE-NG-b93280-compatible-runtimes.patch` to the exact
CommonLibSSE-NG checkout. The patch corrects its compatible-runtime list length
calculation. A clean checkout plus that reviewed patch is the supported state.

Vendor SDKs and runtimes are not committed. Download them from their vendors,
accept their terms, and provide their roots explicitly.

## Configure the environment

`CMakePresets.json` keeps the development and public configurations consistent.
Set these environment variables before using it:

```powershell
$env:VCPKG_ROOT = "C:\path\to\vcpkg"
$env:UFGU_COMMONLIBSSE_NG_ROOT = "C:\path\to\patched-commonlibsse-ng"
$env:UFGU_STREAMLINE_SDK_ROOT = "C:\path\to\streamline-2.12.0"
$env:UFGU_AMD_FIDELITYFX_ROOT = "C:\path\to\fidelityfx-sdk-2.3.0"
$env:UFGU_AMD_ANTILAG_ROOT = "C:\path\to\amd-antilag2-2.0.0a"
$env:UFGU_INTEL_XESS_ROOT = "C:\path\to\xess-3.0.2"
```

The FidelityFX root is the directory that contains `Kits/FidelityFX`; it is
not the `Kits/FidelityFX` directory itself.

## Development build

```powershell
cmake --preset dev
cmake --build --preset dev-release --parallel
ctest --preset dev-release
```

The expected result is 29 passing tests. To run MSVC native code analysis over
project-owned plugin translation units, configure a separate development tree
with `MFG_DLSS_ENABLE_MSVC_CODE_ANALYSIS=ON`. Record the compiler version,
analysis settings, result, and source commit with the release evidence.

## Public package build

```powershell
cmake --preset public
cmake --build --preset public-release --parallel
ctest --preset public-release
cmake -DMFG_DLSS_PACKAGE_DIR="<source>\out\build\public\package" `
  -P "<source>\cmake\ValidatePublicPackage.cmake"
```

The expected result is 32 passing tests and an exact 36-file package containing
zero Markdown files. Public mode rejects NVIDIA's sample application identity,
requires the project licence and vendor notices, validates signed provider
runtimes, refuses unexpected package contents, and writes a sorted SHA-256
manifest beside the package directory.

## Reproducibility

Build twice from empty directories with identical pinned inputs. Hash both
DLLs and compare sorted package-file SHA-256 manifests. Record the compiler,
toolset, Windows SDK, CMake, dependency commits, source commit, and vendor SDK
versions with the release. MSVC compilation uses deterministic path mapping and
linking uses `/Brepro` so PE timestamp fields do not vary between equivalent
builds. Two copies of one DLL are not independent build evidence.
