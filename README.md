# oxrsys-winebridge

**Run Windows SteamVR / OpenXR games on an Apple Silicon Mac, streamed to a Meta
Quest — in hardware-encoded stereo.** No Windows PC, and no SteamVR (which has
had no macOS build since 2020).

```
Windows VR game (x86-64, D3D11, OpenVR or native OpenXR)
  → CrossOver (Rosetta 2)  → OpenComposite openvr_api.dll   (OpenVR → OpenXR; SteamVR titles only)
  → wineopenxr             (PE builtin → __wine_unix_call → native x86_64 .so)
  → OXRSys runtime         (native macOS OpenXR, XR_KHR_metal_enable)
  → DXMT                   (D3D11 → Metal, zero-copy IMTLD3D11InteropDevice)
  → hardware HEVC          (arm64 out-of-process VideoToolbox helper, ~5 ms)
  → USB (adb tunnel)       → OXRSys client on the Quest → display
```

**New here?** Start with **[docs/FRESH-INSTALL.md](docs/FRESH-INSTALL.md)** — the
dependency diagram and the full what-to-install checklist.

---

## Get the drop-in DLL (no build required)

SteamVR (OpenVR) titles need OpenComposite's `openvr_api.dll` dropped in beside
the game. You don't have to build it — **GitHub Actions builds it for you**:

1. **Actions tab → "Build openvr_api.dll"** → download the `openvr_api-dll-win64`
   artifact (or grab it from a tagged **Release**). It's cross-compiled to a
   Windows x86-64 DLL with mingw-w64.
2. Drop it into every installed OpenVR game in one shot:
   ```bash
   ./scripts/provision-all-steamvr.sh            # provision all games (idempotent)
   ./scripts/provision-all-steamvr.sh --restore  # put the stock DLLs back
   ```

> Built with **mingw-w64 (GCC), not MSVC**, on purpose: the MSVC↔GCC x64 ABI
> trampolines in this fork are GCC-specific and must be built with mingw g++ to be
> ABI-correct against the MSVC-compiled games the DLL is dropped into.

## Status

| Milestone | Result |
|---|---|
| Bridge builds on Apple Silicon | ✅ verified |
| PE → unixlib → native OpenXR runtime, headless | ✅ verified (`test/smoke.c`) |
| Full D3D11 render loop through DXMT → OXRSys | ✅ verified — 27,000 frames at 90 Hz |
| **Hardware HEVC** encode (arm64 out-of-process helper) | ✅ verified live — ~5 ms encode, full 2272×1264 stereo |
| Streaming to a real Quest 2 over USB | ✅ working |
| Native-OpenXR titles (e.g. Pac-Man VR) | ✅ run |
| SteamVR (OpenVR) titles via OpenComposite (SUPERHOT VR, BasaultVR) | ✅ run |
| **Headset audio** (game sound → Quest, USB) | ✅ implemented (server + client) |
| Motion-to-photon latency tuning | 🔧 ongoing (prediction horizon / reprojection) |

## Headset audio

Game audio is captured on the Mac and streamed to the headset. Both halves are in
this project (the OXRSys protocol previously only *reserved* an audio channel):

- **Server:** a Core Audio process tap on the in-process Wine host captures exactly
  the game's audio (no virtual device, no permission prompt) and streams it as
  `TcpRecordType::Audio` records over the video TCP socket.
- **Client:** the Quest app plays it via low-latency AAudio.

To use it: set `headset_audio = true` in `~/Library/Application Support/OXRSys/oxrsys-runtime.toml`,
run the audio-enabled runtime, and install the **Quest client APK** — download it
from `anoshmisiosdev/oxrsys` → Actions *"Build Android client APK"* (or the
`client-v*` Release), then `adb install -r app-release.apk`. USB transport only for now.

## Repo layout

