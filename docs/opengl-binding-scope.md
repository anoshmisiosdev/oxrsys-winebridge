# Scope: `XR_KHR_opengl_enable` for OXRSys (native macOS OpenGL OpenXR)

**Goal:** let a *native* macOS OpenGL OpenXR app (e.g. Minecraft Java + the Visor/
AtumVR mod, via LWJGL) render through OXRSys with no Wine — the app submits GL
textures, OXRSys composites/encodes/streams them to the headset.

**Verdict: feasible, but large and partly non-standard, spanning THREE codebases.**
Not a config tweak or a single-file feature. The blocker below is why no native
macOS OpenGL OpenXR runtime exists today.

---

## The load-bearing problem: OpenXR has no macOS OpenGL binding

`XR_KHR_opengl_enable` defines binding structs for **Win32, Xlib, Xcb, Wayland
only** (`openxr_platform.h:106-174`). There is **no** `XrGraphicsBinding­OpenGL­MacOSKHR`
(CGL/NSOpenGL). By contrast the macOS-native `XrGraphicsBindingMetalKHR` *does*
exist (`openxr_platform.h:367`).

Consequences:
- To pass a macOS `CGLContextObj` to a runtime you must invent a **non-standard**
  binding struct (call it `XrGraphicsBindingOpenGLMacOSKHR`).
- **LWJGL** (`org.lwjgl.openxr`) only exposes the four standard GL bindings — it has
  no macOS GL binding to hand to the runtime. So even a GL-capable OXRSys can't be
  reached by the unmodified Visor/LWJGL app.

So this feature is not "add a binding to OXRSys" — it's "define a private macOS GL
binding and implement it in OXRSys **and** in LWJGL **and** get the mod to use it."

---

## Work breakdown

### A. OXRSys runtime (the tractable part — we own it)
All in `runtime/src/`, mirroring the existing Metal/Vulkan plumbing:

1. **Extension advertisement** — add `XR_KHR_opengl_enable` under a new
   `XR_USE_GRAPHICS_API_OPENGL` guard in `GetSupportedExtensionInfos()`
   (`EntryPoint.cpp:234`). *(S)*
2. **`GraphicsApi::OpenGL`** in `GraphicsTypes.h:8` + a `GraphicsContext::OpenGL(...)`
   carrying the `CGLContextObj`. *(S)*
3. **Graphics requirements** — `xrGetOpenGLGraphicsRequirementsKHR` +
   `HasQueriedOpenGLGraphicsRequirements`, mirroring
   `OxrGetMetalGraphicsRequirementsKHR` (`EntryPoint.cpp:3537`) and its dispatch/
   `GetInstanceProcAddr` wiring (`:3724`). *(S)*
4. **Session binding** — accept the custom `XrGraphicsBindingOpenGLMacOSKHR` in
   `xrCreateSession` (`EntryPoint.cpp:855-925`); retain the `CGLContextObj`. *(M)*
5. **GL swapchain images** — the hard part. `Swapchain.mm` currently makes
   `MTLTexture`s (`:220`) or `VkImage`s (`:291`). Add a GL path that creates
   **IOSurface-backed GL textures** the app renders into and returns them as
   `XrSwapchainImageOpenGLKHR` (a `GLuint`) from `EnumerateImages` (`:458`). Each
   image is one `IOSurface` exposed **two ways**: to the app as a GL texture
   (`CGLTexImageIOSurface2D` → `GL_TEXTURE_RECTANGLE`), and to OXRSys as an
   `MTLTexture` (`[device newTextureWithDescriptor:iosurface:plane:]`). *(L)*
6. **GL→Metal for compose/encode** — because each swapchain image is IOSurface-
   backed, the runtime already has an `MTLTexture` view, so the existing Metal
   staging-pool + encoder path (`Swapchain.mm:600+`) works **once a GL fence/glFlush
   is bridged** so the encoder doesn't read before the app's GL draw completes.
   This is the same sync gap `NOTE-oxrsys.md` flags for the Vulkan path; needs a
   `GLsync`→wait (or `glFlushRenderAPPLE`) before the staging blit. *(M)*
7. **Format negotiation** — map GL internal formats (`GL_SRGB8_ALPHA8`, `GL_RGBA8`)
   ↔ IOSurface/`MTLPixelFormat`; the app renders GL, we encode Metal. *(M)*
8. **arm64 build** — OXRSys is built **x86_64** for the Rosetta/Wine host; a native
   arm64 JVM can't load an x86_64 dylib. Add an arm64 (or universal) runtime build +
   an arm64 `oxrsys-runtime.json`/manifest. The runtime is native code so this is a
   build-config change, not a port. *(S–M)*

### B. Native macOS OpenXR loader (we can build)
- Build the Khronos loader **arm64** (`libopenxr_loader.dylib`) — we already build it
  x86_64 in `build/x86/_deps/openxr-build/`. Point LWJGL at it
  (`-Dorg.lwjgl.openxr.libname=…`) and set `XR_RUNTIME_JSON` to the OXRSys manifest. *(S)*

### C. App side — LWJGL + the Visor/AtumVR mod (we don't own; hardest)
- Add the `XrGraphicsBindingOpenGLMacOSKHR` struct + CGL plumbing to `org.lwjgl.openxr`
  (a fork of LWJGL), or hand-roll the struct via LWJGL's raw memory API inside the mod.
- Get AtumVR (`me.phoenixra.atumvr`) to build that binding on macOS and pass the CGL
  context. This is a Java/mod change on code we don't control. *(L, and out of our hands)*

---

## Effort & risk

| Piece | Size | Risk |
|---|---|---|
| A1-A4, A8, B (extension, reqs, binding, arm64 build, loader) | ~1 week | low — mirrors existing code |
| A5-A7 (IOSurface GL↔Metal swapchain + sync + formats) | ~1-2 weeks | **medium-high** — GL is deprecated on macOS, IOSurface/GL sync edge cases |
| C (LWJGL macOS GL binding + mod) | unknown | **high** — non-standard, third-party Java, may need upstreaming or a fork |

**Total: multi-week, three codebases, one non-standard extension.** The OXRSys half
(A+B) is very doable and self-contained. The app half (C) is the real gamble: without
a macOS GL binding in LWJGL, the mod literally cannot address the runtime, so this
only pays off if we're willing to fork/patch LWJGL and the mod too.

## Cheaper alternatives to weigh first
- **Windows Minecraft under CrossOver** doesn't help — Minecraft Java is OpenGL on
  Windows too, so it hits the same GL-vs-DXMT(D3D) wall in the existing pipeline.
- **Vulkan path (already supported):** if Minecraft ran on Vulkan (VulkanMod) *and* a
  Vulkan-binding VR mod existed, it would use OXRSys's existing `XR_KHR_vulkan_enable`
  with no new binding — but VulkanMod + a VR mod that hooks the GL pipeline don't
  coexist in practice.

## Recommendation
If the goal is specifically native Minecraft-Java VR: **A+B is a clean, worthwhile
addition to OXRSys** (unlocks native GL OpenXR broadly), but budget it as a real
feature and validate the LWJGL/mod (C) feasibility *first* with a throwaway spike —
because if the app can't be made to pass a macOS GL binding, the runtime work has no
consumer. Suggested order: (1) prove a minimal native GL OpenXR test app can hand
OXRSys a CGL binding at all → (2) then build the OXRSys GL swapchain/interop.
