# Design: Windows VR apps → OXRSys on Apple Silicon

Synthesis of three research reports (see research-*.md). Date: 2026-09-07.

## Decision: adopt monofunc/wineopenxr as the bridge (vendored as `bridge/`)

Original plan was a from-scratch wineopenxr port using DXVK + Vulkan handoff.
Research found an existing out-of-tree macOS/CrossOver-26 port —
monofunc/wineopenxr (LGPL-2.1) — that already solves the hard parts, with a
BETTER graphics leg than our sketch:

- D3D11 → Metal directly via DXMT's shared-texture path (zero-copy MTLTexture
  sharing), not D3D11 → DXVK → Vulkan → MoltenVK. (This originally used a
  DXMT fork's `IMTLD3D11InteropDevice`; it now runs on stock DXMT — see
  "Metal interop on stock DXMT" below.)
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

1. DXMT must be selected as the bottle's Graphics backend, but no fork or
   patch is needed any more (see below). The residual risk is that the bridge
   writes/reads DXMT's *private* D3DKMT shared-resource record; it validates
   the layout at session creation and refuses to run if it ever changes.
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


## Metal interop on stock DXMT (2026-09-22)

The bridge used to require two DXMT patches (`IMTLD3D11InteropDevice` for
importing an external `MTLTexture` and reading a fence's `MTLSharedEvent`, plus
a relaxation of that importer's format/usage validation). Both are gone. What
replaced them:

**Swapchain images.** OXRSys allocates its Metal swapchain images with
`newSharedTextureWithDescriptor:` (and `MTLTextureUsagePixelFormatView`), so each
image has an IOSurface that can be published as a mach send right. The bridge's
unix half does `newSharedTextureHandle` → `createMachPort` →
`bootstrap_register2` under a unique name, and the PE half wraps that name (plus
the `D3D11_TEXTURE2D_DESC1` the app should see) in the private runtime data of a
`D3DKMTCreateAllocation2` resource, shaped exactly like the record DXMT writes
for its own shared textures. Plain `ID3D11Device::OpenSharedResource` then
returns an `ID3D11Texture2D` backed by the runtime's texture.

That also removes the need for patch 2: DXMT's shared-resource import performs
no format or usage validation (the custom `ImportMTLTexture2D` did), so passing
the typeless parent format keeps working. The sRGB-over-typeless *view* DXMT
creates is what needs `MTLTextureUsagePixelFormatView`, which the runtime now
sets.

**Render-completion fence.** The obvious replacement — `CreateFence(SHARED)` +
`CreateSharedHandle` + reading the event name back — does not work: CrossOver's
Wine answers `D3DKMTQueryResourceInfoFromNtHandle` on a sync-object handle with
`STATUS_OBJECT_TYPE_MISMATCH`, which also breaks DXMT's own `OpenSharedFence`.
Instead the session creates a 1x1 `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX`
texture. DXMT backs its keyed mutex with an `MTLSharedEvent` and stores that
event's mach service name as the mutex's private runtime data, which
`D3DKMTOpenKeyedMutex2` hands back; the unix half opens the same event with
`newSharedEventWithMachPort:`. At `xrReleaseSwapchainImage` the bridge does
`AcquireSync`/`ReleaseSync` on that carrier, which makes DXMT encode
`waitEvent(n)` + `signalEvent(n+1)` on its own queue behind everything the app
encoded — the same effect as `ID3D11DeviceContext4::Signal`. The value is a plain
release count; the unix half confirms on the first release that the event really
reaches it, and disables the fence rather than risk a queue wait that never
retires.

**Layout risk.** `dxmt_shared_resource_data` is DXMT-private. At session
creation the bridge has stock DXMT write one (for the sync carrier) and checks
the record size and every field — name, dimension, full desc, mutex handle —
against what it passed. A DXMT that reorders it fails `xrCreateSession` with a
named error instead of importing misparsed bytes.