Everything the project needs is wired into this one repo as submodules. Our own
changes live either on a fork (where they're substantial) or as a vendored patch
(where they're a one-liner) — see each row.

```
opencomposite/    submodule → anoshmisiosdev/OpenComposite  (merged-fixes)
                  OpenVR→OpenXR shim, cross-built to openvr_api.dll. Carries the
                  MSVC↔GCC ABI trampolines, controller registration, stereo fixes.
bridge/           submodule → anoshmisiosdev/wineopenxr
                  the PE↔native OpenXR bridge (+ native win32 perf-counter-time ext)
dxmt/             submodule → monofunc/dxmt  (feature/openxr) — the DEFAULT fork
                  D3D11→Metal + IMTLD3D11InteropDevice. Our single OpenXR fix is
                  applied from patches/ at build time (no personal fork to maintain).
oxrsys-src/       submodule → anoshmisiosdev/oxrsys  (fix/tracking-reconnect-loop)
                  the native macOS OpenXR runtime + arm64 HEVC encoder helper
oxrsys-src-jitter/ submodule → anoshmisiosdev/oxrsys (fix/ffe-coherent-at-scale)
                  foveated-encoding-at-scale work, kept on its own branch
test/OpenXRSamples/ submodule → anoshmisiosdev/OpenXRSamples  (touch_controller binding fix)
patches/          0001-...patch  — the one DXMT OpenXR fix, applied by build-dxmt.sh;
                  NOTE-oxrsys.md — upstream notes for the OXRSys author
docs/             FRESH-INSTALL.md (install diagram + checklist), DESIGN.md,
                  research reports, oxrsys-runtime-fixes.md
scripts/          build-dxmt.sh, install-dxmt.sh, provision-all-steamvr.sh,
                  install.sh + restore/uninstall counterparts (all idempotent)
test/             smoke.c (headless PE→unixlib proof), d3d11test.cpp (render loop)
```

## Build & install

**Prerequisites:** CrossOver, macOS 15+, Apple Silicon, Xcode with the Metal
toolchain, `brew install cmake ninja meson mingw-w64 android-platform-tools`, and
OXRSys installed as a **universal (x86_64 + arm64)** dylib — the x86_64 slice is
what runs under Rosetta; without it nothing here works.

```bash
git clone --recursive https://github.com/anoshmisiosdev/oxrsys-winebridge.git
cd oxrsys-winebridge
# (or, in an existing clone) git submodule update --init --recursive

# 1. the wineopenxr bridge — registers as the bottle's OpenXR runtime
cmake -B bridge/build bridge -G Ninja && cmake --build bridge/build
./scripts/install.sh VR                    # VR = your CrossOver bottle name

# 2. DXMT (D3D11 → Metal). Applies patches/0001 to the default monofunc fork,
#    then builds; install-dxmt.sh overlays the DLLs into CrossOver.
./scripts/build-dxmt.sh
./scripts/install-dxmt.sh

# 3. openvr_api.dll for SteamVR titles — download from CI (see above), then:
./scripts/provision-all-steamvr.sh         # drop it into every OpenVR game
```

On the Quest: enable Developer mode + USB debugging (one-time, via the Meta Quest
phone app), sideload the OXRSys client APK, connect USB. Then launch any
provisioned game from Steam — the runtime picks it up automatically.

## Why each link exists

**CrossOver + Rosetta 2** run the x86-64 Windows binary — the only way to execute
it on Apple Silicon. This is the load-bearing constraint: the OXRSys runtime dylib
is loaded *in-process*, so it must also be x86-64. Native arm64 is only possible
for **out-of-process** helpers — which is exactly why hardware HEVC encode (which
VideoToolbox refuses to a Rosetta process) runs in a separate **arm64 helper**.

**OpenComposite** translates a SteamVR game's OpenVR calls into OpenXR, dropped in
as `openvr_api.dll` next to the game. (Native-OpenXR titles skip this link.)

**wineopenxr** is the bridge: a PE DLL registered as the bottle's OpenXR runtime,
paired with a native `.so` half that talks to the real runtime over Wine's
`__wine_unix_call` — Proton's technique, reimplemented for macOS with a Metal
graphics binding, the only API that lets DXMT's texture handles cross the boundary
without a GPU copy.

**OXRSys** is the native macOS OpenXR runtime: it owns the session and swapchain,
renders the game through **DXMT** (D3D11→Metal, via `IMTLD3D11InteropDevice` —
zero-copy access to the `MTLTexture` behind a D3D11 texture), encodes, and streams
to the Quest client.

## Engineering notes (bugs found & fixed)

Each of these was root-caused with disassembly and an isolated repro before being
called a bug. Full write-ups in `docs/`.

- **Static-init-order crash in OpenComposite's logger** — a namespace-scope
  `std::ofstream` reachable before its constructor ran; invisible under MSVC,
  fatal under MinGW. Fixed (construct-on-first-use).
- **DXMT `ImportMTLTexture2D` rejected every OXRSys texture** — it compared pixel
  formats without Metal's sRGB/linear view-compatibility rule and demanded
  `PixelFormatView` on textures we don't reformat. Fixed via a narrow relaxation —
  now `patches/0001`, applied at build time (see `patches/NOTE-oxrsys.md`).
- **MSVC↔GCC x64 vtable ABI mismatch** — methods returning a struct >8 bytes by
  value put the hidden return pointer in different registers under MSVC vs GCC
  (RCX vs RDX). Crashed SteamVR titles until fixed with naked register-swap
  trampolines in OpenComposite's codegen. **This is what makes SUPERHOT VR and
  BasaultVR run.**
- **Null hidden-area-mesh crash in a UE4 title** — OXRSys doesn't implement
  `XR_KHR_visibility_mask`, so OpenComposite returned `{nullptr, 0}`; UE4's SteamVR
  plugin indexed it without a null-check. Fixed by returning a valid empty mesh.

## Credits

Built on [demonixis/OXRSys](https://github.com/demonixis/OXRSys),
[monofunc/wineopenxr](https://github.com/monofunc/wineopenxr),
[monofunc/dxmt](https://github.com/monofunc/dxmt), and
[aashishvasu/OpenComposite](https://github.com/aashishvasu/OpenComposite) — all of
which did the genuinely hard parts. This project glues them into a working chain on
Apple Silicon, plus the fixes that came from running real games through it.
