# Design: Windows VR apps → OXRSys on Apple Silicon

Synthesis of three research reports (see research-*.md). Date: 2026-09-07.

## Decision: adopt monofunc/wineopenxr as the bridge (vendored as `bridge/`)

Original plan was a from-scratch wineopenxr port using DXVK + Vulkan handoff.
Research found an existing out-of-tree macOS/CrossOver-26 port —
monofunc/wineopenxr (LGPL-2.1) — that already solves the hard parts, with a
BETTER graphics leg than our sketch:

- D3D11 → Metal directly via DXMT's `IMTLD3D11InteropDevice` (zero-copy
  MTLTexture sharing), not D3D11 → DXVK → Vulkan → MoltenVK.
- Native side speaks ratified `XR_KHR_metal_enable`:
  `xrGetMetalGraphicsRequirementsKHR` → runtime's MTLDevice → bridge creates
  MTLCommandQueue → `XrGraphicsBindingMetalKHR{commandQueue}` (session.m:42-120).
- OXRSys advertises `XR_KHR_metal_enable` (EntryPoint.cpp:240) and its Metal
  session path is its *synchronized* path: staging blit on the app-provided
  queue + MTLSharedEvent (Swapchain.mm:600-644) — unlike its Vulkan path,
  which returns live textures with no GPU sync (Swapchain.mm:710-725).
- PE/unix split identical to Valve's unixlib design, but buildable
  out-of-tree: mingw-w64 PE DLL + dlltool ntdll import lib +
  builtin-signature injection (sign_builtin.py) replaces winegcc/winebuild;
  unix side is plain clang `-arch x86_64` Mach-O named `.so`.
- Unix side links stock Khronos openxr_loader → discovers OXRSys via
  XR_RUNTIME_JSON / ~/.config/openxr/1/active_runtime.json, which OXRSys
  Home's installer already configures.

## Full chain

    Windows VR game (x86-64 PE, D3D11 OpenXR; or OpenVR via OpenComposite PE openvr_api.dll)
      → stock Windows openxr_loader.dll
      → wineopenxr.dll  (PE builtin; ActiveRuntime registry → C:\openxr\wineopenxr64.json)
      → __wine_unix_call → wineopenxr.so  (x86_64 Mach-O under Rosetta)
      → Khronos native openxr_loader → liboxrsys-runtime.dylib (universal; x86_64 slice)
      → XrGraphicsBindingMetalKHR; swapchain MTLTextures shared into DXMT as ID3D11Texture2D
      → OXRSys staging blit → VideoToolbox H.265 → Wi-Fi/USB → Quest

## Why the constraints all clear (verified on this machine)

| Constraint | Status |
|---|---|
| Rosetta procs can't load arm64 dylibs | liboxrsys-runtime.dylib is universal (lipo: x86_64 arm64) ✓ |
| PE toolchain on arm64 | Homebrew mingw-w64 15.2 installed, PE32+ smoke-tested ✓ |
| winevulkan/MoltenVK | Not needed — Metal path bypasses Vulkan entirely ✓ |
| CrossOver unixlib layout | 26.2 installed; lib/wine/x86_64-unix/*.so are Mach-O ✓ |
| OpenVR titles | OpenComposite builds PE openvr_api.dll (xrizer is Linux-only — ruled out) |
| D3D12 | Deferred (vkd3d-proton needs Vulkan; no DXMT-D3D12) |

## Key differences from Valve's wineopenxr (research-valve-wineopenxr.md)

Valve's Linux design needs private winevulkan/win32u patches
(VkCreateInfoWineInstanceCallback, VK_WINE_openxr_device_extensions smuggling,
__wine_set_unix_env) because its graphics leg is Vulkan handle-sharing with
DXVK. The Metal leg sidesteps ALL of that: no winevulkan internals, no
CrossOver source patches. Extension substitution becomes
XR_KHR_D3D11_enable → XR_KHR_metal_enable (bridge/src/include/extension_substitutions.h).

## Known risks / open items

1. DXMT fork requirement: bridge needs monofunc/dxmt installed in CrossOver
   and selected as the bottle's Graphics backend (IMTLD3D11InteropDevice is
   their addition). Stock CrossOver DXMT lacks the interop interface.
2. OXRSys single-instance/single-session globals; 64-bit PE only (handles are
   raw pointers as uint64).
3. OXRSys negotiation rejects loaders demanding minApiVersion ≥ 1.1
   (EntryPoint.cpp:3790) — fine for 1.0-era game loaders; watch new titles.
4. Formats: OXRSys offers 6 swapchain formats, no RGBA16F — UE titles may ask
   for more; DXGI↔Metal format table in bridge/src/include/formats.h is the
   place to extend.
5. QPC time converters (xrConvertWin32PerformanceCounterToTimeKHR): OXRSys has
   no xrConvertTime* — Unreal titles use them. Bridge-side emulation possible
   (map QPC ↔ OXRSys's steady_clock session epoch) — TODO if a UE title needs it.
6. Modifying CrossOver.app payload (installing the .so) breaks its code
   signature → ad-hoc re-sign; reinstall after every CrossOver update.
7. Wine-internal ABI drift (unixlib dispatcher, builtin signature) — pin per
   CrossOver major version.

## Milestones

M1  Build bridge (dll + so) on this machine.                      ← this session
M2  Install: CrossOver payload + bottle registry + DXMT fork.     ← scripted this session
M3  hello_xr.exe (D3D11) in a bottle renders on Quest via OXRSys. ← needs headset
M4  OpenComposite PE openvr_api.dll in front → first OpenVR title.
M5  Upstream fixes: OXRSys Vulkan-path sync; more formats; QPC converters.
