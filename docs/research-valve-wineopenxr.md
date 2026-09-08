# wineopenxr Architecture Report

**Version analyzed:** `ValveSoftware/Proton`, branch **`proton_11.0`**, directory **`wineopenxr/`** (latest commits touching it: 2026-06 "Update to 1.1.58 registry version", 2026-08 "Remove wrapped XrSession from session list on destroy"). Files were fetched to `/tmp/research-wineopenxr/wineopenxr/`.

Important location note: wineopenxr does **not** live in Valve's wine fork (`ValveSoftware/wine` has no `dlls/wineopenxr`), and it is **not** in upstream wine. It lives in the **Proton repo** as a standalone Wine module (`wineopenxr/` at repo root), built against the Proton wine tree. Supporting pieces live in `ValveSoftware/wine` (`dlls/win32u/vulkan.c`, `dlls/winevulkan/winevk.xml`, `loader/wine.inf.in`, ntdll's `__wine_set_unix_env`) and in Valve's DXVK fork (`src/dxvk/dxvk_openxr.cpp`).

---

## 1. File inventory

| File | Lines | Role |
|---|---|---|
| `Makefile.in` | 11 | Wine module makefile. `MODULE = wineopenxr.dll`, `UNIXLIB = wineopenxr.so`, `IMPORTS = advapi32 user32 dxgi winevulkan` |
| `wineopenxr.spec` | 4 exports | PE exports: `xrNegotiateLoaderRuntimeInterface`, `__wineopenxr_GetVulkanInstanceExtensions`, `__wineopenxr_GetVulkanDeviceExtensions`, `wineopenxr_init_registry` |
| `openxr_loader.c` | 2010 | **Hand-written PE side**: runtime negotiation, session/swapchain wrappers, D3D11/D3D12 emulation, DXVK/vkd3d interop, registry init |
| `openxr.c` | 475 | **Hand-written Unix side**: extension substitution, host xrCreateInstance/Session, Vulkan handle unwrapping via winevulkan internals, Vk create callbacks, QPC<->timespec time conversion |
| `loader_thunks.c/.h` | 5751 / 4067 | **Generated PE side**: one `WINAPI xrFoo()` per API function that packs a `struct xrFoo_params` and does `UNIX_CALL`; the `xr_instance_dispatch_table` name->func table; `enum unix_call`; per-function params structs |
| `openxr_thunks.c/.h` | 6303 / 965 | **Generated Unix side**: `thunk64_xrFoo` handlers, `__wine_unix_call_funcs[]` table, `struct openxr_instance_funcs` host dispatch table + `ALL_XR_INSTANCE_FUNCS()` macro (460 functions) |
| `openxr_loader.h` | 127 | Shared PE-side header: wrapper structs `wine_XrInstance/Session/Swapchain`, `UNIX_CALL` macro, unixlib params structs |
| `openxr_private.h` | 60 | Unix-side header: `conversion_context` pool allocator, unixlib entry declarations |
| `loader_structs.h` | 30 | `XrNegotiateLoaderInfo` / `XrNegotiateRuntimeRequest` (Khronos loader-negotiation ABI structs) |
| `wineopenxr.h` | 10654 | Generated Win32-flavoured OpenXR header (from `xr.xml` 1.1.58). `XRAPI_CALL = __stdcall` on PE; `WINE_XR_HOST` define strips it for the unix build (`wineopenxr.h:28-40`) |
| `dxvk-interop.h` | 366 | DXVK COM interop: `IDXGIVkInteropSurface`, `IDXGIVkInteropDevice`, `IDXGIVkInteropDevice2` |
| `vkd3d-proton-interop.h` | 308 | vkd3d-proton COM interop: `ID3D12DeviceExt1`, `ID3D12DXVKInteropDevice`, `ID3D12DXVKInteropDevice2` |
| `make_openxr` | ~3200 (py) | Generator (fork of winevulkan's `make_vulkan`); downloads `xr.xml`, emits all `*_thunks.*` + `wineopenxr.h` |
| `wineopenxr.json` | | Windows-side manifest, `"library_path": ".\\wineopenxr.dll"`, `"api_version": "1.1.58"` |
| `wineopenxr64.json` | | The manifest installed in the prefix: `"library_path": "C:\\windows\\system32\\wineopenxr.dll"` |

---

## 2. PE/Unix split: it IS a modern unixlib

This is the current unixlib mechanism, not the old winegcc mixed build:

- `Makefile.in` declares `MODULE = wineopenxr.dll` **and** `UNIXLIB = wineopenxr.so`. `openxr.c` and `openxr_thunks.c` carry `#pragma makedep unix` (openxr.c:1-3, openxr_thunks.c:22-24) -> compiled into the `.so`; `openxr_loader.c` + `loader_thunks.c` -> the PE DLL.
- PE->Unix calls go through `#define UNIX_CALL(code, params) WINE_UNIX_CALL(unix_##code, params)` (openxr_loader.h:125) after `__wine_init_unix_call()` in `wine_openxr_unix_init` (openxr_loader.c:203-211). The Unix side exports the standard `const unixlib_entry_t __wine_unix_call_funcs[]` (openxr_thunks.c:5834), indexed by `enum unix_call` in loader_thunks.h:26ff, with `unix_init` and `unix_is_available_instance_function` as the two non-API entries.
- **64-bit only.** Every unix thunk is inside `#ifdef _WIN64`; there is no `thunk32_*`/wow64 path at all (`grep -c thunk32` = 0), and Proton builds it only for x86_64 and arm64ec (`WINEOPENXR_aarch64_PE_ARCHS = arm64ec x86_64`, Proton `Makefile.in:448`). Only `wineopenxr64.json` is installed. This matters: **PE-side pointers are passed raw through the params structs and dereferenced on the unix side** (shared address space), including wrapper objects allocated by PE `calloc`.
- The unix `.so` links the **native Khronos OpenXR loader**: Proton `Makefile.in:445` `WINEOPENXR_LDFLAGS = -lopenxr_loader`, `WINEOPENXR_DEPENDS = wine openxr`, where `openxr` is a unix CMake build of the `OpenXR-SDK` submodule (Proton `Makefile.in:428-437`). `openxr.c` therefore calls `xrCreateInstance`/`xrGetInstanceProcAddr`/`xrEnumerateInstanceExtensionProperties` directly as loader symbols (openxr.c:80, 87, 154).

### Wrapper objects and handle policy

Only **three** handle types are wrapped -- the generator hardcodes this in `make_openxr:1057-1066` (`host_handle()` knows only `XrInstance`, `XrSession`, `XrSwapchain`). Everything else (`XrSpace`, `XrAction`, `XrActionSet`, `XrHandTrackerEXT`, `XrSystemId`, atoms, futures, ...) is passed through as raw host handles.

- `wine_XrInstance` (openxr_loader.h:56-70): `host_instance`, cached `systemId`, and the graphics-binding state: `dxvk_device` (`IDXGIVkInteropDevice2*`), `d3d12_device`/`d3d12_device2`, `d3d12_queue`, plus `vk_device`, `vk_queue`, `vk_command_pool`, `vk_phys_dev`, `vk_instance`.
- `wine_XrSession` (openxr_loader.h:84-96): `host_session`, back-pointer to instance, `session_type` (`SESSION_TYPE_VULKAN/OPENGL/D3D11/D3D12`, openxr_loader.c:16-19), a `struct list entry` in a global `session_list` (needed to reverse-map host->wrapper in `xrPollEvent`, openxr_loader.c:37-55), and scratch arrays used to rewrite composition layers each frame.
- `wine_XrSwapchain` (openxr_loader.h:102-113): `host_swapchain`, session back-pointer, the cached D3D image array (`images`, `image_count`), D3D12 acquire-order ring (`acquired`, `acquired_indices`, `acquired_count/start`), the original `create_info`, and per-image `cmd_acquire`/`cmd_release` `VkCommandBuffer`s.

Wrappers are **allocated on the PE side** (`calloc` in PE `xrCreateInstance` openxr_loader.c:461, `xrCreateSession` :566, `xrCreateSwapchain` :984; wrapper handle = pointer cast, `*instance = (XrInstance)wine_instance` :474) and **dereferenced on the unix side** by the generated thunks (`wine_instance_from_handle(params->instance)->host_instance`, e.g. openxr_thunks.c thunk64_xrGetSystem:3729ff, thunk64_xrDestroyInstance:1617ff). This works only because of the shared 64-bit address space -- a deliberate simplification vs winevulkan.

### Dispatch flow for a typical call (e.g. `xrWaitFrame`)

1. Windows game's `openxr_loader.dll` (the stock Khronos Windows loader shipped with the game) calls the pointer it got from wineopenxr's `xrGetInstanceProcAddr`.
2. PE thunk (loader_thunks.c pattern, e.g. :26-37): fill `struct xrWaitFrame_params {session, frameWaitInfo, frameState, result}` (loader_thunks.h), `UNIX_CALL(xrWaitFrame, &params)`.
3. Unix `thunk64_xrWaitFrame`: `g_xr_host_instance_dispatch_table.p_xrWaitFrame(wine_session_from_handle(params->session)->host_session, ...)`.
4. `g_xr_host_instance_dispatch_table` is filled at instance creation by looping `ALL_XR_INSTANCE_FUNCS()` over host `xrGetInstanceProcAddr` (openxr.c:86-89).

### xrGetInstanceProcAddr dispatch

PE `xrGetInstanceProcAddr` (openxr_loader.c:1832-1858) first asks the unix side whether the function exists (`unix_is_available_instance_function` -> `is_available_instance_function_openxr`, openxr.c:450-475, which force-reports the four D3D/QPC emulated functions as `always_supported` and otherwise defers to host `xrGetInstanceProcAddr`), then resolves the PE thunk by a **flat name->pointer table** `xr_instance_dispatch_table` searched linearly in `wine_xr_get_instance_proc_addr` (loader_thunks.c:5739-5751).

### String/struct conversion

OpenXR is a `char`/UTF-8 API -- there is **no wchar conversion anywhere**. Because the module is 64-bit-only and win64 struct layout matches Linux x86_64, there is exactly **one** generated struct conversion in the entire unix side: `convert_XrInstanceCreateInfo_win64_to_host` (openxr_thunks.c:35-71), which deep-copies the `next` chain, blindly copying 32-byte structs whose type has high word `0x7ead` (Valve/SteamVR-internal structs) and dropping/FIXME-ing others. A small `conversion_context` bump allocator (openxr_private.h:15-52) backs this. Everything else, including all `next` chains on session-level calls, is passed through untouched.

### Which functions are hand-written (generator's own lists, make_openxr:133-180)

- `MANUAL_UNIX_THUNKS` (custom unix-side code in openxr.c): `xrCreateInstance`, `xrCreateSession`, `xrCreateSwapchain`, `xrGetInstanceProcAddr`, `xrEnumerateInstanceExtensionProperties`, `xrConvertTimeToWin32PerformanceCounterKHR`, `xrConvertWin32PerformanceCounterToTimeKHR`, `xrGetD3D11GraphicsRequirementsKHR`, `xrGetD3D12GraphicsRequirementsKHR`, `xrGetVulkanGraphicsDeviceKHR`, `xrGetVulkanGraphicsDevice2KHR`, `xrGetVulkanInstanceExtensionsKHR`.
- `MANUAL_LOADER_FUNCTIONS` (entirely hand-written on PE, no unix thunk): `xrGetD3D11GraphicsRequirementsKHR`, `xrGetD3D12GraphicsRequirementsKHR`, `xrCreateApiLayerInstance`, `xrGetInstanceProcAddr`, `xrNegotiateLoaderRuntimeInterface`, `xrNegotiateLoaderApiLayerInterface`, `xrCreateVulkanInstanceKHR`, `xrCreateVulkanDeviceKHR`.
- `MANUAL_LOADER_THUNKS` (hand-written PE thunk, generated params): `xrCreateInstance`, `xrDestroyInstance`, `xrCreateSession`, `xrDestroySession`, `xrPollEvent`, `xrGetSystem`, `xrEnumerateSwapchainFormats`, `xrCreateSwapchain`, `xrDestroySwapchain`, `xrEnumerateSwapchainImages`, `xrAcquireSwapchainImage`, `xrReleaseSwapchainImage`, `xrBeginFrame`, `xrEndFrame`, `xrGetVulkanDeviceExtensionsKHR`.
- `UNSUPPORTED_EXTENSIONS` (make_openxr:86-97): `XR_EXT_debug_utils`, `XR_KHR_loader_init`, `XR_MSFT_perception_anchor_interop`, `XR_HTC_foveation`.

`xrPollEvent` (openxr_loader.c:727-776) is the one place needing host->wrapper reverse mapping: it patches `evt->session` for the six session-carrying event types via the global session list.

---

## 3. Vulkan instance/device crossing (`XR_KHR_vulkan_enable` + `vulkan_enable2`)

### Unwrapping app handles (vulkan_enable path)

The app's `VkInstance`/`VkPhysicalDevice`/`VkDevice` are **winevulkan client wrappers**. The unix side includes `wine/vulkan_driver.h` (openxr.c:15) and unwraps them with winevulkan's unix-side inline helpers:

- `wine_xrCreateSession` (openxr.c:102-136): for `XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR` (== `..._VULKAN_KHR`), rewrites the binding: `our_vk_binding.instance = vulkan_instance_from_handle(their->instance)->host.instance`, same for `physicalDevice` (`->host.physical_device`) and `device` (`->host.device`) (openxr.c:114-116), then calls host `xrCreateSession`.
- `wine_xrGetVulkanGraphicsDeviceKHR` / `2KHR` (openxr.c:221-255): unwrap the instance, call host, then **re-wrap the returned host VkPhysicalDevice into the client handle** via `get_client_physical_device()` (openxr.c:206-219), which scans `vulkan_instance_from_handle(handle)->physical_devices[i].host.physical_device` and returns `.client.physical_device`.
- `wine_xrGetVulkanInstanceExtensionsKHR` (openxr.c:257-296): appends `"VK_KHR_surface VK_KHR_win32_surface"` to whatever the Linux runtime reports, because Windows SteamVR reports win32_surface and games (hello_xr included) expect it.

### `xrCreateVulkanInstanceKHR` / `xrCreateVulkanDeviceKHR` (vulkan_enable2) -- the callback trick

Problem: the host runtime must create the VkInstance/VkDevice itself (it injects its own extensions), but the app must receive a *winevulkan-wrapped* handle. Valve solved it with a private winevulkan extension:

1. PE `xrCreateVulkanInstanceKHR` (openxr_loader.c:1761-1794) chains a `VkCreateInfoWineInstanceCallback` struct (`sType = VK_STRUCTURE_TYPE_CREATE_INFO_WINE_INSTANCE_CALLBACK`, defined in Valve's `dlls/winevulkan/winevk.xml`) onto the app's `VkInstanceCreateInfo.pNext`, carrying a **unix function pointer** (`native_create_callback`, obtained once at init via `unix_init` -> `init_openxr`, openxr.c:360-367 / openxr_loader.c:203-210) and a context pointer, then calls plain winevulkan `vkCreateInstance`.
2. Inside wine's unix vulkan code (`ValveSoftware/wine dlls/win32u/vulkan.c:702-707`): after converting the create-info to host format, win32u pops that struct and, instead of calling `p_vkCreateInstance`, invokes the callback with the fully converted host `VkInstanceCreateInfo`, host allocator, and host `vkGetInstanceProcAddr`.
3. The callback is wineopenxr's unix `vk_create_instance_callback` (openxr.c:298-337): it builds `XrVulkanInstanceCreateInfoKHR` around the host create-info (force-adding `VK_KHR_surface` + `VK_KHR_xlib_surface` if missing, openxr.c:315-331) and calls host `xrCreateVulkanInstanceKHR`. The host VkInstance it produces is then wrapped by win32u as a normal winevulkan instance (`vulkan_object_init_ptr(..., host_instance, ...)`, win32u/vulkan.c:709).
4. `vk_create_device_callback` (openxr.c:339-358) is the identical dance for `xrCreateVulkanDeviceKHR` <-> win32u/vulkan.c:960-965 (`VK_STRUCTURE_TYPE_CREATE_INFO_WINE_DEVICE_CALLBACK`).

Net effect: the app gets an ordinary winevulkan `VkInstance`/`VkDevice`, and the host runtime saw the creation happen through its own `xrCreateVulkan*KHR` entry points. `context.ret` carries the XrResult back up (openxr_loader.c:1788-1793).

### Extension smuggling for `vulkan_enable` (non-2) path

For apps that create the device themselves, the runtime's required *device* extensions are Linux-only names the PE winevulkan would reject. So:

- PE `xrGetVulkanDeviceExtensionsKHR` (openxr_loader.c:1901-1930) calls the host (part of expected init sequence), then **replaces the returned string with the single fake extension** `"VK_WINE_openxr_device_extensions"` and stashes the real host list into the *unix* environment via ntdll's Valve-private export `__wine_set_unix_env("__WINE_OPENXR_VK_DEVICE_EXTENSIONS", buffer)` (also at init, openxr_loader.c:234).
- When the app then enables that fake extension at `vkCreateDevice`, win32u (win32u/vulkan.c:953-957) sees `has_VK_WINE_openxr_device_extensions`, does `getenv("__WINE_OPENXR_VK_DEVICE_EXTENSIONS")`, and enables the real host extensions instead. The analogous instance-side pair `VK_WINE_openxr_instance_extensions` / `__WINE_OPENXR_VK_INSTANCE_EXTENSIONS` exists in win32u/vulkan.c:693-697.

### Swapchain images for Vulkan sessions

For `SESSION_TYPE_VULKAN`, `xrEnumerateSwapchainImages` is a **pure passthrough** (openxr_loader.c:1172-1177): the host runtime's `XrSwapchainImageVulkanKHR.image` handles go straight to the app. This is legal because 64-bit winevulkan does **not** wrap non-dispatchable handles (`VkImage` is just a uint64 shared between client and host). The app uses those images on its winevulkan `VkDevice` whose host device is the same host `VkDevice` the runtime knows.

---

## 4. D3D11 / D3D12 emulation (the critical part for the macOS port)

The unix side advertises and substitutes extensions in `wine_xrCreateInstance` / `wine_xrEnumerateInstanceExtensionProperties` via the `substitute_extensions` table (openxr.c:21-29):

```
XR_KHR_D3D11_enable  -> XR_KHR_vulkan_enable
XR_KHR_D3D12_enable  -> XR_KHR_vulkan_enable
XR_KHR_win32_convert_performance_counter_time -> XR_KHR_convert_timespec_time (removed+force-advertised)
```

So the app enables `XR_KHR_D3D11_enable`; the host instance is created with `XR_KHR_vulkan_enable`. Enumeration re-injects the win32 names (openxr.c:145-204).

### `xrGetD3D11/D3D12GraphicsRequirementsKHR` (openxr_loader.c:1672-1751)

Entirely PE-side, never touches the runtime: enumerate DXGI adapters via `CreateDXGIFactory1`/`IDXGIFactory1_EnumAdapters`, match `VendorId`/`DeviceId` against the Vulkan physical device's `vendorID/deviceID` cached in the registry (`g_physdev_vid/pid`, see section 5), return that adapter's `AdapterLuid`; falls back to adapter 0. `minFeatureLevel` is hardcoded `D3D_FEATURE_LEVEL_10_0` (D3D11) / `11_0` (D3D12).

### D3D11 session creation (openxr_loader.c:586-616) -- exact DXVK interop calls

```c
ID3D11Device_QueryInterface(device, &IID_IDXGIVkInteropDevice2, &wine_instance->dxvk_device);
/* fail => XR_ERROR_VALIDATION_FAILURE: "Only DXVK is supported" */
dxvk_device->GetVulkanHandles(&binding.instance, &binding.physicalDevice, &binding.device);
dxvk_device->GetSubmissionQueue2(NULL /*pQueue*/, &binding.queueIndex, &binding.queueFamilyIndex);
do_vulkan_init(...);                      /* runs the mandated GetVulkanGraphicsRequirements/
                                             InstanceExtensions/GraphicsDevice/DeviceExtensions
                                             sequence, openxr_loader.c:513-562 */
binding.physicalDevice = wine_instance->vk_phys_dev;   /* phys dev chosen by the runtime */
create_info.next = &binding;              /* XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR */
```

DXVK's D3D11 device handles are winevulkan client handles, so the standard unwrap in openxr.c:110-122 applies when the rewritten binding crosses to the unix side.

### D3D11 swapchains

- `xrCreateSwapchain` (openxr_loader.c:982-1026): translate `createInfo->format` DXGI->Vk via `map_format_dxgi_to_vulkan` (openxr_loader.c:816-858, 12 formats), OR-in `XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT`, sanity-check depth-vs-color usage, then create the host swapchain (Vulkan).
- `xrEnumerateSwapchainFormats` (openxr_loader.c:913-980): enumerate host Vulkan formats, map each through `map_format_vulkan_to_dxgi` (:860-911), drop unmappables.
- `xrEnumerateSwapchainImages` (openxr_loader.c:1153-1345): first call enumerates host images as `XrSwapchainImageVulkanKHR`, then for each `VkImage` builds a `D3D11_TEXTURE2D_DESC1` from the cached create-info (`Usage = D3D11_USAGE_DEFAULT`, `BindFlags` from `d3d11usage_from_XrSwapchainUsageFlags` :1082-1106) and calls
  **`IDXGIVkInteropDevice2::CreateTexture2DFromVkImage(&desc, vkImage, &texture)`** (:1225-1226).
  The resulting `ID3D11Texture2D*` array is cached and returned as `XrSwapchainImageD3D11KHR`.

### D3D11 frame-loop synchronization -- no fences, no keyed mutexes

Synchronization is purely **submission-queue locking** around every host call that internally submits to the shared VkQueue (`lock_d3d_queue`/`unlock_d3d_queue`, openxr_loader.c:1347-1369):

- `xrAcquireSwapchainImage`: `LockSubmissionQueue()` -> host acquire -> `ReleaseSubmissionQueue()` (:1392-1399).
- `xrReleaseSwapchainImage`: `FlushRenderingCommands()` (forces DXVK to flush pending CS work so the rendered frame is actually submitted) then `LockSubmissionQueue()` -> host release -> `ReleaseSubmissionQueue()` (:1444-1464, drain_queue=TRUE path).
- `xrBeginFrame` / `xrEndFrame`: lock without flush around the host call (:1495-1498, :1664-1668).

That's the whole contract: DXVK's internal submission thread is excluded while the OpenXR runtime submits its own work to the same `VkQueue`. Image layout correctness is left to the runtime/DXVK (D3D11 path records **no** transition command buffers).

### D3D12 session (vkd3d-proton), openxr_loader.c:617-678

```c
QueryInterface(IID_ID3D12DXVKInteropDevice2 || IID_ID3D12DXVKInteropDevice);
QueryInterface(IID_ID3D12DeviceExt1);
d3d12_device->GetVulkanHandles(&binding.instance, &binding.physicalDevice, &binding.device);
device_ext->GetVulkanQueueInfoEx(their_binding->queue, &vk_queue, &queue_index, &queue_flags, &binding.queueFamilyIndex);
vkCreateCommandPool(vk_device, ..., &vk_command_pool);   /* for layout transitions */
```

D3D12 swapchain images: `ID3D12DeviceExt1::CreateResourceFromBorrowedHandle(&desc1, vkImage, &resource)` (:1291), returned as `XrSwapchainImageD3D12KHR`. Because D3D12 has explicit resource states, wineopenxr additionally records two command buffers per image (`record_transition_command`, :1108-1151 -- a single `vkCmdPipelineBarrier`):
- `cmd_release[i]`: layout `GetVulkanImageLayout(texture, RENDER_TARGET|DEPTH_WRITE)` -> `COLOR_ATTACHMENT_OPTIMAL`/`DEPTH_STENCIL_ATTACHMENT_OPTIMAL` (what the runtime expects),
- `cmd_acquire[i]`: the inverse.

Frame loop (D3D12): acquire submits `cmd_acquire[index]` on the vkd3d queue under `LockVulkanQueue`/`LockCommandQueue` (:1402-1423, with an acquired-ring bookkeeping and `XR_ERROR_CALL_ORDER_INVALID` guards); release submits `cmd_release[index]` *before* the host release, under `LockCommandQueue` (drain path), reverting the transition if the host call fails (:1446-1481).

### QPC time emulation (openxr.c:369-448)

`XR_KHR_win32_convert_performance_counter_time` is implemented on top of host `XR_KHR_convert_timespec_time`: compute `qpc_to_monotonic_offset()` from `NtQueryPerformanceCounter` vs `clock_gettime(CLOCK_MONOTONIC)` (wine's QPC is monotonic-based), convert QPC<->timespec, then call `xrConvertTimespecTimeToTimeKHR`/`xrConvertTimeToTimespecTimeKHR`.

---

## 5. Runtime discovery, registration, and init sequencing

### Unix side (host runtime discovery)

Nothing custom: `grep XR_RUNTIME_JSON wineopenxr/` -> no hits. `wineopenxr.so` links the stock **Khronos loader built for Linux** (`-lopenxr_loader`), so discovery is the loader's standard behavior -- `XR_RUNTIME_JSON` env override, else `active_runtime.json` under `XDG_CONFIG_DIRS`/`/etc/xdg`/`~/.config/openxr/1/`. Wine passes the Unix environment through to the unix side, so the user's/SteamVR's runtime registration is simply visible via `getenv` -- **env var passthrough by construction, not by code**.

### Windows side (registering wineopenxr as the Windows runtime)

- `ValveSoftware/wine loader/wine.inf.in:1538`:
  `HKLM,Software\Khronos\OpenXR\1,"ActiveRuntime",,"C:\openxr\wineopenxr64.json"` -- baked into every prefix at wineboot.
- Proton's `proton` script copies the manifest: `try_copy(dist/share/openxr/wineopenxr64.json, "drive_c/openxr", ...)` (proton:1134-1136).
- The game ships/loads the stock Windows `openxr_loader.dll`, reads `ActiveRuntime`, loads `C:\windows\system32\wineopenxr.dll`, and negotiates via **`xrNegotiateLoaderRuntimeInterface`** (openxr_loader.c:1860-1899): validates the `XrNegotiateLoaderInfo`/`XrNegotiateRuntimeRequest` structs (loader_structs.h) and returns `xrGetInstanceProcAddr` plus `runtimeApiVersion` (1.1.x, with fallback to 1.0 if the host rejects 1.1 -- openxr_loader.c:317-322).

### Pre-init handshake (the `HKCU\Software\Wine\VR` dance)

Because Linux SteamVR's `xrCreateInstance` hangs if SteamVR isn't running (comment at openxr_loader.c:102-107), instance creation is gated on a registry handshake produced by **vrclient** at prefix startup:

1. `vrclient_x64/vrclient_main.c:334ff` (`vrclient_init_registry`, called on VR game launch) creates volatile `HKCU\Software\Wine\VR` and spawns `initialize_vr_data`, which checks OpenVR availability and then `LoadLibraryW(L"wineopenxr")` + calls the private export **`wineopenxr_init_registry()`** (vrclient_main.c:313-323), finally sets `state=1`.
2. `wineopenxr_init_registry` (openxr_loader.c:1977-2010) runs `get_extensions()` (:238-458): creates a throwaway **host** XrInstance (`XR_KHR_vulkan_enable` only) + PE VkInstance, runs `xrGetSystem`, `xrEnumerateViewConfigurations`, `xrGetVulkanGraphicsRequirementsKHR`, `xrGetVulkanInstanceExtensionsKHR`, `xrGetVulkanGraphicsDeviceKHR`, `vkGetPhysicalDeviceProperties`, and the raw unix `xrGetVulkanDeviceExtensionsKHR`; writes `openxr_vulkan_instance_extensions`, `openxr_vulkan_device_extensions`, `openxr_vulkan_device_vid`, `openxr_vulkan_device_pid` into the key.
3. On first real use, `wine_openxr_init_once` -> `get_vulkan_extensions()` (openxr_loader.c:101-201) blocks on `RegNotifyChangeKeyValue` until `state != 0`, then loads the cached values and pushes the device list to the unix env (`__wine_set_unix_env`, :234).

### DXVK's role (why D3D11 devices are XR-capable at all)

Valve's DXVK fork (`ValveSoftware/dxvk src/dxvk/dxvk_openxr.cpp`, `DxvkXrProvider`) loads `wineopenxr.dll` inside the game process and calls the two exports **`__wineopenxr_GetVulkanInstanceExtensions` / `__wineopenxr_GetVulkanDeviceExtensions`** (openxr_loader.c:1934-1975) to pre-enable the runtime-required Vulkan instance/device extensions on the DXVK device it creates for the game -- before the game ever calls xrCreateSession. The device call returns the fake `VK_WINE_openxr_device_extensions` name, resolved by win32u as described in section 3.

---

## 6. What will NOT port to macOS directly

1. **winevulkan unix internals.** `openxr.c` includes `wine/vulkan_driver.h` and dereferences `struct vulkan_instance/device/physical_device` (`->host.instance`, `->physical_devices[i].client...`). In CrossOver, winevulkan sits on MoltenVK, and these internal structs exist too (CrossOver tracks upstream wine), but they are **private, version-locked ABI** -- a port must either build in-tree against CrossOver's wine source or replace this with the vulkan driver funcs route. The handle-unwrap concept itself carries over 1:1.
2. **The `VkCreateInfoWineInstanceCallback` / `VkCreateInfoWineDeviceCallback` mechanism** (`VK_STRUCTURE_TYPE_CREATE_INFO_WINE_INSTANCE_CALLBACK`) is a **Valve-only patch** to winevulkan/win32u (win32u/vulkan.c:702, :960, `winevk.xml`). Upstream wine and CrossOver do not have it. Options: (a) patch CrossOver's win32u the same way; (b) skip `XR_KHR_vulkan_enable2` and only expose `XR_KHR_vulkan_enable` (apps then call `vkCreateInstance` themselves and you only need the extension-smuggling trick); (c) since OXRSys supports `XR_KHR_vulkan_enable2`, emulate enable(1) on top of enable2 on the unix side if needed -- but exposing vulkan_enable2 to the *Windows* app without the callback patch is the hard case.
3. **`__wine_set_unix_env`** -- Valve-private ntdll export (`ValveSoftware/wine dlls/ntdll/env.c`, `ntdll.spec`). Not in upstream/CrossOver. Replacement: pass the extension strings through your own unixlib call and stash them in unix-side globals (you control both sides), or `setenv` from your own unix code.
4. **The `HKCU\Software\Wine\VR` + vrclient handshake** -- SteamVR-specific workaround. For OXRSys just probe the runtime directly in `wineopenxr_init_registry`/first use (drop the RegNotify wait loop).
5. **DXVK/vkd3d-proton interop** -- CrossOver ships DXVK (D3D11) on MoltenVK, but `IDXGIVkInteropDevice2::CreateTexture2DFromVkImage` and `GetSubmissionQueue2` are **Valve DXVK additions** (upstream DXVK has only `IDXGIVkInteropDevice`(1)); you must carry those DXVK patches (small) or fall back to creating the D3D11 texture normally and copying (slow). vkd3d-proton does not run on MoltenVK today -- **drop D3D12 for the initial port**.
6. **`VK_KHR_xlib_surface` / X11 assumptions** (openxr.c:329, and the win32_surface append in `wine_xrGetVulkanInstanceExtensionsKHR`) -- replace with `VK_EXT_metal_surface`/`VK_MVK_macos_surface` reasoning; still append `VK_KHR_win32_surface` on the PE-facing string since Windows apps expect it, but ensure winevulkan maps it.
7. **`-lopenxr_loader` unix link** -- fine on macOS: build the Khronos OpenXR-SDK loader as a macOS dylib/static lib; it discovers OXRSys via `XR_RUNTIME_JSON` or `/usr/local/share/openxr/1/active_runtime.json` (the loader's macOS paths). Point `XR_RUNTIME_JSON` at OXRSys's manifest.
8. **ELF/unixlib specifics**: the unixlib mechanism itself (`__wine_unix_call`, `.so` per module) works in CrossOver on macOS (Mach-O host side). The QPC math in `qpc_to_monotonic_offset` (openxr.c:373-384) assumes wine QPC == CLOCK_MONOTONIC-derived; on macOS wine's QPC is mach_absolute_time-based -- recheck and use the mac equivalent. If OXRSys lacks `XR_KHR_convert_timespec_time`, you must synthesize XrTime conversion differently.
9. **32-bit games**: wineopenxr is 64-bit only; keep that constraint.

---

## 7. Minimal-port function list

### Must hand-implement (~15, the session/swapchain/graphics-requirements path)

| # | Function | Why / what it does |
|---|---|---|
| 1 | `xrNegotiateLoaderRuntimeInterface` (PE) | Entry point from the Windows loader; struct validation + return your `xrGetInstanceProcAddr` |
| 2 | `xrGetInstanceProcAddr` (PE) + availability check (unix) | Dispatch; force-advertise the 4 emulated functions |
| 3 | `xrEnumerateInstanceExtensionProperties` (unix) | Advertise `XR_KHR_D3D11_enable` (+win32 QPC ext) on top of host list |
| 4 | `xrCreateInstance` (PE wrapper alloc + unix) | Extension substitution D3D11->vulkan(2); fill host dispatch table |
| 5 | `xrDestroyInstance` (PE + generated unix) | Wrapper teardown, interop device release |
| 6 | `xrGetSystem` (PE thunk) | Cache `systemId` in the instance wrapper |
| 7 | `xrGetD3D11GraphicsRequirementsKHR` (PE only) | DXGI adapter LUID matching against Vk vendor/device id |
| 8 | `xrGetVulkanGraphicsRequirementsKHR` + `xrGetVulkanInstanceExtensionsKHR` + `xrGetVulkanDeviceExtensionsKHR` (unix + PE) | String rewriting (win32_surface append; fake device extension + smuggling into winevulkan) -- one work item, three functions |
| 9 | `xrGetVulkanGraphicsDeviceKHR` (unix) | Host->client VkPhysicalDevice re-wrap |
| 10 | `xrCreateSession` (PE + unix) | The heart: DXVK `QueryInterface`/`GetVulkanHandles`/`GetSubmissionQueue2`, binding rewrite, winevulkan unwrap |
| 11 | `xrDestroySession` + session list management | Needed for `xrPollEvent` reverse mapping; note the 2026-08 use-after-free fix (remove from list on destroy) |
| 12 | `xrEnumerateSwapchainFormats` (PE) | Vk->DXGI format filtering |
| 13 | `xrCreateSwapchain` / `xrDestroySwapchain` (PE) | DXGI->Vk format map, `MUTABLE_FORMAT` bit, wrapper caching of create-info |
| 14 | `xrEnumerateSwapchainImages` (PE) | Enumerate host `VkImage`s -> `CreateTexture2DFromVkImage` -> cached `XrSwapchainImageD3D11KHR` array |
| 15 | `xrAcquireSwapchainImage` / `xrReleaseSwapchainImage` / `xrBeginFrame` / `xrEndFrame` (PE) | Queue lock/flush discipline (`FlushRenderingCommands` + `Lock/ReleaseSubmissionQueue`) and, in `xrEndFrame`, the composition-layer deep-rewrite (`convert_XrCompositionLayer`, openxr_loader.c:1502-1633: swapchain handle substitution incl. projection views / depth-info / space-warp chains) |
| 16 | `xrPollEvent` (PE) | Host->wrapper session patching for the 6 session events |

(Plus, if you keep `vulkan_enable2` exposed to Windows apps: `xrCreateVulkanInstanceKHR`/`xrCreateVulkanDeviceKHR` with either the win32u callback patch or an alternate approach; plus the two QPC time converters if any target game uses them -- Unreal does.)

### Near-pass-through (generate or macro-stamp)

Everything else -- the other ~440 functions are mechanical: pack params -> `UNIX_CALL` -> unwrap at most one leading `XrInstance`/`XrSession`/`XrSwapchain` handle -> call host. All input/output structs, `next` chains, spaces, actions, hand-tracking, etc. pass through untouched on 64-bit. For a *minimal* port you don't even need the full set: the core-1.0 function list (~60 functions: spaces, reference spaces, views, actions, action sets, suggested bindings, haptics, `xrLocateViews`, `xrWaitFrame`, `xrBeginSession`, `xrEndSession`, `xrRequestExitSession`, `xrWaitSwapchainImage`, `xrStringToPath`/`xrPathToString`, `xrGetSystemProperties`, `xrEnumerateViewConfiguration*`, `xrEnumerateEnvironmentBlendModes`, etc.) is enough for most D3D11 VR games, and each is a 5-line thunk pair. Reusing/porting `make_openxr` (it needs only `xr.xml` and Python) is the sane route rather than writing thunks by hand.

### Suggested macOS deltas (summary)

- Replace SteamVR gating with direct OXRSys probing; keep `wineopenxr_init_registry`-style caching of vid/pid + instance extensions (you still need the DXGI LUID match for `xrGetD3D11GraphicsRequirementsKHR`).
- Set `HKLM\Software\Khronos\OpenXR\1\ActiveRuntime = C:\openxr\wineopenxr64.json` via your bottle's inf/registry, ship the json pointing at `C:\windows\system32\wineopenxr.dll`.
- Unix side: link the Khronos loader, `XR_RUNTIME_JSON` -> OXRSys manifest; OXRSys speaks `XR_KHR_vulkan_enable2` over MoltenVK, so provide the host-side `vulkan_enable`(1) surface by internally using enable2 if OXRSys lacks enable(1).
- Carry Valve's small DXVK patches (`IDXGIVkInteropDevice2::GetSubmissionQueue2` + `CreateTexture2DFromVkImage`) into CrossOver's DXVK, and the `DxvkXrProvider` extension pre-enable hook.
- Defer D3D12 (vkd3d-proton/MoltenVK gap) and 32-bit.
