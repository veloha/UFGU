# Known issues and coverage limits

UFGU is beta software. Report a renderer fault with the complete `UFGU.log`,
hardware and driver, Skyrim and SKSE versions, output resolution and refresh
rate, provider settings, and a comparison with frame generation, upscaling, and
UFGU disabled.

## Hardware coverage

- Live acceptance currently comes from one NVIDIA test system.
- AMD and Intel paths pass runtime signature, required-export, context-creation,
  and one-frame GPU smoke tests on that system. This proves the paths do not
  silently fall back to NVIDIA, but it is not physical Radeon or Arc acceptance.
- The exact FidelityFX model selected internally on a non-AMD adapter remains
  provider-defined. Do not treat successful AMD-path evaluation on an NVIDIA
  adapter as proof that FSR 4 ML upscaling ran.
- Intel 3x and 4x availability comes from the runtime's reported capability.
  Those multipliers have not been accepted on physical Arc hardware.

## Compatibility limits

- Community Shaders integration is unsupported.
- Do not combine UFGU with another upscaler, frame generator, DLSS Enabler,
  OptiScaler, or presentation-hook replacement.
- ENB depth of field has not received a complete compatibility pass.
- Sampler mip-bias automation remains disabled by default while ENB and ReShade
  combinations are evaluated.

## Performance and packaging

- D3D11-to-D3D12 capture synchronization can show occasional multi-millisecond
  stalls in diagnostics and remains an investigation item.
- The UFGU plugin DLL is not Authenticode-signed. Bundled vendor runtimes are
  signed and their expected publishers are validated before loading.
- The build produces and validates a package directory. Deterministic archive
  creation, final archive hashing, and comparison of two independent fresh
  builds remain maintainer release tasks.

## Setup requirements

- Enable V-Sync in the graphics driver's per-program settings for the exact
  `SkyrimSE.exe` launched by the mod manager. If the driver controls V-Sync,
  leave ENB `ForceVSync` off. A moving horizontal line during camera movement
  is display tearing caused by the wrong synchronization override.
- For Fixed Frame Generation, use UFGU's **Base FPS Cap** and choose a rate the
  system can sustain in its heaviest area. UFGU bypasses this cap automatically
  during Dynamic MFG.
- Keep `[Reflex] FrameLimit=0` unless deliberately testing a final-output cap.
  It is not the Fixed-mode base cap.

## Resolved regressions

- Native UI separation removed the reported HUD ghosting.
- The incorrect black screenshot background is fixed on the accepted test
  system.
- Menu opacity flicker under vendor frame generation is fixed.
- The 1.6.1170 DLAA shimmer regression is fixed by the accepted jitter policy.
- Frame generation now survives supported multiplier changes and menu closure.

## Unrelated crash note

A JContainers continue/new-save crash found during testing was not caused by
UFGU. Its workaround is documented in [troubleshooting](troubleshooting.md) only
for users whose crash log shows the same JContainers signature.
