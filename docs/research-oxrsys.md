# OXRSys Runtime — Deep-Read Report (bridge-oriented)

Repo cloned at `/tmp/research-oxrsys/` (shallow, default branch, read 2026-09-07). All paths below are relative to the repo root; line numbers are from this checkout.

---

## 1. Runtime loading contract

**Dylib name & exports.** The runtime builds as a single shared library `liboxrsys-runtime.dylib` (`runtime/CMakeLists.txt:106-118` — `OUTPUT_NAME oxrsys-runtime`, `PREFIX "lib"`, `SUFFIX ".dylib"` on APPLE). Symbol visibility is hidden (`CXX_VISIBILITY_PRESET hidden`, `runtime/CMakeLists.txt:107-108`); **the only exported symbol is `xrNegotiateLoaderRuntimeInterface`** (`runtime/src/EntryPoint.cpp:3776-3802`, `__attribute__((visibility("default")))` at 3776). There is also a hidden `__attribute__((destructor))` cleanup on unload (`EntryPoint.cpp:3770-3774` → `CleanupRuntimeState()` at 149-164).

**Manifest.** `runtime/oxrsys-runtime.json.in:1-7`:

```json
{
    "file_format_version": "1.0.0",
    "runtime": {
        "name": "OXRSys Runtime",
        "library_path": "@CMAKE_CURRENT_BINARY_DIR@/@OXRSYS_RUNTIME_LIBRARY_FILENAME@"
    }
}
```

The configured manifest is generated as `build/runtime/oxrsys-runtime.json` next to the dylib (`runtime/CMakeLists.txt:120-131`) with an **absolute** `library_path`. Discovery is via:
- `XR_RUNTIME_JSON` env var (docs: `docs/architecture.md:95`; README:26), or
- a user-level active-runtime symlink `~/.config/openxr/1/active_runtime.json` created by `scripts/oxrsys_runtime_default.sh` (lines 10-12), which additionally installs a LaunchAgent `net.demonixis.oxrsys.runtime-env` doing `launchctl setenv XR_RUNTIME_JSON <manifest>` for GUI apps (script lines 39-63).

**Negotiation.** `xrNegotiateLoaderRuntimeInterface` (`EntryPoint.cpp:3777-3802`) validates struct type/version/size and interface versions, then returns:
- `runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION` (3796)
- `runtimeApiVersion = XR_CURRENT_API_VERSION` (3797) — the OpenXR SDK is fetched at tag **release-1.1.57** (`CMakeLists.txt:81-84`), so the runtime claims **OpenXR 1.1.57**.
- `getInstanceProcAddr = OxrGetInstanceProcAddr` (3798), the static dispatcher at `EntryPoint.cpp:3617-3760`.

Caveat: negotiation **fails** if `loaderInfo->minApiVersion >= XR_MAKE_VERSION(1,1,0)` (`EntryPoint.cpp:3790-3791`).

`xrCreateInstance` accepts any requested `apiVersion` with major==1 and `<= XR_CURRENT_API_VERSION` (`EntryPoint.cpp:602-607`). Only **one instance** (`gInstance`, `XR_ERROR_LIMIT_REACHED` at 577-579) and **one session** (899-902) may exist — all state is file-scope globals in EntryPoint.cpp (49-98).

Config gating: `xrCreateInstance` returns `XR_ERROR_RUNTIME_UNAVAILABLE` if `runtime_enabled=false` in the TOML config (`EntryPoint.cpp:609-614`). Config file is `~/Library/Application Support/OXRSys/oxrsys-runtime.toml` on macOS, falling back to the dylib's own directory (`runtime/src/Config.cpp:222-241`, `runtime/src/RuntimePlatform.cpp:68-119`; dylib dir found via `dladdr` at `RuntimePlatform.cpp:122-140`).

---

## 2. Vulkan binding path on macOS

