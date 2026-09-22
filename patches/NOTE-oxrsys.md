# Note for the OXRSys author (draft — not sent)

_Updated 2026-09-22: item 1 is implemented on `feat/shared-swapchain-textures`._

Context: OXRSys works as the OpenXR runtime behind a Wine bridge on macOS.
We run Windows D3D11 titles through Wine + monofunc/wineopenxr + DXMT, with
OXRSys as the native runtime underneath; the full chain renders end-to-end
(verified 2026-09-07, 900 frames, both eyes). Two observations from that
integration work, referenced against the OXRSys tree (runtime/src):

## 1. Swapchain images should be shareable, with PixelFormatView

Swapchain images were created as plain private textures with only
`MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead`. Two consequences
for anything outside the runtime's own Metal stack:

- A plain (non-shared) texture cannot produce a `MTLSharedTextureHandle`, so
  an interop consumer in another graphics stack cannot adopt the image and has
  to blit into it instead of rendering into it directly.
- Without `MTLTextureUsagePixelFormatView` a consumer cannot create a view in a
  different-but-compatible pixel format. Metal exempts same-family sRGB casts,
  so the runtime's own use is fine, but a D3D11 translation layer resolving a
  typeless `DXGI_FORMAT_*_TYPELESS` desc does create such a view.

Proposed (and implemented on our branch `feat/shared-swapchain-textures`):
allocate the images with `newSharedTextureWithDescriptor:` and add
`MTLTextureUsagePixelFormatView`. Both cost nothing on Apple GPUs for these
formats, and the shared allocation falls back to a private texture if a
descriptor is ever unshareable. With that in place a Wine/D3D11 client renders
zero-copy straight into the runtime's swapchain images, with no changes to the
D3D11->Metal translation layer at all.

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
