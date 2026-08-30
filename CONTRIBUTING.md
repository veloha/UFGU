# Contributing

UFGU is licensed under the Mozilla Public License 2.0. By submitting a
contribution, you represent that you are authorised to provide it and agree
that it may be distributed under MPL-2.0. Bug reports, logs, reproducible test
cases and documentation corrections are also useful.

Read the [building](docs/developers/building.md),
[testing](docs/developers/testing.md), [support](SUPPORT.md), and
[code-of-conduct](CODE_OF_CONDUCT.md) documents before opening a pull request.
Work on a focused branch and keep unrelated changes out of the same review.

When contributing:

- Work only from code you are authorised to use. Do not submit copied,
  decompiled, leaked, paywalled or licence-incompatible implementation code.
- Keep renderer behavior changes separate from cleanup and documentation work.
- Keep implementation rationale in focused developer or maintainer
  documentation. Project-owned C++, embedded HLSL, and CMake files are
  maintained as code-only inputs; do not replace local comments with generated
  or verbatim comment archives.
- Preserve fail-closed runtime loading, canonical path validation and signed
  vendor-runtime verification.
- Do not add WinForms, .NET, C++/CLI or a helper executable. The in-game UI and
  runtime are native and in-process.
- Build with MSVC warnings enabled and run all 29 development tests. Release
  changes must also pass the three public package contracts for 32 tests total.
- Do not claim Skyrim runtime, GPU vendor or feature compatibility without
  matching physical acceptance evidence.
- Include a sanitized full `UFGU.log`, GPU and driver, Skyrim and SKSE versions,
  provider modes, resolution and refresh rate with renderer bug reports.

Small, reviewable changes are preferred. A patch that changes presentation,
frame pacing, temporal inputs or hook behavior must explain its invariants and
include a rollback plan.

Before requesting review, run the development suite. Changes to packaging,
configuration defaults, dependencies, notices, or release metadata must also
run the public suite and exact package validator. Maintainers may request
additional physical runtime or GPU evidence before accepting renderer changes.
