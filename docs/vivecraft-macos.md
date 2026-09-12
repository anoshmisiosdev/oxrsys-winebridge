# Vivecraft (Minecraft) on macOS via OpenComposite + OXRSys

Vivecraft talks OpenVR through LWJGL. On macOS there is no SteamVR, so we load
OpenComposite's `libopenvr_api.dylib` instead, which translates OpenVR to OpenXR
and renders through OXRSys.

## Why it failed before

```
java.lang.UnsatisfiedLinkError: Failed to locate library: libopenvr_api.dylib
```

Vivecraft only bundles x86_64 macOS natives (`macos/x64/org/lwjgl/openvr/`), and
Prism was launching an arm64 JVM, so LWJGL never found them. The OXRSys runtime is
x86_64-only anyway (it is loaded in-process), so the JVM has to be x86_64 under
Rosetta.

## Setup

1. Build the dylib: `scripts/build-opencomposite-macos.sh`
   (output: `opencomposite/build-macos/bin/libopenvr_api.dylib`).
2. Install an x86_64 JDK 17 (Temurin), e.g. under
   `~/Library/Application Support/PrismLauncher/java/temurin-17-x64/`.
3. In the Prism instance: Settings -> Java
   - Java installation: the x86_64 `.../Contents/Home/bin/java`
   - JVM arguments: add
     `-Dorg.lwjgl.openvr.libname=/absolute/path/to/opencomposite/build-macos/bin/libopenvr_api.dylib`
   (LWJGL loads that file instead of the bundled Valve library.)
4. Make sure the OXRSys server is running and the headset is connected, then
   launch the instance. Vivecraft should start in VR mode.

Logs: `~/.local/state/OpenComposite/logs/opencomposite.log`.

## How the graphics path works

OpenXR has no OpenGL graphics binding for macOS and OXRSys only offers Metal and
Vulkan swapchains. OpenComposite therefore:

- creates the session with `XR_KHR_metal_enable` (`TemporaryMetal`), and keeps
  that session for GL apps instead of recreating it with an app binding;
- per eye, blits the app's GL texture into an IOSurface-backed
  `GL_TEXTURE_RECTANGLE` (flipped vertically, GL is bottom-up), waits on a GL
  fence, then blit-copies the IOSurface (as an `MTLTexture`) into the Metal
  swapchain image on the session's command queue (`GLMetalCompositor`).

Interfaces Vivecraft/LWJGL asks for but OpenComposite doesn't implement
(IVRDebug, IVRNotifications, IVRTrackedCamera, ...) now return
`InterfaceNotFound` instead of aborting; LWJGL tolerates that.
