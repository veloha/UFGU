# Compatibility

## Game runtimes

Each Skyrim version needs its own build of UFGU. One download does not cover
both. Check which Skyrim version you are on before downloading, and take the
build that matches it.

| Skyrim runtime | SKSE | Support |
|---|---:|---|
| 1.5.97 | 2.0.20 | Needs the 1.5.97 build. Not yet released |
| 1.6.1170 | 2.2.6 | Needs the 1.6.1170 build. Released and live tested |
| Other Special Edition versions | Varies | Rejected unless an exact profile is added and tested |
| Skyrim VR | VR build | Not supported |
| GOG | Varies | Not currently planned or tested |

## Provider capabilities

| Provider | Upscaling | Frame generation | Low latency |
|---|---|---|---|
| NVIDIA | DLSS and DLAA | DLSS FG and hardware-reported MFG | Reflex |
| AMD | FidelityFX | Cross-vendor 2x FG | Not exposed as supported |
| Intel | XeSS | XeSS-FG when reported by the runtime | Not exposed as supported |

The menu is capability-driven. A multiplier shown on one GPU is not promised on
another GPU.

## ENB and ReShade

UFGU has been exercised with ENB, but compatibility can vary by preset and
effect. ENB `ForceVSync` should be off when the graphics driver controls V-Sync.

ReShade compatibility depends on the shader and required buffers. Reduced
resolution modes can lower the cost of effects evaluated before UFGU's upscale;
DLAA does not reduce their processing resolution.

## Community Shaders

Community Shaders integration is untested. Neither compatibility nor a conflict
has been confirmed, so treat the combination as unknown rather than as known
broken. Both projects interact with renderer resources and timing, so if you run
them together you are ahead of any testing we have done. Disabling only the
Community Shaders upscaling feature has not been validated as sufficient either.

If you do try it, a report either way is useful.

## Other renderer injectors

Do not combine UFGU with another Skyrim upscaler, frame generator, DLSS Enabler,
OptiScaler, or swap-chain replacement. Running multiple owners of the same
renderer and presentation hooks is unsupported.

## Test coverage boundary

Live acceptance currently comes from an NVIDIA test system. AMD and Intel
runtime paths pass signature, export, context-creation, and one-frame GPU smoke
tests on that system, but this is not physical Radeon or Arc acceptance.
