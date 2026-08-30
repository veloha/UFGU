# Release process

Release binaries are built from a reviewed source commit in a fresh public
configuration. Source and the binary FOMOD are separate artifacts.

## Package contract

The validated package contains 42 allowlisted files under these roots:

```text
fomod/
presets/
Licenses/
SKSE/Plugins/
```

The FOMOD installs the same binaries for every selection. Its graphics-card and
image-quality choices together install a generated preset over
`SKSE/Plugins/UFGU.ini` through `conditionalFileInstalls`, keyed on the
`UFGU_Vendor` and `UFGU_Upscaling` flags. Nine presets are generated from
`config/UFGU.ini`, one per card and upscaling mode; do not maintain them by hand.

### The anchored Mode substitution

`GenerateProviderPreset.cmake` rewrites the `[Upscaling]` `Mode` line by matching
the comment that precedes it, not the bare key. `Mode=Off` appears under both
`[Upscaling]` and `[Reflex]`, so replacing the key alone would switch Reflex on in
every generated preset as a side effect. The comment used as the anchor is:

```text
; Provider or mode changes require a restart when requested by the menu.
```

Rewording that line in `config/UFGU.ini` breaks preset generation. The script
fails with an explicit error rather than producing wrong presets, so the build
stops instead of shipping them.

### Why no preset ships with upscaling off by default

The plugin hands anti-aliasing to the upscaler and holds Skyrim's own image-space
TAA state false for the whole frame. With `Mode=Off` nothing performs the temporal
resolve, so alpha-tested foliage breaks up. The installer therefore asks for an
upscaling mode and recommends the balanced one. `Off` remains selectable for
frame generation on its own, and both the installer and the release notes state
that the image will look worse than with the mod uninstalled.

The package contains zero Markdown files. Required notices whose vendor source
uses a `.md` extension are preserved as `.txt` files. The exact-manifest
validator rejects every missing or unexpected file.

## Build and verify

Follow [building](../developers/building.md), then run:

```powershell
cmake --preset public
cmake --build --preset public-release --parallel
ctest --preset public-release
cmake -DMFG_DLSS_PACKAGE_DIR="<source>\out\build\public\package" `
  -P "<source>\cmake\ValidatePublicPackage.cmake"
```

Require 32 passing tests, a successful exact 36-file validation, and the sorted
SHA-256 manifest written beside the package directory.

## Release checklist

1. Build from the exact reviewed commit with a clean worktree.
2. Confirm both runtime metadata profiles: Skyrim 1.5.97 with SKSE 2.0.20 and
   Skyrim 1.6.1170 with SKSE 2.2.6.
3. Confirm `UFGU.ini` starts with upscaling, frame generation, Reflex, tearing,
   and diagnostics off; `BaseFrameLimit=0`, `FrameLimit=0`, and
   `JitterFold=NopGate`.
4. Confirm NVIDIA, AMD, and Intel presets differ from the canonical INI only in
   provider selection.
5. Preserve the project licence, linked-component notices, and every required
   vendor notice.
6. Confirm the package has no source, Markdown, game INI, ENB, ReShade, SSE
   Display Tweaks, log, dump, credential, private-development file, or project
   developer path embedded in `UFGU.dll`. The validator separately recognizes
   reviewed vendor-owned NGX build strings introduced by NVIDIA's linked SDK
   library and rejects every other absolute source or build path.
7. Install the finished archive into a clean mod-manager profile and inspect the
   resolved Data tree.
8. Live-test 1.5.97 and 1.6.1170 with their exact SKSE builds. Exercise Off,
   Native AA, Quality, Fixed Frame Generation, Dynamic MFG where supported,
   menu apply, restart-required transitions, and the built-in base limiter.
9. Record physical GPU coverage precisely. Passing AMD or Intel runtime smoke
   tests on NVIDIA hardware is not Radeon or Arc acceptance.
10. Keep the binary release private until NVIDIA's software notification is
    complete and its required attribution and trademark placement have been
    verified against the exact SDK supplement used for the build.
11. Create the archive only after validation. Record SHA-256 for the source
    commit, built DLL, package manifest, and final archive.
12. Repeat the build in a second empty tree and compare the DLL and package
    manifests before calling the release reproducible.

## Maintainer cautions

- Never ship a build configured to use NVIDIA's sample application identity.
- Never bypass runtime loading, signature, canonical-path, or package allowlist
  failures.
- Driver V-Sync guidance must target the exact `SkyrimSE.exe` used by the mod
  manager. If the driver controls V-Sync, ENB `ForceVSync` remains off.
- The built-in Base FPS Cap is for Fixed Frame Generation and bypasses itself in
  Dynamic MFG. `[Reflex] FrameLimit` is a separate final-output control.
