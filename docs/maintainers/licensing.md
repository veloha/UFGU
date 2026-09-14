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

A binary containing NVIDIA SDK redistributables ships NVIDIA's licence files
beside those runtimes, names the features NVIDIA DLSS and NVIDIA Reflex in the
in-game menu, uses no NVIDIA logos, and never implies that NVIDIA made or
endorses UFGU. NVIDIA's runtimes stay under NVIDIA's terms and are not covered
by the project licence.

The NVIDIA RTX SDK supplement asks for a software notification before a
commercial release. UFGU is free, so the notification is optional rather than
a publication gate. It remains a good idea if the project is ever sold or
bundled with something that is. This engineering checklist is not a legal
determination.

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
4. Keep NVIDIA's licence files, the NVIDIA DLSS and NVIDIA Reflex menu names,
   and the no-logo, no-endorsement rule described above.
5. Complete every other applicable vendor redistribution requirement.
6. Record SHA-256 hashes for the source commit, DLL and final archive.
