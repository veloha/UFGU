# Third-party components

This inventory is documentation, not a substitute for the corresponding
licence and notice files.

| Component | Pinned or reviewed version | Use | Source notice |
|---|---:|---|---|
| CommonLibSSE-NG | `b93280e832f263dbef44e44cbe2936622a02f91a` plus reviewed patch | SKSE integration | `Licenses/CommonLibSSE-NG-LICENSE.txt` |
| rapidcsv | 8.99 | CommonLibSSE-NG dependency | `Licenses/rapidcsv-LICENSE.txt` |
| fmt | 12.2.0 | Native formatting | `Licenses/fmt-LICENSE.txt` |
| spdlog | 1.17.0 | Native logging | `Licenses/spdlog-LICENSE.txt` |
| NVIDIA Streamline | 2.12.0 | DLSS, DLSS-G and Reflex integration | Vendor SDK notices staged beside its runtimes |
| AMD FidelityFX | 2.3.0 SDK runtime set | Upscaling and frame generation | Vendor SDK notices staged beside its runtimes |
| AMD Anti-Lag 2 | 2.0.0a | Headers retained for experimental integration; not advertised as supported | `Licenses/AMD-Anti-Lag-2-LICENSE.txt` |
| Intel XeSS | 3.0.2 | XeSS and XeSS-FG integration; XeLL is staged for SDK compatibility but not advertised as supported | Vendor SDK notices staged beside its runtimes |

The public package validator requires the top-level binary redistribution
notices and each provider's runtime notices. Vendor SDK source trees and tools
must not be committed merely to make a clone self-contained.

UFGU's project-owned source is licensed separately under MPL-2.0 in the
top-level `LICENSE`. That licence does not replace or modify any notice listed
above.
