# Upscaler backends behind the FFX front

The game drives its upscaler through the FidelityFX API five functions (`ffxConfigure`, `ffxQuery`,
`ffxCreateContext`, `ffxDestroyContext`, `ffxDispatch`, measured on 1.0.2.1 in fsr_dispatch_test). That five-function
front is the one integration surface Leonida owns: what sits behind it is a provider choice, not a code change. This
note records the license status and the interface facts for each provider, so the follow-up PRs do not re-survey;
nothing speculative ships here — the only backend code in the tree is the DLSS one (PR: runtime/src/dlss_backend,
behind `LEONIDA_DLSS=1`).

```
game → ffxConfigure/ffxQuery/ffxCreateContext/ffxDispatch (the front, untouched)
         ├─ default:            the package's amd_fidelityfx_upscaler_xbox.dll (FSR4 scarlett), the measured path
         ├─ LEONIDA_DLSS=1:     runtime/src/dlss_backend  → driver NGX Core (nvngx.dll) + nvngx_dlss.dll
         ├─ (reserved) FSR3.1:  a PC FSR3.1 context behind the same five functions — same API, no translation
         └─ (reserved) XeSS:    an XeSS 2.x context — either through its FFX front or a thin bridge
```

## 1. What ships today

**FSR4 (the game's own) — zero code.** `amd_fidelityfx_upscaler_xbox.dll` runs on the Xbox-ABI device proxy and
fsr_dispatch_test drives it step by step (docs/d3d12_vulkan.md Appendix A.3 records the backend descriptor 0x01000002
and the slots it calls). The runtime dll stays next to the package as shipped; nothing from it is redistributed. This
is the default path and it works.

**DLSS — runtime/src/dlss_backend (this PR).** The FFX five functions stay untouched; when `LEONIDA_DLSS=1` the
shim's upscale dispatch reaches the DLSS backend, which translates the FFX description to NGX (isomorphic ML-upscaler
inputs; the field mapping is in dlss_backend.h) and runs it through the driver's NGX Core loaded by name. The
nvngx_api_min.h header is written from the export table and the 310.6 programming guide — no SDK file is in the tree,
and the runtime dlls (nvngx_dlss.dll etc.) are provided by the user's driver installation next to the exe, exactly
like the game's own upscaler dll. Quality mapping and the per-slot DLSS.Hint.Render.Preset hint are the pure
functions in dlss_backend.cpp, covered by dlss_backend_unit_test (CPU-only, no device).

## 2. FSR3.1 — by construction, almost nothing to ship

FSR 3.1 exposes the same five functions: the game's front is already FSR 3.1-shaped (the API is versioned at the
context; fsr_dispatch_test measures 4.1/4.1.1 version descriptors on the FSR4 dll). A PC FSR3.1 provider is therefore
the same shape as the FSR4 provider — context creation, version descriptor, a D3D12 backend interface — with no
translation layer at all. The integration work is plumbing, not mapping:

- provider choice in the shim (the game dll is FSR4; a PC FSR3.1 runtime is opt-in via env, same pattern as
  `LEONIDA_NGX_DLL`),
- the pre-existing hard prerequisite: the descriptor model must land first (#335) — color/depth/mv need real
  render outputs to feed either provider.

**License map (measured on the upstream repos, 2026-11-23):** the `GPUOpen-Effects/FidelityFX-FSR` repo is MIT
(license.txt, the standard grant), but it ships only the FSR1-era headers (`ffx_a.h`, `ffx_fsr1.h`). The FFX API and
FSR3.x sources live in the `GPUOpen-LibrariesAndSDKs/FidelityFX-SDK` repo under `Kits/FidelityFX`, whose license.md
is **not MIT** — it is AMD's redistribution license ("binary form only", "no reverse engineering, decompilation, or
disassembly"). So, same rule as DLSS: no SDK header is copied into this GPL tree. The front's ABI is already written
from the export table and the measured structs (fsr_dispatch_test's own ffx types); the provider dll is
user-provided as shipped.

## 3. XeSS — reserved, same pattern

Intel XeSS 2.x accepts both shapes: a D3D12-native API (`xess.h`: `xessD3D12CreateContext`/`Init`/Execute…, which
needs an FFX→XeSS bridge of ~a dozen setters — jitter, mv scale, exposure, reset all have named equivalents) and a
FFX-API front (`libxess_fx.dll`, which answers the same five functions and would need no bridge at all). The
non-Intel path exists (DP4a / SM6.4 dot-product kernels run on any vendor), so XeSS is the natural third provider
rather than an Intel-only one — but it is not current work.

**License (intel/xess LICENSE.txt, read 2026-11-23):** Intel Simplified Software License — binary form only,
explicitly no reverse engineering or modification. Same conclusion as above: nothing from the XeSS SDK enters the
tree; an xess_api_min.h-style header would be written from the export table at integration time, and the runtime
libs are user-provided.

## 4. The complete license map

| Component | License | In this tree? | User-side artifact |
|---|---|---|---|
| game's `amd_fidelityfx_upscaler_xbox.dll` (FSR4) | game EULA | no — measured, never redistributed | as shipped |
| driver NGX Core (`nvngx.dll`) | NVIDIA driver distribution | no — loaded by name LEONIDA_NGX_DLL | driver install |
| `nvngx_dlss.dll` / `dlssg` / `dlssd` | NVIDIA driver / RTX EULA | no | driver install (exe-adjacent) |
| NVIDIA DLSS SDK (headers, libs, docs) | RTX proprietary (no OSS relicensing) | no — ABI written from export table + 310.6 guide | not needed |
| AMD FFX API + FSR3.x (`FidelityFX-SDK` Kits) | AMD redistribution (binary-only, no RE) | no — front ABI already written from the export table | user-provided FSR3.1 runtime (reserved) |
| `GPUOpen-Effects/FidelityFX-FSR` repo (FSR1 headers) | MIT | usable, but FSR1 has no role here | — |
| Intel XeSS SDK (`xess.h`, `libxess*`) | Intel Simplified (binary-only, no RE) | no — reserved; ABI from export table at integration time | user-provided runtime (reserved) |

The pattern is uniform: Leonida writes its own minimal ABI headers from export tables and public specifications,
never ships a vendor SDK file, and every runtime dll is user-provided at the location its installer manages. The
providers differ only in what their translation layer contains: FSR4/FSR3.1 none, DLSS the NGX parameter bag with
its quality/preset mapping (shipped), XeSS a small bridge (reserved).
