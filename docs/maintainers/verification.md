# Verification

This document records the checks required for a UFGU source or binary release.
It is engineering evidence, not an independent security or legal audit.

## Source repository

- The working tree must be clean and contain no generated build output, vendor
  SDK trees, signed redistributables, game files, logs, dumps, credentials, or
  private development records.
- Project-owned C++, embedded HLSL, and CMake inputs are built with MSVC
  `/W4 /WX` and remain reachable from a production or test target.
- Repository policy validates required files, JSON and XML syntax, local
  Markdown links, source-only boundaries, installer text, and provenance
  markers.
- Dependency versions, source relationships, notices, and research boundaries
  are recorded in the dependency, licensing, and provenance documents.

## Automated builds

The development configuration must complete 29 tests. The separate public
configuration must complete 32 tests and validate an exact 36-file FOMOD
package containing zero Markdown files. Both configurations are built from a
fresh directory using the pinned inputs in the build guide.

MSVC native code analysis is an additional maintainer check. Its result must be
recorded with the source commit and toolchain version rather than presented as
proof that every runtime or GPU path is defect-free.

## Runtime acceptance

Skyrim 1.5.97 with SKSE 2.0.20 and Skyrim 1.6.1170 with SKSE 2.2.6 have separate
exact runtime profiles. A release must exercise plugin loading, menu operation,
Off, Native AA, Quality, Fixed Frame Generation, supported Dynamic MFG,
restart-required transitions, and the built-in base limiter on both profiles.

Physical GPU claims remain narrower than provider-runtime smoke tests. Current
hardware coverage and compatibility limits are maintained in the known-issues
document.

## Publication gates

Before source publication, rerun repository policy, secret, provenance, licence,
and clean-tree checks on the exact commit. Before binary publication, also
review vendor redistribution terms, preserve all required notices, complete any
required vendor notification, compare two fresh builds, and publish SHA-256
hashes for the commit, plugin DLL, package manifest, and final archive.
