# GitHub repository setup

The source tree is prepared for a private GitHub repository. UFGU's
project-owned source is licensed under MPL-2.0 as described in
[licensing](licensing.md).

Before making the repository public:

1. Confirm the top-level `LICENSE` contains the unmodified MPL-2.0 terms.
2. Re-run the clean development and public builds from
   [building](../developers/building.md).
3. Confirm the repository-policy workflow passes on the exact publication
   commit.
4. Enable private vulnerability reporting and use the root `SECURITY.md` as the
   policy.
5. Protect the default branch by requiring pull requests and the repository
   policy check.
6. Keep Actions permissions read-only unless a future workflow has a documented
   need for more access.
7. Publish release binaries separately from source. Attach the package manifest
   and SHA-256 report produced by the public build.

Do not commit vendor SDK trees, signed redistributable collections, local build
directories, game files, logs, videos, crash dumps or private development
records. The repository contains only project-owned source, build metadata,
tests, documentation, the reviewed CommonLibSSE-NG patch and permitted notice
copies.

The issue forms collect the minimum environment and comparison data needed for
renderer bugs. Keep security reports out of public issues and route them through
GitHub private vulnerability reporting.
