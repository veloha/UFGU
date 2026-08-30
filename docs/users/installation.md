# Installation

## Requirements

- Skyrim Special Edition 1.5.97 with SKSE 2.0.20, or Skyrim 1.6.1170 with SKSE
  2.2.6
- Address Library for SKSE Plugins for the installed Skyrim runtime
- Microsoft Visual C++ 2015-2022 Redistributable, x64
- Windows and a DirectX 12-capable GPU
- A current graphics driver for the selected provider
- Mod Organizer 2 or another manager that preserves the archive layout

## Install

1. Remove or disable other upscaling and frame-generation injectors.
2. Install the UFGU archive with the FOMOD.
3. Select the GPU preset you intend to start with. Every feature remains off by
   default.
4. Confirm the installed layout contains `SKSE/Plugins/UFGU.dll`, `UFGU.ini`,
   and the private `UFGU/AMD`, `UFGU/Intel`, and `UFGU/Streamline` folders.
5. Launch the exact Skyrim copy managed by the profile through SKSE.
6. Press `PageDown` in game to open the UFGU menu.

Do not move any bundled provider DLL into Skyrim's root directory. UFGU checks
the canonical module-relative path and Authenticode publisher before loading a
vendor runtime.

## Display synchronization

In the graphics driver's program settings, target the exact `SkyrimSE.exe` used
by the mod manager and enable V-Sync. Do not assume the executable in the Steam
directory is the active copy.

When V-Sync is controlled by the driver, leave ENB `ForceVSync` off. A forced
V-Sync-off override causes a moving horizontal tear during camera motion.

## Conflicts

Do not run UFGU with another upscaler, frame generator, DLSS Enabler,
OptiScaler, or presentation-hook replacement. Community Shaders integration is
not currently supported. See [compatibility](compatibility.md).

## Removal

Disable UFGU in the mod manager. The package does not edit save files, ENB,
ReShade, SSE Display Tweaks, or Skyrim INI files.
