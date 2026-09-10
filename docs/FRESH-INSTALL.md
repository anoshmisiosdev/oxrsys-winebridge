# Fresh install — what you need to run Windows SteamVR games on Apple Silicon

This bridge runs **Windows OpenVR/OpenXR games on an Apple Silicon Mac** and
streams them to a **Meta Quest** headset, with **hardware HEVC** encoding — no
Windows PC, no SteamVR (which has had no macOS build since 2020).

## The two-device picture

Everything lives on two devices: your **Mac** (renders + encodes) and your
**Quest** (decodes + displays), joined by a **USB cable** (adb tunnel).

```mermaid
flowchart TB
    subgraph MAC["🖥️  Apple Silicon Mac — renders &amp; encodes"]
        direction TB
        R2["Rosetta 2<br/><i>x86_64 translation (built into macOS)</i>"]
        CX["CrossOver<br/><i>commercial x86_64 Wine host</i>"]

        subgraph BOTTLE["CrossOver bottle &quot;VR&quot;"]
            direction TB
            STEAM["Steam + your VR games"]
            OC["OpenComposite<br/><b>openvr_api.dll</b><br/><i>dropped into each OpenVR game</i>"]
            BRIDGE["wineopenxr bridge<br/><i>registered as the OpenXR ActiveRuntime</i>"]
            DXMT["DXMT<br/><i>D3D11 → Metal (CX_GRAPHICS_BACKEND=dxmt)</i>"]
        end

        OXR["<b>OXRSys runtime</b><br/>liboxrsys-runtime.dylib<br/><i>native macOS OpenXR runtime</i>"]
        HELPER["arm64 encoder helper<br/>oxrsys-encoder-helper<br/><i>out-of-process HW HEVC</i>"]
        TOML["config .toml<br/><i>~/Library/Application Support/OXRSys/</i>"]
        ADB["adb (android platform-tools)"]
    end

    subgraph QUEST["🥽  Meta Quest 2 — decodes &amp; displays"]
        direction TB
        DEV["Developer mode + USB debugging<br/><i>one-time, via Meta Quest app</i>"]
        CLIENT["<b>OXRSys client app</b> (APK)"]
    end

    STEAM --> OC --> BRIDGE --> OXR
    OXR --> DXMT
    OXR --> HELPER
    OXR --> TOML
    OXR --> ADB
    ADB <-->|"USB cable (adb reverse tunnel)"| CLIENT
    DEV -.enables.-> CLIENT
```

## Runtime data flow (what happens when you press Play in Steam)

```mermaid
flowchart LR
    G["VR game<br/>(OpenVR API)"] --> OCd["openvr_api.dll<br/>OpenComposite<br/><i>OpenVR → OpenXR</i>"]
    OCd --> BR["wineopenxr<br/><i>PE → native thunk</i>"]
    BR --> RT["OXRSys runtime"]
    RT -->|"render"| MT["DXMT<br/>D3D11 → Metal"]
    MT -->|"frame"| ENC["arm64 helper<br/>HW HEVC encode<br/><i>~5 ms</i>"]
    ENC -->|"H.265 over USB"| CL["Quest OXRSys client<br/><i>decode + reproject</i>"]
    CL --> DISP["Headset display"]
    CL -.->|"head pose (UDP)"| RT
```

## Install checklist

### On the Mac (Apple Silicon)
| # | Component | What / where | How you get it |
|---|-----------|--------------|----------------|
| 1 | **Rosetta 2** | x86_64 translation | built into macOS (`softwareupdate --install-rosetta`) |
| 2 | **CrossOver** | x86_64 Wine host | commercial — codeweavers.com |
| 3 | **CrossOver bottle `VR`** | the Windows environment | create a Win10 x64 bottle named `VR` |
| 4 | **Steam + games** | inside the bottle | install Steam in the bottle, then your VR titles |
| 5 | **OpenComposite `openvr_api.dll`** | dropped into each OpenVR game | **built by CI** (see below); deploy with `scripts/provision-all-steamvr.sh` |
| 6 | **wineopenxr bridge** | `C:\openxr\wineopenxr64.json` + DLLs, set as `HKLM\Software\Khronos\OpenXR\1\ActiveRuntime` | `bridge/` submodule |
| 7 | **DXMT** | `CX_GRAPHICS_BACKEND=dxmt` in the bottle | `dxmt/` submodule |
| 8 | **OXRSys runtime** | `liboxrsys-runtime.dylib` + `oxrsys-encoder-helper` (arm64) | `oxrsys-src/` submodule |
| 9 | **OXRSys config** | `~/Library/Application Support/OXRSys/oxrsys-runtime.toml` | `transport = "usb_adb"`, `encoder_helper = true` |
| 10 | **adb** | USB tunnel to the headset | `brew install android-platform-tools` |
| 11 | **BlackHole** *(for headset audio)* | loopback device; route the game's output to it | `brew install blackhole-2ch` — see [Headset audio](#headset-audio-optional) |

