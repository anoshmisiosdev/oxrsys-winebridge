# oxrsys-winebridge

Run Windows OpenXR (and, via OpenComposite, OpenVR/SteamVR) apps on Apple
Silicon Macs, displayed on a Quest headset — CrossOver + a wineopenxr bridge +
[OXRSys](https://github.com/demonixis/OXRSys).

    Windows VR game (x86-64, D3D11)
      → CrossOver 26 (Rosetta 2) → DXMT fork (D3D11→Metal, zero-copy interop)
      → wineopenxr.dll (PE builtin) → __wine_unix_call → wineopenxr.so (x86_64 Mach-O)
      → Khronos openxr_loader → liboxrsys-runtime.dylib (XR_KHR_metal_enable)
      → VideoToolbox H.265 → Wi-Fi/USB → Quest

The bridge itself is [monofunc/wineopenxr](https://github.com/monofunc/wineopenxr)
(LGPL-2.1), vendored as the `bridge/` submodule. This repo adds the research,
design, OXRSys wiring, and install tooling.

## Status

- [x] M1 — bridge builds on this machine (`wineopenxr.dll` PE32+ builtin-signed,
      `wineopenxr.so` x86_64 Mach-O)
- [x] M2 — install scripts (`scripts/install.sh <Bottle>`)
- [x] M2.5 — headless smoke test PASSES in the VR bottle: PE → unixlib → OXRSys,
      XR_KHR_D3D11_enable advertised (test/smoke.c)
- [x] M3a — d3d11test.exe renders 900 frames through the full chain (headless)
- [x] M3b — visual confirmation: render observed in OXRSys Simulator (user-verified)
- [x] M4a — OpenComposite PE openvr_api.dll built (mingw, 4 upstreamable patches) + installer script
- [ ] M4b — first SteamVR title running end-to-end
- [x] M5a — DXMT interop fixes upstreamed: monofunc/dxmt PR #1
- [ ] M5b — OXRSys upstream: Vulkan-path GPU sync, formats, QPC converters (note drafted in patches/)

## Prerequisites

- CrossOver 26, macOS 15+, Apple Silicon
- [monofunc/dxmt](https://github.com/monofunc/dxmt) installed, selected as
  Graphics in the bottle (stock DXMT lacks `IMTLD3D11InteropDevice`)
- OXRSys runtime installed (universal dylib) + OXRSys Home configured, Quest
  client running
- `brew install cmake ninja mingw-w64`

## Build & install

    git submodule update --init --recursive
    cmake -B bridge/build bridge -G Ninja && cmake --build bridge/build
    ./scripts/install.sh <BottleName>     # modifies CrossOver.app payload (re-run after CX updates)

Docs: `docs/DESIGN.md` (architecture + risk register), `docs/research-*.md`
(OXRSys internals, Valve wineopenxr anatomy, macOS toolchain).
