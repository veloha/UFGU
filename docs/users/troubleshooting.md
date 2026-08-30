# Troubleshooting

## Start with the log

UFGU writes its log to:

```text
Documents/My Games/Skyrim Special Edition/SKSE/UFGU.log
```

For rendering faults, include the complete log, GPU and driver version, Skyrim
and SKSE versions, selected upscaling and frame-generation settings, output
resolution and refresh rate, and whether the fault stops when frame generation,
upscaling, or UFGU itself is disabled.

## Game does not start

Disable every other upscaling or frame-generation injector before testing.
UFGU must be the only component owning the renderer and presentation path.
Confirm that the installed Skyrim and SKSE versions match the compatibility
boundary in [compatibility](compatibility.md).

If no `UFGU.log` is created, the SKSE plugin did not load. Check the resolved
MO2 Data tree, SKSE log, Microsoft Visual C++ 2015-2022 Redistributable (x64),
and exact game executable version before treating it as a renderer failure.

## Moving horizontal line

This is display tearing when the driver forces V-Sync off. In the GPU driver's
per-program settings, target the exact `SkyrimSE.exe` launched by the mod
manager and enable V-Sync. If ENB is present, leave ENB `ForceVSync` off so only
one layer controls synchronization. The measured boundary is recorded in
[known issues](known-issues.md).

## Fixed frame generation feels uneven

Use UFGU's **Base FPS Cap** and choose a rate the system can sustain in its
heaviest scene. Lower the cap if the real frame rate falls below it. The
built-in cap bypasses itself during Dynamic MFG. Disable other base limiters so
two tools do not fight for cadence. Keep UFGU `[Reflex] FrameLimit=0`; that
value is a final-output control, not the base-FPS limiter.

## Unrelated JContainers crash found during testing

A continue/new-save crash identified as `JContainers64.dll+10AE45` was found
while testing UFGU. It was not caused by UFGU. On the affected setup, the tester
resolved it by reinstalling the .NET 5 Desktop Runtime and Microsoft Visual C++
redistributables, then installing the mod-list-specific
[DISCO JContainers Crash Patch](https://www.nexusmods.com/skyrimspecialedition/mods/108591?tab=files&file_id=458596)
after Custom Skills Menu.

UFGU does not load or require .NET, JContainers, or that patch. Only use this
note when a crash log shows the same JContainers signature. The patch contains
pre-generated data intended for specific mod lists, and .NET 5 is an end-of-life
legacy runtime.
