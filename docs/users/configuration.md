# Configuration

Open the in-game menu with `PageDown`. Changes that replace a provider or
presentation route require a restart; the menu reports when that is necessary.

## Upscaling

Choose NVIDIA, AMD, or Intel, then select Off, Native AA, Quality, Balanced,
Performance, or Ultra Performance when offered. Availability comes from the
active adapter and signed vendor runtime.

NVIDIA exposes DLSS presets through `UFGU.ini`. `Recommended` is the safe
default. Advanced presets such as K or L should only be changed for deliberate
quality testing.

## Frame generation

Fixed mode uses the selected multiplier. Dynamic mode is available only when
the NVIDIA runtime and hardware report support.

The upscaling provider and frame-generation provider are separate. For example,
an RTX 30-series user can keep NVIDIA DLSS upscaling and select AMD 2x Frame
Generation. NVIDIA Frame Generation and Multi Frame Generation still require
supported NVIDIA hardware.

## FPS controls

**Base FPS Cap** limits Skyrim's real rendered frames for stable Fixed Frame
Generation pacing. Choose a rate the system can sustain in its heaviest areas.
UFGU bypasses this cap automatically during Dynamic MFG.

**Final Output Cap** controls final presented output. `0` leaves presentation
uncapped by UFGU. It is not a replacement for the Fixed-mode base cap.

Use one limiter for each purpose. Disable external base limiters during Dynamic
MFG.

## Low latency

NVIDIA Reflex is available when the NVIDIA path reports support. AMD and Intel
low-latency controls are not exposed as supported user features in this beta.

## INI configuration

The installed file is `SKSE/Plugins/UFGU.ini`. Use it for additional provider
and diagnostic controls that are intentionally not shown in the menu.

Keep these release defaults unless a documented test says otherwise:

```ini
[FrameGeneration]
BaseFrameLimit=0

[Upscaling]
SurfaceModel=CompleteFrame
JitterFold=NopGate

[Reflex]
FrameLimit=0

[Debug]
View=0
```

Debug views replace the scene image and should remain off during normal play.
