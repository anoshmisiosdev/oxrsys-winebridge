# OpenComposite: OpenVR→OpenXR shim, cross-built for the bottle

OpenComposite is a drop-in replacement for `openvr_api.dll` that translates a
game's OpenVR (SteamVR) API calls into OpenXR. Combined with our wineopenxr
bridge as the bottle's active OpenXR runtime, this lets SteamVR-only titles run
against OXRSys with no SteamVR installation at all:

```
game ──OpenVR ABI──▶ OpenComposite openvr_api.dll ──OpenXR──▶ wineopenxr bridge ──▶ OXRSys
```

We build the 64-bit Windows PE DLL on macOS with Homebrew mingw-w64
(GCC 15.2), the same toolchain used for the bridge PE side. 32-bit is
deferred (would need `i686-w64-mingw32-*` plus the `Lib32` Vulkan import lib;
OXRSys is 64-bit-only anyway — handles are raw pointers in `uint64_t`).

## Getting the source

`opencomposite/` is intentionally **not** tracked by this repo (see
`.gitignore`); it is a patched clone. To recreate it:

```sh
cd ~/oxrsys-winebridge
git clone --depth 1 --recurse-submodules --shallow-submodules \
    https://github.com/aashishvasu/OpenComposite opencomposite
# Vulkan headers + import libs, exactly what upstream AppVeyor CI uses.
# (Not a submodule; extracted into libs/vulkan, which upstream .gitignore's.)
cd opencomposite
curl -sSLO https://znix.xyz/random/vulkan-1.1.85.0-minisdk.7z
7z x -olibs vulkan-1.1.85.0-minisdk.7z
```

Base revision at time of writing: `7db7fdb` ("Merge branch 'openxr'") on the
default `main` branch of the aashishvasu fork — `main` **is** the OpenXR line
(the old znixian GitLab repo kept it on an `openxr` branch).

Then re-apply our MinGW patches (kept as commits in the clone; regenerate with
`git format-patch` from a built clone, or redo by hand from the list below).

## Building

```sh
cd ~/oxrsys-winebridge/opencomposite
cmake -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$HOME/oxrsys-winebridge/bridge/cmake/x86_64-w64-mingw32.cmake \
    -DCMAKE_BUILD_TYPE=Release
ninja -C build
# Output: build/bin/vrclient_x64.dll (the DLL is one and the same as
# openvr_api.dll — upstream CI just renames it; we keep a stripped copy):
cp build/bin/vrclient_x64.dll build/bin/openvr_api.dll
x86_64-w64-mingw32-strip --strip-unneeded build/bin/openvr_api.dll
```

Verification (both pass on the current build):

```sh
file build/bin/openvr_api.dll
#   PE32+ executable (DLL) (console) x86-64, for MS Windows
x86_64-w64-mingw32-objdump -p build/bin/openvr_api.dll | grep -E "VR_InitInternal2|VR_GetGenericInterface"
#   both exported (27 exports total: VR_* C API + interface factory exports)
x86_64-w64-mingw32-objdump -p build/bin/openvr_api.dll | grep "DLL Name"
#   only system DLLs + vulkan-1.dll — libgcc/libstdc++/winpthread are linked
#   statically, UCRT via api-ms-win-crt-* (CrossOver provides these)
```

## Patches applied (commits in the clone, oldest first)

Upstream targets MSVC on Windows and GCC only on Linux; every patch closes a
gap between those two configurations. All are upstreamable.

1. **`CMake: support MinGW-w64 cross builds targeting Windows`**
   - Define `WIN32` globally: CMake's MSVC platform files add `/DWIN32`, the
     GNU ones don't, and sources guard Windows code with `#ifdef WIN32` as
     well as `#ifdef _WIN32`.
   - `-fvisibility=hidden` moved from `add_definitions` to
     `add_compile_options` — `add_definitions` leaks it to `windres`, which
     dies on `invalid option -f`.
   - Explicitly link `d3d11 d3d12 dxgi d3dcompiler dbghelp shlwapi` — MSVC
     gets these from `#pragma comment(lib, …)`, GCC ignores that pragma.
   - Wrap `OCCore`/`DrvOpenXR` in `-Wl,--start-group` on any GNU toolchain
     (they reference each other's symbols; MSVC's linker resolves this
     itself, GNU ld is order-sensitive).
   - `-static -static-libgcc -static-libstdc++` on the final link so the DLL
     needs no MinGW runtime DLLs next to the game exe.
   - `#include <shlwapi.h>` in `DrvOpenXR.cpp` for `PathStripPathA`.
