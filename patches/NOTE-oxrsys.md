# Note for the OXRSys author (draft — not sent)

Context: OXRSys works as the OpenXR runtime behind a Wine bridge on macOS.
We run Windows D3D11 titles through Wine + monofunc/wineopenxr + DXMT, with
OXRSys as the native runtime underneath; the full chain renders end-to-end
(verified 2026-09-07, 900 frames, both eyes). Two observations from that
integration work, referenced against the OXRSys tree (runtime/src):

## 1. Swapchain texture usage flags vs external interop consumers

Swapchain images are created with only
`MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead`
(`Swapchain.mm:217`, and the staging copies at `Swapchain.mm:257`).

That is correct and sufficient for sRGB<->linear reinterpretation — Metal
exempts same-family sRGB casts from `MTLTextureUsagePixelFormatView` — but
external consumers that import these textures (e.g. a D3D11 translation
layer resolving a typeless `DXGI_FORMAT_*_TYPELESS` desc) tend to expect
`PixelFormatView` for any format casting and reject the import. We hit
exactly this in DXMT and relaxed its import validation upstream, but it
would help other interop consumers to either:

- add `MTLTextureUsagePixelFormatView` to the swapchain image descriptor
  (cost is typically nil on Apple GPUs for these formats), or
- document the format/usage contract for external consumers: images are
  concrete (possibly sRGB) formats, `RenderTarget|ShaderRead` only, and
  same-family sRGB casts are the only reinterpretation guaranteed to work.

## 2. Known gap: Vulkan-path streaming lacks GPU sync

The Metal path snapshots each released image through a staging pool: on
release, a blit into a staging slot plus an `MTLSharedEvent` signal is
encoded (`Swapchain.mm:599-644`), and
`GetLastReleasedFrameImageSource` returns the staged texture with a
sync token carrying that event and value (`Swapchain.mm:710-722`).

The Vulkan path never takes that branch (it is gated on
`graphicsApi_ == GraphicsApi::Metal`) and falls through to
`Swapchain.mm:724-725`:

    id<MTLTexture> texture = (__bridge id<MTLTexture>)textures_[lastReleasedIndex_];
    return MakeMetalFrameImageSource(texture, arraySize_, arrayIndex, {}, {}, 0);

i.e. the live swapchain texture with an empty `FrameSyncToken`
(`GraphicsTypes.h:50-60` — `IsValid()` is false with `waitValue == 0`), so
the encoder/streaming consumer can read the image while the app's Vulkan
(MoltenVK) work that rendered it is still in flight, and can also race the
app's next-frame render into the same image. Options: extend the staging
pool + shared-event snapshot to the Vulkan path (blit on the runtime's
Metal queue after a VkFence/timeline-semaphore wait for the release), or
export a `MTLSharedEvent` into the app's Vulkan device via
`VK_EXT_metal_objects` and signal it at release. In practice this shows up
as occasional torn/stale frames in the stream under load, not in the local
render, which may be why it has gone unnoticed.
