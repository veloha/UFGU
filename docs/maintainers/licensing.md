# Licensing review

This is an engineering inventory, not legal advice.

## Project licence

UFGU's project-owned source is licensed under the Mozilla Public License 2.0.
The authoritative terms are in the top-level `LICENSE` file. MPL-2.0 applies
only to UFGU's covered source and does not relicense vendor SDKs, signed
runtimes or other third-party components.

## Source repository

The source repository contains project code, build scripts, tests, the reviewed
CommonLibSSE-NG compatibility patch, and required notice copies. It does not
vendor NVIDIA Streamline/NGX, AMD FidelityFX, Intel XeSS, Windows SDK, ENB, or
other external SDK trees. Builders obtain those inputs under their respective
terms and provide their paths explicitly.

CommonLibSSE-NG, fmt, spdlog, rapidcsv, and AMD Anti-Lag 2 are covered by the
notice files in `Licenses`. The [dependency inventory](../developers/dependencies.md)
records the exact versions and use. Public research references in
[provenance](provenance.md) are not compiled, linked, copied, or packaged.

## Binary package

The package stager includes the required notices for statically linked
components and places each vendor runtime's licence and third-party notices
beside that provider's DLLs. The exact-manifest validator rejects missing or
unexpected files. Vendor SDK source, samples and tools are never staged.

Do not publish a binary containing NVIDIA SDK redistributables until the exact
SDK supplement has been reviewed, NVIDIA's software notification has been
completed, and the required NVIDIA attribution and trademark placement have
been verified for the release. Keep the release as a private draft until those
steps are recorded. This engineering checklist is not a legal determination.

## Release checklist

Before publishing source:

1. Confirm the top-level `LICENSE` is the unmodified MPL-2.0 text.
2. Verify every third-party notice against the pinned dependency commit or SDK.
3. Review the CommonLibSSE-NG patch under that project's MIT terms.
4. Re-run the source, secret, binary and provenance scans on the exact commit.

Before publishing a binary:

1. Build and package from the exact reviewed source commit.
2. Pass all 32 public tests and the exact 36-file package validator.
3. Preserve every staged third-party notice.
4. Complete NVIDIA's software notification and verify its required attribution.
5. Complete every other applicable vendor redistribution requirement.
6. Record SHA-256 hashes for the source commit, DLL and final archive.