2. **`Guard atlbase.h includes behind _MSC_VER`** — MinGW has no ATL. The only
   ATL users (DX10 compositor, pre-XR-port VRKeyboard code) are compiled out
   in this configuration anyway (`SUPPORT_DX10` unset, `OC_XR_PORT` always
   defined); WRL `ComPtr`, which mingw-w64 does ship, covers the rest.
3. **`logging: portable varargs macros and Windows flush`** —
   `##__VA_ARGS__` so `OOVR_ABORTF("msg")` (no varargs) compiles under GCC;
   `_commit()` instead of the POSIX-only `fsync()` on Windows.
4. **`Portability: wide-path fstreams and CD3D11_VIEWPORT for libstdc++/MinGW`**
   — open fstreams from `std::wstring` via `std::filesystem::path` (MSVC's
   `wchar_t*` fstream overloads are non-standard); hand-rolled full-texture
   viewport where mingw-w64's `d3d11.h` lacks the `CD3D11_VIEWPORT` helper.

No `-fpermissive`, no header shims, no disabled graphics APIs: DX11/DX12/
Vulkan/GL support all compile (`GRAPHICS_API_SUPPORT_FLAGS` unchanged).

## Install into a game

```sh
# Windows-style path (bottle auto-detected when only one exists, else --bottle):
scripts/install-opencomposite.sh 'C:\Program Files (x86)\Steam\steamapps\common\ExampleGame'
# POSIX path works too. Restore stock SteamVR openvr_api.dll:
scripts/install-opencomposite.sh --restore 'C:\Program Files (x86)\Steam\steamapps\common\ExampleGame'
```

The script backs up every 64-bit `openvr_api.dll` it finds (game root,
`bin/win64/`, and any other tree locations except `win32`/`x86` dirs) to
`openvr_api.dll.stock` before replacing it, and refuses nothing silently.
OpenComposite reads an optional `opencomposite.ini` next to the DLL.

## Known limitations against OXRSys (via the wineopenxr bridge)

OpenComposite enables optional OpenXR extensions only when the runtime offers
them, so absences degrade rather than crash:

- **`XR_KHR_visibility_mask` — absent in OXRSys.** OpenComposite treats it as
  optional ("no big deal"); `IVRSystem::GetHiddenAreaMesh` returns an empty
  mesh, so games render the full per-eye viewport. Correct output, small
  fill-rate cost.
- **`XR_KHR_composition_layer_depth` — absent, and irrelevant:**
  OpenComposite never submits depth layers (no reference to the extension in
  the codebase). `Submit()` with a depth texture is simply not forwarded.
- **Swapchain formats: OXRSys offers only 6** (no RGBA16F; usage flags
  hardwired to `COLOR_ATTACHMENT|TRANSFER_SRC|SAMPLED`). The DX11/DX12
  compositors derive the swapchain format from the game's submitted texture
  (sRGB/linear pairs of RGBA8/BGRA8 and friends) without consulting
  `xrEnumerateSwapchainFormats`; a game submitting e.g. FP16 HDR textures
  will abort at `xrCreateSwapchain`. Extend the format table in the bridge
  (`bridge/src/include/formats.h`) if a title needs it.
- **SteamVR-isms that have no OpenXR equivalent** are stubbed by
  OpenComposite itself (works, returns defaults): overlays are partial
  (`BaseOverlay`), Chaperone bounds are synthesized from the XR stage bounds,
  `IVRRenderModels` serves generic models, `IVRSettings` reads
  `opencomposite.ini`, and camera/screenshot/notification interfaces are
  no-ops. Vendor extensions it can use (`XR_EXT_hand_tracking`,
  `XR_MNDX_xdev_space`, `XR_EXT_hp_mixed_reality_controller`) are all
  optional and absent in OXRSys.
- **API version:** OpenComposite requests OpenXR 1.0; OXRSys negotiation
  rejects loaders demanding ≥ 1.1 — not an issue here.
- **Hard requirement:** the bridge must expose `XR_KHR_D3D11_enable` (and
  `XR_KHR_D3D12_enable` for DX12 titles). D3D11 is the tested bridge path;
  everything else (GL/Vulkan submissions inside the bottle) is untested.
- **Untested at runtime.** Built and export-verified only; no game has been
  launched with it yet.

## 32-bit (deferred)

Homebrew mingw-w64 ships the `i686-w64-mingw32` toolchain and the mini-SDK
contains `Lib32/vulkan-1.lib`, so the recipe should transfer: new build dir,
an i686 clone of the toolchain file, output `build/bin/vrclient.dll`. Blocked
on OXRSys being 64-bit-only regardless.
