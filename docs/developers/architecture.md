# Architecture

UFGU is an in-process native SKSE plugin. It has no managed runtime, helper
application, service, network component, or authentication layer.

## Runtime flow

1. SKSE loads `UFGU.dll` and selects an exact Skyrim runtime profile.
2. Configuration is loaded from the module-relative `UFGU.ini`.
3. Renderer hooks capture Skyrim's scene extent, camera constants, depth, and
   motion data.
4. `PresentationBridge` owns the D3D11 presentation boundary and D3D12 interop
   required by vendor runtimes.
5. `UpscalingPass` evaluates the selected provider against scene resources.
6. Native UI composition is retained at output resolution.
7. The selected frame generator receives color, depth, motion, camera, and UI
   contracts before presentation.

## Main components

| Area | Responsibility |
|---|---|
| `src/plugin` | SKSE entry point and lifecycle |
| `src/config` | INI parsing, validation, persistence, and safe defaults |
| `src/render` | Runtime profiles, hooks, resources, UI composition, pacing, diagnostics |
| `src/providers` | Runtime discovery, identity, signatures, low-latency control, FSR and XeSS upscalers |
| `src/streamline` | NVIDIA Streamline initialization, DLSS, DLSS-G, tagging, and submission |
| `src/enb` | Optional ENB API discovery and interoperability |

## Safety boundaries

- Only exact Skyrim runtime profiles are accepted.
- Vendor DLLs must resolve under UFGU's private module-relative directory.
- Canonical paths and expected Authenticode publishers are validated.
- Unsupported provider choices fail closed instead of silently routing to a
  different vendor.
- Public packaging uses an exact allowlist and rejects unexpected files.

## Documentation policy

Project-owned C++, headers, embedded shader strings, and CMake files are kept as
code-only inputs. Architecture decisions, release invariants, compatibility
limits, and operational guidance belong in focused documents under `docs`.
Generated comment dumps and stale point-in-time reports are not retained in the
public source tree.
