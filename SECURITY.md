# Security policy

## Supported versions

Security fixes are made against the newest maintained release-candidate branch.
Withdrawn binaries, superseded experiments and unsupported Skyrim runtimes do
not receive security updates.

| Component | Supported |
|---|---|
| Latest UFGU beta or release candidate | Yes |
| Superseded UFGU builds | No |
| Skyrim 1.5.97 with SKSE 2.0.20 | Yes |
| Skyrim 1.6.1170 with SKSE 2.2.6 | Yes |
| Other Skyrim runtimes and Skyrim VR | No |

## Reporting a vulnerability

Do not publish exploit details, malicious DLLs, personal paths, or private logs
in a public issue. Use [GitHub private vulnerability reporting](https://github.com/veloha/UFGU/security/advisories/new).
If that form is unavailable, open a public issue containing only the title
"Private security contact requested" and ask the maintainer to establish a
private channel. Do not include vulnerability details in that issue.

Include the affected commit or binary hash, Skyrim and SKSE versions, a concise
impact statement, reproduction steps, and the smallest safe proof of concept.
Remove credentials and unrelated personal data from logs.

## Security boundaries

Reports are particularly useful when they concern:

- vendor-runtime path containment or Authenticode publisher validation;
- loading a DLL outside UFGU's private module-relative runtime tree;
- unsafe configuration parsing or path traversal;
- renderer-hook validation, memory corruption or lifetime errors;
- FOMOD or package-manifest writes outside the intended UFGU paths;
- release artifacts containing source, credentials or private development data.

Issues in vendor drivers, vendor-signed runtimes, Skyrim, SKSE, ENB, ReShade or
other mods should be reported to their maintainers unless UFGU creates the
vulnerability or bypasses an established security boundary.

The best-effort targets are acknowledgement within seven days and an initial
assessment within fourteen days. These are targets, not guaranteed remediation
deadlines. Coordinated disclosure is preferred for validated issues.
