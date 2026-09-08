# oxrsys-winebridge

A `wineopenxr`-style bridge for macOS: lets Windows OpenXR/OpenVR apps running
under Wine/CrossOver on Apple Silicon use [OXRSys](https://github.com/demonixis/OXRSys)
(native macOS OpenXR runtime, MoltenVK/Metal, Quest streaming) as their runtime.

## Target chain

    Windows VR game (x86-64 PE, D3D11)
      -> CrossOver / Wine + Rosetta 2
      -> OpenComposite / xrizer     (OpenVR -> OpenXR, PE)
      -> DXVK                       (D3D11 -> Vulkan, PE)
      -> THIS BRIDGE                (PE proxy runtime + in-process native call)
      -> MoltenVK                   (Vulkan -> Metal)
      -> OXRSys                     (XR_KHR_vulkan_enable2, VK_EXT_metal_objects)
      -> VideoToolbox -> Quest

## Status

Design phase. Research reports in docs/.

## Milestone 1

hello_xr.exe (Vulkan backend) under CrossOver renders on a Quest via OXRSys.
No DXVK, no OpenVR shim — proves the unixlib + VkImage handoff.