Both `XR_KHR_vulkan_enable` and `XR_KHR_vulkan_enable2` are advertised (`EntryPoint.cpp:249-252`). Design principle (`runtime/CMakeLists.txt:84-87`, `docs/architecture.md:59-63`): **the runtime never links or dlopens the Vulkan loader**; every Vulkan call goes through `VulkanDispatch` function pointers.

### Who creates VkInstance/VkDevice

Both models are supported:

- **v2 path (runtime creates on behalf of app):**
  - `OxrCreateVulkanInstanceKHR` (`EntryPoint.cpp:3374-3455`): stores `createInfo->pfnGetInstanceProcAddr` into `gVulkanDispatch.getInstanceProcAddr` (3389), resolves `vkCreateInstance`/`vkEnumerateInstanceExtensionProperties` with `VK_NULL_HANDLE` (`VulkanDispatch.h:32-42`), then calls `vkCreateInstance` itself (3442) and loads instance-level functions (3451).
  - `OxrCreateVulkanDeviceKHR` (`EntryPoint.cpp:3471-3540`): calls `vkCreateDevice` (3528) with a modified extension list.
  - `OxrGetVulkanGraphicsDevice2KHR` (3457) forwards to the v1 device picker.
- **v1 path (app creates everything):** `OxrGetVulkanInstanceExtensionsKHR` and `OxrGetVulkanDeviceExtensionsKHR` return **empty strings** (`EntryPoint.cpp:3238-3292` — "Godot/apps handle portability enumeration/subset themselves in the v1 path").

In **both** cases the session binding is `XrGraphicsBindingVulkanKHR` chained to `xrCreateSession` (`EntryPoint.cpp:882-897`); the runtime copies `instance/physicalDevice/device/queueFamilyIndex/queueIndex` into a `VulkanGraphicsContext` (930-935) and constructs the session with `GraphicsContext::Vulkan(vulkanContext, gMetalDevice)` (936-937). `xrCreateSession` requires a prior `xrGetVulkanGraphicsRequirements[2]KHR` (`XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING`, 890-893). Requirements report Vulkan 1.0.0–1.3.0 (`EntryPoint.cpp:3355-3357`). `OxrGetVulkanGraphicsDeviceKHR` returns `devices[0]` from `vkEnumeratePhysicalDevices` (3309-3319).

### How VulkanDispatch acquires function pointers

`VulkanDispatch` (`runtime/src/VulkanDispatch.h:12-67`) is a **global** (`gVulkanDispatch`, `EntryPoint.cpp:109`). Resolution order, in `ResolveVulkanGetInstanceProcAddrFromProcess` (`runtime/src/VulkanDispatch.cpp:16-36`):

1. If the app provided `pfnGetInstanceProcAddr` (v2 `xrCreateVulkanInstanceKHR`), reuse it (VulkanDispatch.cpp:19-22).
2. Otherwise (v1 path): **POSIX:** `dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr")` (VulkanDispatch.cpp:33-34) — assumes a Vulkan loader **or ICD exporting that symbol is already loaded in the flat process image**. **Windows scaffold:** `GetModuleHandleW(L"vulkan-1.dll")` + `GetProcAddress` (24-31). It deliberately never `dlopen`s a loader.

`EnsureVulkanInstanceDispatch` (`EntryPoint.cpp:111-144`) runs this lazily from `xrGetVulkanGraphicsDeviceKHR` (3304) and `xrCreateSession` (925); `LoadInstanceFunctions(vkInstance)` resolves `vkDestroyInstance`, `vkEnumeratePhysicalDevices`, `vkGetPhysicalDeviceProperties`, `vkGetPhysicalDeviceMemoryProperties`, `vkGetDeviceProcAddr`, `vkCreateDevice` (`VulkanDispatch.h:44-63`). Device-level functions are resolved per-device via `vkGetDeviceProcAddr` in `Swapchain.mm:117-147` (`vkCreateImage`, `vkDestroyImage`, `vkGetImageMemoryRequirements`, `vkAllocateMemory`, `vkFreeMemory`, `vkBindImageMemory`, `vkExportMetalObjectsEXT`), cached in a global `gDeviceFuncs` keyed by last device (`Swapchain.mm:114-123`).