### Headset audio (optional)
To hear game sound in the headset: install **BlackHole** (`brew install blackhole-2ch`),
create a **Multi-Output Device** (BlackHole 2ch + your speakers) in Audio MIDI Setup
and select it as the system output, set `headset_audio = true` in the OXRSys config,
and install the audio-enabled Quest client APK. A Core Audio tap does **not** work
under CrossOver (silence — needs System-Audio-Recording permission it can't request);
reading a loopback **input** uses CrossOver's Microphone permission instead. USB only.
Full steps: see the README's *Headset audio* section.

### On the Quest 2
| # | Component | How you get it |
|---|-----------|----------------|
| 11 | **Developer mode + USB debugging** | enable in the Meta Quest phone app (one-time) |
| 12 | **OXRSys client app (APK)** | sideload onto the headset |

## Getting `openvr_api.dll` (CI-built)

You do **not** need to build OpenComposite yourself. GitHub Actions
cross-compiles it to a Windows x86_64 `openvr_api.dll` on every push:

- **Repo:** `anoshmisiosdev/OpenComposite`, branch **`merged-fixes`**
- **Workflow:** *Build openvr_api.dll (mingw-w64)* → download the
  `openvr_api-dll-win64` artifact, or grab it from a tagged **Release**.
- Then drop it into every installed OpenVR game in one shot:
  ```bash
  scripts/provision-all-steamvr.sh            # provision all games (idempotent)
  scripts/provision-all-steamvr.sh --restore  # put the stock DLLs back
  ```

> Built with **mingw-w64 (GCC), not MSVC** — on purpose. The MSVC↔GCC x64 ABI
> trampolines in this fork are GCC-specific and must be built with mingw g++ to
> be ABI-correct against the MSVC-compiled games the DLL is dropped into.

## Building new versions from source

Clone with submodules first: `git clone --recursive …` (or in an existing
checkout, `git submodule update --init --recursive`).

**Build prerequisites:** Xcode + command-line tools (Metal toolchain), and
`brew install cmake ninja meson mingw-w64`.

### OXRSys runtime (`liboxrsys-runtime.dylib`) — x86_64
The runtime is `dlopen`ed **in-process** by the x86_64/Rosetta Wine host, so it
**must be built x86_64** (an arm64 dylib can't load there).

```bash
cd oxrsys-src
cmake -S . -B build/x86 -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_BUILD_TYPE=Release
cmake --build build/x86 --target oxrsys_runtime -j
lipo -info build/x86/runtime/liboxrsys-runtime.dylib          # must say: x86_64

# deploy over the active runtime, then re-sign
cp build/x86/runtime/liboxrsys-runtime.dylib ~/liboxrsys-runtime-1.1.0/liboxrsys-runtime.dylib
codesign --force --sign - ~/liboxrsys-runtime-1.1.0/liboxrsys-runtime.dylib
```

### Hardware HEVC encoder helper — arm64 (out-of-process)
VideoToolbox's hardware HEVC encoder is unreachable from the x86_64/Rosetta
runtime, so encoding runs in a separate **native arm64** helper.

```bash
cd oxrsys-src
runtime/encoder_helper/build-helper.sh                        # builds + ad-hoc signs (arm64)
cp build/helper/oxrsys-encoder-helper ~/liboxrsys-runtime-1.1.0/oxrsys-encoder-helper
```
Enable it in `~/Library/Application Support/OXRSys/oxrsys-runtime.toml`:
`encoder_helper = true` and `encoder_helper_path = ".../oxrsys-encoder-helper"`.

### DXMT — from pristine upstream + our patches
The `dxmt/` submodule is pinned to a **pristine 3Shain/dxmt** commit; our changes
are applied as the `patches/` series by the build script (no fork to maintain).

```bash
./scripts/build-dxmt.sh      # applies patches/ in order, builds RELEASE (-O3)
./scripts/install-dxmt.sh    # overlays the built DLLs into CrossOver (backs up stock)
```
> **Release is not optional.** A debug/-O0 DXMT is ~4-6× slower and shows up as
> encode-path latency and microstutter in the headset even when the local render
> looks fine — `build-dxmt.sh` forces `--buildtype=release`.
> The build needs a native LLVM 15 + Wine headers, vendored under
> `dxmt/toolchains/`; point elsewhere with `LLVM15=… WINE=… ./scripts/build-dxmt.sh`.

> **After deploying any new runtime dylib or DXMT DLL, relaunch the game** — a
> running process won't pick up a swapped library.

### OpenComposite `openvr_api.dll`
Prefer the CI artifact (above). To build locally instead, use the mingw-w64
cross toolchain in `opencomposite/ci/` (see that repo's `.github/workflows`).

## Why each piece exists (the short version)
- **Rosetta 2 + CrossOver** — the games and the whole Wine host are x86_64, so
  they run under Rosetta. This is the load-bearing constraint: the OXRSys runtime
  dylib is loaded *in-process*, so it must be x86_64 too. Native arm64 is only
  possible for **out-of-process** helpers — which is exactly why the HW HEVC
  encoder is a separate **arm64 helper** (VideoToolbox hardware encode is
  unreachable from an x86_64/Rosetta process).
- **OpenComposite** translates the game's OpenVR calls to OpenXR.
- **wineopenxr** thunks those PE-side OpenXR calls to the native macOS runtime.
- **OXRSys** is that native runtime — it renders (via **DXMT**: D3D11→Metal),
  encodes, and streams to the headset over USB.