**Key assumption:** whatever `vkGetInstanceProcAddr` is resolved must dispatch correctly for the **exact handle values** the app passes in the graphics binding — app and runtime share one flat dylib namespace with one MoltenVK.

### MoltenVK-specific handling

- **`VK_KHR_portability_enumeration` injection (v2 instance):** availability probed via `vkEnumerateInstanceExtensionProperties` (`EntryPoint.cpp:3404-3420`); if present, `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` is set and the extension appended (3422-3438).
- **`VK_KHR_portability_subset` + `VK_EXT_metal_objects` injection (v2 device, `#if defined(__APPLE__)`):** `EntryPoint.cpp:3488-3517`. The v1 path injects **nothing** — an app that doesn't itself enable `VK_EXT_metal_objects` yields `vkGetDeviceProcAddr(dev,"vkExportMetalObjectsEXT") == NULL`, silently disabling streaming (see §3).
- **MTLTexture extraction from VkImage:** at swapchain creation, not per-frame. Images are created with a chained `VkExportMetalObjectCreateInfoEXT{VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT}` (`Swapchain.mm:321-328`) and, after memory bind, `vkExportMetalObjectsEXT` with `VkExportMetalTextureInfoEXT` extracts an `id<MTLTexture>` per color image, retained into `textures_[i]` (`Swapchain.mm:367-386`). Depth images are skipped (368).
- **Metal device for the Vulkan session:** `EnsureMetalDevice()` (`EntryPoint.cpp:3216-3234`) dlopens `/System/Library/Frameworks/Metal.framework/Metal` and calls `MTLCreateSystemDefaultDevice` — this `gMetalDevice` (runtime-created, *not* MoltenVK's internal device object) is what the encoder/debug renderer uses (`EntryPoint.cpp:937`, `VideoEncoder.mm:436-444`).

---

## 3. Swapchain lifecycle (Vulkan path, macOS)

macOS compiles only `Swapchain.mm` (`runtime/CMakeLists.txt:22-26`); `SwapchainVulkan.cpp`/`FfmpegVideoEncoder.cpp` are Linux-only (27-31). One `Swapchain` class handles both APIs, branching on `graphicsContext.api` (`Swapchain.mm:184-197`).

**Formats:** `OxrEnumerateSwapchainFormats` (`EntryPoint.cpp:1055-1115`) offers, for Vulkan: `VK_FORMAT_B8G8R8A8_SRGB(50)`, `B8G8R8A8_UNORM(44)`, `R8G8B8A8_SRGB(43)`, `R8G8B8A8_UNORM(37)`, `D32_SFLOAT(126)`, `D32_SFLOAT_S8_UINT(130)` (1077-1084). The app's requested format is used verbatim; `createInfo->usageFlags` are **ignored**.

**xrCreateSwapchain:** `Session::CreateSwapchain` (`Session.cpp:802-813`) → `Swapchain::InitVulkan` (`Swapchain.mm:287-396`). **The runtime allocates the VkImages itself**: `vkCreateImage` with `VK_IMAGE_TYPE_2D`, app format, `arrayLayers = createInfo->arraySize`, `TILING_OPTIMAL`, usage `COLOR_ATTACHMENT | TRANSFER_SRC | SAMPLED` (color) or `DEPTH_STENCIL_ATTACHMENT | SAMPLED` (depth) (305-319); memory `DEVICE_LOCAL` via `vkAllocateMemory`/`vkBindImageMemory` (346-365). Image count is **3** (`Swapchain::SwapchainImageCount`, `Swapchain.h:86`) or 1 for `XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT` (Swapchain.mm:295).

**xrEnumerateSwapchainImages:** returns `XrSwapchainImageVulkanKHR{.image = vkImages_[i]}` (`Swapchain.mm:484-492`).

**Acquire/Wait/Release:** pure CPU ring state machine under `stateMutex_` (`Swapchain.mm:502-647`): Acquire picks next `Available` slot (525-545); Wait flips `Acquired→Waited` and **performs no GPU wait** (549-570); Release marks `Available` and records `lastReleasedIndex_` (585-594).

**How images reach the encoder:**
- **Metal sessions:** on Release, blit copy of the released texture into a staging texture on the **app-provided `MTLCommandQueue`**, signaling an `MTLSharedEvent` (`Swapchain.mm:600-644`); the encoder GPU-waits on it (`EncodeWaitForFrameImage`, `VideoEncoder.mm:259-273`). Staging slots are leased so async encode outlives the frame (`Swapchain.mm:43-54`).
- **Vulkan sessions (macOS): no copy, no readback, no sync.** `InitMetalStaging` is only called from the Metal init path (`Swapchain.mm:233`), so `stagingSlots_` is empty; `GetLastReleasedFrameImageSource` falls through to returning the **live** MoltenVK-extracted `MTLTexture` for `textures_[lastReleasedIndex_]` with an **empty sync token** (`Swapchain.mm:710-725`; comment `Swapchain.h:74-77`: "Vulkan currently returns the live image handle as the Linux readback path is still scaffolded"). This is the macOS Vulkan gap: the encoder can sample a swapchain slot while the app re-renders it, and there is no Vulkan→Metal queue synchronization or image-layout handling. The README/architecture Linux caveat ("real Vulkan image readback still pending", `docs/architecture.md:63`) is about the Linux `SwapchainVulkan.cpp` path; on macOS the mechanism is zero-copy `VK_EXT_metal_objects` aliasing instead of readback.
- If `vkExportMetalObjectsEXT` is unavailable, `textures_[i]` stays null (`Swapchain.mm:303, 368-386`), the FrameImageSource is invalid, and `StreamingServer::SendFrame` silently drops every frame (`frameSource.IsStereoValid()` fails, `StreamingServer.cpp:2361`). App still runs; headset gets no video.

**Composite/encode flow:** `Session::EndFrame` → `ValidateProjectionLayer` pulls a `FrameImageSource` per eye (view 0→left, 1→right; `Session.cpp:655-665`) → `StreamingServer::SendFrame` tags the frame with the render-time head pose and `PushLatest`es into a latest-frame-only `StreamingFrameQueue` (`StreamingServer.cpp:2357-2408`) → `EncodeThread` pops (`StreamingServer.cpp:1242-1312`) → `VideoEncoder::EncodeStereo` treats `FrameImageSource.image` as `id<MTLTexture>` unconditionally (`VideoEncoder.mm:774-775`), scales/packs both eyes via MPS/compute into an IOSurface-backed `CVPixelBuffer` (pool with `kCVPixelBufferIOSurfacePropertiesKey`/`kCVPixelBufferMetalCompatibilityKey`, `VideoEncoder.mm:476-484`; mapped to Metal via `CVMetalTextureCache`, 465-470/765-767), then encodes H.265 with a VideoToolbox `VTCompressionSession` (`VideoEncoder.mm:~560-663`).

---

## 4. Process architecture

**Entirely in-process.** No compositor daemon, no IPC. The loader dlopens `liboxrsys-runtime.dylib` into the app; `Session::BeginSession` spins up the `StreamingServer` inside the app process (`Session.cpp:199-200`, `889-915`). Worker threads (`StreamingServer.h:269-276`): broadcast, control, encode, videoSend, tcpControl/tcpVideo/tcpTracking/tcpSpatial, plus the frame queue. Frames travel: `xrEndFrame` (app thread, non-blocking enqueue) → `EncodeThread` (GPU blit + VideoToolbox) → `VideoSendThread` → network to the Quest client (ports 9943-9948, `common/protocol/include/oxrsys/protocol/Protocol.h:15-20`; UDP discovery broadcast + TCP/UDP video/tracking, plus USB ADB-reverse TCP). IOSurface/CVPixelBuffer are only VideoToolbox inputs in-process, not cross-process sharing.

**OXRSys Home** (`clients/Apple/oxrsys-home/`, `clients/Qt/oxrsys-home/`) is a separate launcher/configurator (registers `XR_RUNTIME_JSON`, LaunchAgent, ADB USB reverse, launches apps) — **not** in the frame path (`docs/architecture.md:95-97`, README:13-17).

---

## 5. Session / frame loop

- **`Session::WaitFrame`** (`Session.cpp:339-402`): frame pacing by **sleeping on the calling thread** to the target period (default 90 Hz, or the client-negotiated refresh from `streamingServer_->GetTargetRefreshRateHz()`, 362-377). Calls `inputManager_->Update(dt)` (383). `predictedDisplayTime` = ns of `steady_clock::now() - startTime_` (session-relative epoch; `GetCurrentTime()` 144-150), `predictedDisplayPeriod` = 1/refresh (385-390). It's "now", not a pipelined prediction.
- **`BeginFrame`** (404-429): requires prior WaitFrame (`waitedFrameCount_`), `XR_FRAME_DISCARDED` on double-begin.
- **`EndFrame`** (431-576): validates blend mode (OPAQUE always; ALPHA_BLEND only with passthrough enabled — 50-59, 457-460), layer count vs `XR_MIN_COMPOSITION_LAYERS_SUPPORTED` (461-464), and layers: `PROJECTION` (exactly 2 views, 632-635; poses/FOV finite; sub-images validated against owned swapchains with a released image, 587-622) and `QUAD` (671-689 — validated but **not streamed**); anything else → `XR_ERROR_LAYER_INVALID` (516-517). Projection views feed the stereo `FrameSource`; enqueue is non-blocking (533-567). Session state auto-advances per submitted frame READY→SYNCHRONIZED→VISIBLE→FOCUSED (`AdvanceSessionStateAfterFrameSubmission`, 750-800); events via `Instance::PushEvent`/`xrPollEvent` (`Session.cpp:152-169`).
- **`LocateViews`** (696-748): PRIMARY_STEREO only (711-714); always reports orientation+position valid/tracked (724-725). Eye poses from `InputManager::GetEyeViews` (`InputManager.cpp:354+`): head pose ± IPD/2, FOV from the client's streamed per-eye FOV when available, else ~100° default (`InputManager.h:153-156`). Snapshots `lastRenderHeadPose_` so the streamed frame is tagged with the exact rendered pose (742-745; consumed at `StreamingServer.cpp:2383-2396` for client-side reprojection).
- **Pose/input return path:** Quest client sends `TrackingPacket`s → `TrackingReceiver` (owned by `StreamingServer`) → wired into `InputManager` on client connect (`Session::CheckStreamingConnection`, `Session.cpp:917-940`) → `InputManager::UpdateFromStreaming` (`InputManager.cpp:208+`) refreshes head/controller poses, buttons, hand tracking. Actions resolved in `OxrSyncActions` (`EntryPoint.cpp:2311-2363`) by matching stored suggested bindings (`EntryPoint.cpp:1746-1829`) against InputManager component queries, with hand-interaction vs controller priority (2148-2155). Known profiles: `IsKnownInteractionProfilePath` (`EntryPoint.cpp:1430-1477`; 1.1-only profiles gated on instance API version).

---

## 6. Extension surface

`GetSupportedExtensionInfos()` (`EntryPoint.cpp:236-254`) — the full advertised list on macOS:

| Extension | Notes |
|---|---|
| `XR_KHR_metal_enable` | + Unity alias `XR_KHRX2_metal_enable` and `xrGetMetalGraphicsRequirementsKHRX2` (`EntryPoint.cpp:210-214`, 3735-3742) |
| `XR_EXT_hand_tracking` | 241 |
| `XR_EXT_conformance_automation` | 242 |
| `XR_EXT_hand_interaction` | 243 |
| `XR_EXT_debug_utils` | 244 |
| `XR_META_touch_controller_plus` | only if SDK headers define it (245-247) |
| `XR_KHR_vulkan_enable` | 250 |
| `XR_KHR_vulkan_enable2` | 251 |

Plus `xrLocateSpacesKHR` dispatched as alias of core 1.1 `xrLocateSpaces` (215, 3691), gated to apiVersion ≥ 1.1 (339-348). No API layers (`OxrEnumerateApiLayerProperties` returns 0, 558-568). **API version claimed:** `XR_CURRENT_API_VERSION` of OpenXR SDK **release-1.1.57** (`CMakeLists.txt:84`, `EntryPoint.cpp:3797`). System: `systemId` fixed 1, `systemName "OXRSys Runtime"`, max swapchain 4096², stereo-only, recommended 1512×1680/eye (`Instance.cpp:27-190`, `Instance.h:78-79`). Reference spaces: VIEW/LOCAL/STAGE (+LOCAL_FLOOR on 1.1) (`EntryPoint.cpp:1192-1230`). Notably absent: `XR_KHR_composition_layer_depth`, `XR_KHR_visibility_mask`, `XR_FB_display_refresh_rate`, any `xrConvertTime*` extension.

---

## 7. Bridge-relevant risk list (Wine/CrossOver PE caller)

1. **Winevulkan handle wrapping (highest risk).** The runtime consumes raw `VkInstance/VkPhysicalDevice/VkDevice` from `XrGraphicsBindingVulkanKHR` (`EntryPoint.cpp:930-935`) and calls MoltenVK on them (`EnsureVulkanInstanceDispatch`, 111-144). A PE app's Vulkan handles are winevulkan *wrapper* handles with PE-side dispatch; passing them to native MoltenVK misdispatches. The proxy must translate to the native MoltenVK handles before forwarding the binding — and likewise for the `pfnGetInstanceProcAddr` in `XrVulkanInstanceCreateInfoKHR` (PE function pointers use ms_abi and dispatch wrapped handles; `gVulkanDispatch` would call them with SysV ABI and native handles). Practical answer: the unixlib proxy supplies its **own** native `vkGetInstanceProcAddr` (from the unix-side MoltenVK already loaded by winevulkan) and native handles, never the PE ones.
2. **`dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr")` (v1 path, `VulkanDispatch.cpp:33-34`).** In a CrossOver process the unix side has winevulkan's MoltenVK loaded; RTLD_DEFAULT may resolve `vkGetInstanceProcAddr` from **MoltenVK's ICD export directly** (bypassing loader/layers) or from a Vulkan loader — whichever appears first in the flat namespace. That pfn must match the origin of the `VkInstance` passed in; if the proxy forwards native handles created by that same MoltenVK it coincidentally works, but it's fragile. Prefer forcing the v2 path (`xrCreateVulkanInstanceKHR` with an explicit native gipa) so `gVulkanDispatch.getInstanceProcAddr` is pinned deterministically (`EntryPoint.cpp:3389`).
3. **v1 path never enables `VK_EXT_metal_objects`.** `xrGetVulkanDeviceExtensionsKHR` returns an empty list (`EntryPoint.cpp:3266-3292`), yet streaming depends on `vkExportMetalObjectsEXT` (`Swapchain.mm:143, 303, 368-386`). If the native VkDevice isn't created with `VK_EXT_metal_objects` (+`VK_KHR_portability_subset`), video output silently disappears (frames dropped at `StreamingServer.cpp:2361`). The bridge must force-enable those device extensions itself, and confirm CrossOver's bundled MoltenVK version supports `VK_EXT_metal_objects`.
4. **No GPU sync on the Vulkan streaming path.** The encoder samples the live MoltenVK-aliased `MTLTexture` with no wait (`Swapchain.mm:710-725`, empty sync token; contrast the Metal path's staging blit + `MTLSharedEvent`, `Swapchain.mm:600-644`). Under Wine the extra scheduling jitter makes read-during-rerender more likely. No API-level mitigation exists; this needs an upstream fix (per-release blit like the Metal path).
5. **Two Metal devices.** Encoder/debug rendering use a runtime-created `MTLCreateSystemDefaultDevice` (`EntryPoint.cpp:3216-3234`, `VideoEncoder.mm:436-444`) while swapchain textures belong to MoltenVK's internal `MTLDevice`. Fine on single-GPU Apple Silicon; invalid cross-device texture use on any multi-GPU Mac or if CrossOver pins a different device.
6. **Threading/blocking.** `xrWaitFrame` **sleeps on the calling thread** (`Session.cpp:373-377`) — a PE app thread sleeps inside a unixlib call; acceptable, but the proxy must not hold Wine's loader lock there. The runtime spawns ~8 unix pthreads from inside `xrBeginSession` (`StreamingServer.cpp:706-711, 951-956`); they never call back into app code.
7. **Objective-C autorelease pools.** ObjC is used on app-called paths (`[queue commandBuffer]` in `Swapchain::ReleaseImage`, `Swapchain.mm:619-631`; CoreVideo/VideoToolbox elsewhere) with manual retain/release but no `@autoreleasepool` at entry points. Wine threads have no pools → autoreleased objects leak. The bridge should wrap forwarded calls in `objc_autoreleasePoolPush/Pop`.
8. **No AppKit/main-thread requirement.** AppKit is linked (`runtime/CMakeLists.txt:98`) but grep shows **zero** `NSApplication/NSWindow/NSScreen/NSEvent` use in `runtime/src/`; Metal/VideoToolbox/CoreVideo use is thread-agnostic. No app-bundle assumption: paths derive from `$HOME` (`RuntimePlatform.cpp:61-119`) and the dylib directory via `dladdr` (122-140).
9. **Environment & filesystem/network.** Needs unix `$HOME`; writes config/log/status under `~/Library/Application Support/OXRSys/` (`Config.cpp:237-241`). Opens listening sockets (UDP 9943 broadcast, 9944-9948) inside the game process — expect macOS local-network permission prompts attributed to the Wine binary.
10. **Timing domain.** `XrTime` is session-relative `steady_clock` ns (`Session.cpp:144-150`). Opaque round-trips are fine, but there is no `xrConvertWin32PerformanceCounterTimeKHR`/timespec support — apps/shims requiring them get `XR_ERROR_FUNCTION_UNSUPPORTED` (`EntryPoint.cpp:3758-3759`).
11. **Global singletons and unload.** One instance/one session (`EntryPoint.cpp:577, 899`); dylib `__attribute__((destructor))` (3770-3774) tears down at unload — avoid explicit `dlclose`. Handles are pointers cast to `uint64_t` (`EntryPoint.cpp:170-178`) — 64-bit PE only; wow64 32-bit apps would truncate handles unless the proxy maps them.
12. **Negotiation gate:** a proxy re-implementing loader negotiation must pass `minApiVersion < 1.1.0` (`EntryPoint.cpp:3790-3791`) and the exact struct sizes of the unix-side `openxr_loader_negotiation.h`.
13. **Swapchain usage flags ignored; formats fixed.** Only the 6 formats at `EntryPoint.cpp:1077-1084` (no RGBA16F, common in UE); usage is hardwired to `COLOR_ATTACHMENT|TRANSFER_SRC|SAMPLED` (`Swapchain.mm:314-317`) — no `TRANSFER_DST`/`STORAGE`/mutable-format, which D3D11→Vulkan translation layers often request.

---

## 8. Minimal proxy call sequence (hello_xr-style Vulkan app, enable2 path)

Everything is reached through the negotiated dispatcher `OxrGetInstanceProcAddr` (`EntryPoint.cpp:3617-3760`).

1. `dlopen("liboxrsys-runtime.dylib")` → `xrNegotiateLoaderRuntimeInterface` — `EntryPoint.cpp:3777-3802`.
2. `xrEnumerateInstanceExtensionProperties` — `EntryPoint.cpp:526` (confirm `XR_KHR_vulkan_enable2`).
3. `xrCreateInstance` with `XR_KHR_vulkan_enable2` — `EntryPoint.cpp:570-636`.
4. `xrGetSystem` (`HEAD_MOUNTED_DISPLAY` → systemId 1) — `EntryPoint.cpp:665` → `Instance.cpp:27-42`.
5. `xrEnumerateViewConfigurationViews` (PRIMARY_STEREO, 2×1512×1680) — `EntryPoint.cpp:807` → `Instance.cpp:148-189`.
6. `xrGetVulkanGraphicsRequirements2KHR` — `EntryPoint.cpp:3366-3372` (**mandatory** before session, checked 890-893).
7. `xrCreateVulkanInstanceKHR` (pass a **native** `pfnGetInstanceProcAddr`) — `EntryPoint.cpp:3374-3455`.
8. `xrGetVulkanGraphicsDevice2KHR` — `EntryPoint.cpp:3457-3469` (returns `devices[0]`).
9. `xrCreateVulkanDeviceKHR` — `EntryPoint.cpp:3471-3540` (injects portability_subset + metal_objects).
10. `xrCreateSession` with `XrGraphicsBindingVulkanKHR{instance, physicalDevice, device, queueFamilyIndex, queueIndex}` — `EntryPoint.cpp:839-957` (Vulkan branch 922-941).
11. `xrCreateReferenceSpace` (LOCAL/STAGE) — `EntryPoint.cpp:1232` → `Session.cpp:828-860`.
12. `xrEnumerateSwapchainFormats` — `EntryPoint.cpp:1055-1115`.
13. `xrCreateSwapchain` — `EntryPoint.cpp:1117` → `Swapchain.mm:287-396`.
14. `xrEnumerateSwapchainImages` (`XrSwapchainImageVulkanKHR`) — `EntryPoint.cpp:1143` → `Swapchain.mm:454-496`.
15. Input: `xrStringToPath` (754) / `xrCreateActionSet` (1608) / `xrCreateAction` (1671) / `xrSuggestInteractionProfileBindings` (1746) / `xrAttachSessionActionSets` (1831) / `xrCreateActionSpace` (dispatch 3705).
16. `xrPollEvent` until `XR_SESSION_STATE_READY` (pushed at session creation, `Session.cpp:112-113`) → `xrBeginSession` — `EntryPoint.cpp:974` → `Session.cpp:171-204` (starts the streaming server).
17. Per frame: `xrWaitFrame` (1005 → `Session.cpp:339`) → `xrBeginFrame` (1016) → `xrLocateViews` (1038 → `Session.cpp:696`) → per swapchain `xrAcquireSwapchainImage` (1155) / `xrWaitSwapchainImage` (1166) / render / `xrReleaseSwapchainImage` (1177) → `xrEndFrame` with one 2-view `XrCompositionLayerProjection` (1027 → `Session.cpp:431`). Interleave `xrSyncActions` (2311) + `xrGetActionState*` / `xrLocateSpace` (1257).
18. Teardown: `xrRequestExitSession` (995) → poll to `STOPPING` → `xrEndSession` (985) → `xrDestroySession` (959) → `xrDestroyInstance` (642).

For the **v1** (`XR_KHR_vulkan_enable`) variant, replace 6-9 with `xrGetVulkanGraphicsRequirementsKHR` (3330), `xrGetVulkanInstanceExtensionsKHR` (3238, returns ""), app/proxy-side `vkCreateInstance`, `xrGetVulkanGraphicsDeviceKHR` (3294), `xrGetVulkanDeviceExtensionsKHR` (3266, returns "") and app/proxy-side `vkCreateDevice` — **the proxy must itself add `VK_EXT_metal_objects` + `VK_KHR_portability_subset` to the device**, since the runtime won't (risk #3).
