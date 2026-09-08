# Wine-on-macOS Toolchain Research: Building a wineopenxr-Style Bridge on Apple Silicon

Research date: 2026-09-07. Machine: MacBook Pro (Mac16,8, Apple M4 Pro, 24 GB), macOS 27.0 (26A5425a).

**TL;DR — Verdict: FEASIBLE, and there is direct prior art.** Every ingredient exists and was verified on this machine: CrossOver 26.2 with a working unixlib mechanism (Mach-O `.so` files in `lib/wine/x86_64-unix/`), winevulkan + bundled x86_64 MoltenVK, Homebrew mingw-w64 on arm64 producing valid PE32+ DLLs, and a universal (x86_64+arm64) OXRSys runtime dylib already installed. Critically, **monofunc/wineopenxr on GitHub is an existing out-of-tree macOS/CrossOver-26 port of wineopenxr** that solves exactly the PE-DLL + unixlib-.so build problem, with CMake + Homebrew mingw-w64 and no winegcc. The one hard architectural constraint — Rosetta processes cannot load arm64-only dylibs — is already satisfied by OXRSys shipping a universal binary.

---

## 1. Local machine inventory (verified by direct inspection)

### Hardware / OS
- MacBook Pro, Model Mac16,8, **Apple M4 Pro** (12 cores: 8P+4E), 24 GB RAM
- macOS **27.0** (build 26A5425a)
- Xcode Command Line Tools: **installed** (`xcode-select -p` → `/Library/Developer/CommandLineTools`)

### Applications
| Item | Status |
|---|---|
| `/Applications/CrossOver.app` | **Installed, version 26.2** |
| `/Applications/Wine Stable.app` | Installed — Homebrew `wine-stable` 11.0 (WineHQ build). Note: the Homebrew formula was **disabled 2026-09-01** ("does not pass the macOS Gatekeeper check") but the local install remains functional |
| `/Applications/OXRSys Home.app` | Installed, **universal binary (x86_64 + arm64)** |
| Whisky | Not installed |

### Command-line toolchain
| Tool | Status |
|---|---|
| `wine` | `/opt/homebrew/bin/wine` → **wine-11.0** (x86_64 Mach-O) |
| `wine64` | not found (merged into `wine` in modern builds) |
| `brew` | `/opt/homebrew/bin/brew` (arm64 Homebrew) |
| `cmake`, `ninja`, `git` | all present |
| `clang` | `/opt/homebrew/opt/llvm/bin/clang` (Homebrew LLVM) + Apple clang via CLT |
| **`x86_64-w64-mingw32-gcc`** | **`/opt/homebrew/bin/x86_64-w64-mingw32-gcc` — GCC 15.2.0, working** |
| winegcc / winebuild / widl | **NOT present anywhere** (neither CrossOver nor Wine Stable ships them) |

### Relevant Homebrew packages installed
`mingw-w64` (13.0.0_2; formula now at 14.0.0, **bottled for arm64** — Homebrew officially supports mingw-w64 on Apple Silicon), `molten-vk` (1.4.1, **arm64-only dylib**), `vulkan-headers`, `vulkan-loader`, `vulkan-tools`, `wine-stable`.

### CrossOver user data
`~/Library/Application Support/CrossOver/` exists (Bottles dir present but **empty — no bottles created yet**), with `compatdb-26.dat`, `cxfixes.xml`, etc.

### OXRSys (the native OpenXR runtime we want to bridge to)
- `~/liboxrsys-runtime-1.1.0/liboxrsys-runtime.dylib` — **Mach-O universal binary: x86_64 + arm64** (verified with `lipo -archs`). This is the single most important local fact: **the Rosetta constraint (section 4) is already satisfied.**
- Exports `_xrNegotiateLoaderRuntimeInterface` (standard OpenXR runtime entry point).
- Links directly against Metal / MetalPerformanceShaders / AppKit / VideoToolbox / CoreMedia — it does **not** link an external `libMoltenVK.dylib`. Strings show it resolves `vkCreateInstance` *from the app* ("OXRSys: Failed to resolve vkCreateInstance from app"), i.e. it consumes the application's Vulkan instance (XR_KHR_vulkan_enable-style graphics binding) rather than shipping its own Vulkan ICD. Under Wine, "the app's Vulkan" is winevulkan → CrossOver's bundled x86_64 MoltenVK — exactly the right topology.
- Manifest `oxrsys-runtime.json` → `library_path: /Users/riyananosh/liboxrsys-runtime-1.1.0/liboxrsys-runtime.dylib`; config in `~/Library/Application Support/OXRSys/oxrsys-runtime.toml` (streaming runtime for a Quest client: H.265 bitrate, FOV, resolution-scale settings); LaunchAgent `net.demonixis.oxrsys.runtime-env.plist` exists.

### Toolchain smoke tests (run on this machine, all passed)
```
x86_64-w64-mingw32-gcc -shared -o t.dll t.c
  → t.dll: PE32+ executable (DLL) (console) x86-64, for MS Windows
clang -arch x86_64 -dynamiclib …           → x86_64 Mach-O dylib   (CLT targets x86_64 fine)
clang -arch x86_64 -arch arm64 -dynamiclib → universal dylib        (universal builds work)
```

---

## 2. Wine unixlib on macOS: same mechanism, Mach-O files named `.so`

**Confirmed: the unixlib mechanism works identically to Linux on macOS, and CrossOver 26 uses it.** Verified directly against CrossOver 26.2's payload at `/Applications/CrossOver.app/Contents/SharedSupport/CrossOver`:

- `lib/wine/` contains exactly three dirs: `i386-windows`, `x86_64-windows`, `x86_64-unix` — the standard modern Wine split (PE builtins vs. unix libraries), in the new-WoW64 layout (32-bit Windows code, but only 64-bit unix libs).
- `lib/wine/x86_64-unix/` holds 34 unixlibs — `ntdll.so`, `win32u.so`, `winevulkan.so`, `opengl32.so`, `winemac.so`, `winecoreaudio.so`, `ws2_32.so`, `winegstreamer.so`, etc. **Every one is a Mach-O 64-bit x86_64 dylib that keeps the `.so` file extension** (`file winevulkan.so` → "Mach-O 64-bit dynamically linked shared library x86_64"). Wine's build system deliberately names unix libraries `.so` on every platform, macOS included; ntdll's unixlib loader `dlopen()`s them by that name from the `<arch>-unix` directory.
- `otool -L winevulkan.so` → links `@rpath/ntdll.so`, `@rpath/win32u.so`, `/usr/lib/libSystem.B.dylib`. Unixlibs resolve Wine-internal symbols against the other Mach-O `.so`s via rpath, exactly like the ELF case with sonames.
- `__wine_unix_call` / `__wine_unix_call_dispatcher` and `WINE_UNIX_CALL` work unchanged — the monofunc/wineopenxr project (§3/§6) explicitly communicates "via Wine's `__wine_unix_call_dispatcher`" on macOS/CrossOver 26, and Homebrew's WineHQ `wine-stable` 11.0 shows the identical `x86_64-unix/*.so` layout including its own `winevulkan.so` + bundled `lib/libMoltenVK.dylib`.

### winevulkan / MoltenVK in CrossOver
- **winevulkan is present and enabled by default** in CrossOver 26.2: `x86_64-windows/{vulkan-1.dll, winevulkan.dll}` + `x86_64-unix/winevulkan.so`. No setting needed to "turn Vulkan on" — vulkan-1 is a builtin; the Graphics backend setting (Auto/wined3d/D3DMetal/DXVK/DXMT) only chooses how *DirectX* is translated. DXVK/DXMT themselves consume winevulkan→MoltenVK.
- CrossOver ships its **own MoltenVK: `lib64/libMoltenVK.dylib` — x86_64-only slice** (verified with lipo). The unix-side loading reference lives in `win32u.so` (the `libMoltenVK.dylib` string is embedded there — modern Wine routes the Vulkan driver through win32u/winemacdrv).
- Wine-on-macOS Vulkan-via-MoltenVK dates to 2018 (https://news.ycombinator.com/item?id=25778677 ; https://en.wikipedia.org/wiki/MoltenVK). CrossOver 24 shipped Wine 9.0 + MoltenVK 1.2.5 (https://alternativeto.net/news/2024/2/crossover-24-released-with-wine-9-0-and-support-for-more-games-on-macos); CrossOver 25 shipped Wine 10.0 + MoltenVK 1.2.10 + D3DMetal 2.1 + new DXMT (https://www.codeweavers.com/blog/mjohnson/2025/3/11/experience-next-level-gaming-on-mac-with-crossover-25 ; https://www.codeweavers.com/crossover/changelog); CrossOver 26.x continues (MoltenVK 1.4.x era). Community caveat: "Custom DXVK and MoltenVK versions are not supported for CrossOver Mac" (https://depal1.github.io/mac-gaming/docs/update-MoltenVK-on-Crossover.html) — i.e. swapping CrossOver's MoltenVK is unsupported-but-done-by-modders.
- **Apple Game Porting Toolkit is a CrossOver fork:** Apple publishes patched CrossOver/Wine sources plus a proprietary x86_64 `D3DMetal.framework`; users build the Wine part from source via **x86_64 Homebrew under Rosetta** (`arch -x86_64 brew install game-porting-toolkit`) (https://www.applegamingwiki.com/wiki/Game_Porting_Toolkit ; https://github.com/Gcenx/game-porting-toolkit ; https://gist.github.com/Frityet/448a945690bd7c8cff5fef49daae858e). Takeaways: (a) building CrossOver-source Wine on Apple Silicon is proven, if slow; (b) CrossOver 26.2 itself bundles GPTK's D3DMetal at `lib64/apple_gptk/external/D3DMetal.framework` — **x86_64-only Mach-O** (verified) — Apple's own precedent for "ship native macOS code as x86_64 so Rosetta Wine can load it in-process."

---

## 3. Building the PE DLL + unixlib pair out-of-tree on macOS

### The toolchain that actually works (proven by monofunc/wineopenxr)
**You do not need winegcc, and you do not need to build Wine.** The existing macOS port https://github.com/monofunc/wineopenxr — which targets exactly our stack (macOS 15+, Apple Silicon, CrossOver 26, native runtimes exposing Metal interop) — demonstrates the complete out-of-tree recipe:

- **Prereqs:** `brew install cmake mingw-w64` (both already on this machine). Wine headers come from an `extern/wine` git submodule (headers only — no Wine build); OpenXR headers from `extern/OpenXR-SDK`.
- **PE side (`wineopenxr.dll`)** — cross-compiled with Homebrew mingw-w64 via a CMake toolchain file (`cmake/x86_64-w64-mingw32.cmake`):
  - `-mcrtdll=ucrt` on compile and link (avoid msvcrt `FILE*` mismatches against Wine's ucrtbase),
  - links a **hand-made ntdll import library**: `dlltool -d cmake/ntdll.def -l libntdll.a` — the `.def` exports the needed ntdll entry points **including `__wine_unix_call`** (this replaces winegcc/winebuild entirely),
  - `-Wl,--exclude-all-symbols`, `-Wl,--kill-at`,
  - a post-build `sign_builtin.py` **writes the 32-byte "Wine builtin DLL" signature into the PE header** — the replacement for winebuild's `.spec` machinery, needed so ntdll treats the DLL as a builtin and pairs it with a unixlib (and so `WINEDLLOVERRIDES="wineopenxr=b"` resolves it).
- **Unix side (`wineopenxr.so`)** — plain Apple clang, `CMAKE_OSX_ARCHITECTURES "x86_64"`, output `PREFIX ""` / `SUFFIX ".so"`, with `-Wl,-U,___wine_dbg_header -Wl,-U,___wine_dbg_output -Wl,-U,_NtQueryPerformanceCounter` so those symbols stay undefined at link time and resolve at runtime against CrossOver's already-loaded `ntdll.so` (macOS `-U`/flat-lookup trick standing in for ELF's default undefined-symbol behavior). It exports the `__wine_unix_call_funcs` table; the dispatcher finds it by convention.
- **Install:** copy the `.so` into `$CX_WINE/x86_64-unix/wineopenxr.so` inside the CrossOver payload (Wine's unixlib loader only searches the `<arch>-unix` dirs), and the `.dll` into the bottle (or `x86_64-windows/`), then register (§5). Modifying `CrossOver.app`'s payload breaks its code signature — expect ad-hoc re-signing / Gatekeeper friction, and re-install after each CrossOver update.

### Alternatives evaluated
- **winegcc/wineg++ on macOS:** exists only inside a Wine *source build*'s tools; no Homebrew/CrossOver binary ships it. Out-of-tree winegcc use on macOS is poorly supported (WineHQ forum: https://forum.winehq.org/viewtopic.php?t=36387 "building a native winedll on MacOS Monterey"; https://forum.winehq.org/viewtopic.php?p=150863 "How to debug unixlib?"). Not recommended.
- **In-tree (add a dll to a Wine source build):** works — Proton's wineopenxr does exactly this on Linux (https://github.com/ValveSoftware/Proton/blob/proton_9.0/wineopenxr/openxr.c, generated via https://github.com/GloriousEggroll/proton-ge-custom/blob/master/wineopenxr/make_openxr) — and building Wine (incl. CrossOver sources / GPTK) on Apple Silicon is proven via x86_64 Homebrew under Rosetta (needs clang + mingw-w64 for PE cross-builds). But it's a multi-hour build, produces *your* Wine rather than integrating with the user's CrossOver, and CrossOver's proprietary bits (D3DMetal, DXMT, cxfixes) don't come with it. **Only worth it if payload-injection proves too fragile.**
- **Precedents for PE-only mingw builds:** DXVK ships pure-PE mingw-built DLLs (no winelib), incl. the macOS fork https://github.com/Gcenx/DXVK-macOS/releases; vkd3d-proton likewise. Our project differs only in also needing the unix `.so` half — which monofunc's CMake shows how to do with stock clang.
- **MacPorts / Homebrew Wine:** Homebrew's `wine-stable` (11.0) is installed locally but the formula is currently **disabled** (Gatekeeper, 2026-09-01); it bundles winevulkan + MoltenVK but no dev tools. Fine as a test harness, not a toolchain.

---

## 4. The Rosetta / ABI landscape — the critical constraint

**How CrossOver runs x86-64 games on Apple Silicon:** the entire Wine process is x86_64. Verified: CrossOver 26.2's `wineloader` and `wineserver` are **Mach-O x86_64 executables**, every unixlib is x86_64, bundled `libMoltenVK.dylib` is x86_64-only, `D3DMetal.framework` is x86_64-only, and 32-bit Windows code runs through new-WoW64 (`i386-windows` PE dir, no `i386-unix`). Rosetta 2 translates everything — Wine's own code, the game's code, and every native dylib the process loads. (There is no arm64-native Wine path for x86-64 Windows apps in shipping CrossOver.)

**The hard rule:** a Rosetta-translated process **cannot load arm64 code — the system refuses to mix arm64 and x86_64 in one process**. Rosetta translation applies to the whole process including every dynamically loaded module; a universal dylib loaded by a translated process uses its **x86_64 slice** (Apple: https://developer.apple.com/documentation/apple-silicon/about-the-rosetta-translation-environment ; real-world manifestation: https://github.com/LWJGL/lwjgl3/issues/628 — x86_64 JVM under Rosetta cannot load arm64 natives).

**Consequences — all verified satisfiable on this machine:**
1. Our `wineopenxr.so` unixlib must be x86_64 (monofunc builds it `OSX_ARCHITECTURES "x86_64"`). ✅ trivial — Apple clang `-arch x86_64` smoke-tested.
2. **`liboxrsys-runtime.dylib` must have an x86_64 slice — it already does** (universal, verified). ✅ **No blocker.** OXRSys Home.app is universal too.
3. MoltenVK under Rosetta: CrossOver ships its own **x86_64 MoltenVK** and winevulkan uses it; Metal.framework is callable from translated processes (that's D3DMetal's and CrossOver's whole model). OXRSys doesn't even dlopen its own MoltenVK — it resolves `vkCreateInstance` from the app, so it rides winevulkan's x86_64 MoltenVK. ✅
4. Homebrew's arm64 `molten-vk` bottle (arm64-only) is **unusable** for the in-process chain; don't link it. If a standalone x86_64 MoltenVK were ever needed, MoltenVK builds x86_64/universal from source or via x86_64 Homebrew under Rosetta (as GPTK proves).
5. Everything transitively dlopen'd inside the bottle process must also have x86_64 slices. OXRSys's direct deps are all system frameworks (universal by definition). If in-process x86_64 ever became untenable for some component, the fallback is an out-of-process arm64-native helper over IPC/shared memory — OXRSys is already a streaming runtime (encode → Quest client), so its heavy lifting tolerates a process boundary; the verified universal dylib makes this unnecessary today.

**Residual watch item (soft):** performance — the whole VR frame loop (game, winevulkan, MoltenVK, OXRSys's Metal/VideoToolbox encode path) runs CPU-side under Rosetta translation. GPU/encode work lands in hardware regardless. Not a correctness blocker; benchmark, don't assume.

---

## 5. Registering the OpenXR runtime inside the Wine prefix

Standard Windows OpenXR discovery applies verbatim inside a prefix (https://registry.khronos.org/OpenXR/specs/1.0/loader.html): the loader (openxr_loader.dll shipped with each game) reads `HKLM\SOFTWARE\Khronos\OpenXR\1` → `ActiveRuntime` (REG_SZ) → path to a runtime manifest JSON → `library_path` → the runtime DLL.

**Prior art (Proton, mirrored by the macOS port):**
- Valve bakes it into wine.inf: https://github.com/ValveSoftware/wine/commit/ee64c2b291290753eefdca064b89ad86e2e6fcbe — `HKLM\Software\Khronos\OpenXR\1\ActiveRuntime = C:\openxr\wineopenxr64.json`.
- The manifest (https://git.epicm.org/Valve/Proton/src/commit/79a92742a154a1df2b11bb85159c9a7faccb55eb/wineopenxr/wineopenxr64.json) points `library_path` at `wineopenxr.dll`; Proton copies it to `drive_c/openxr/` (https://deepwiki.com/ValveSoftware/Proton/5-vrxr-compatibility).
- monofunc/wineopenxr does the same in a CrossOver bottle: copy `wineopenxr64.json` to `drive_c/openxr/`, then `reg add 'HKLM\Software\Khronos\OpenXR\1' /v ActiveRuntime /d 'C:\openxr\wineopenxr64.json'` (+ `WINEDLLOVERRIDES="wineopenxr=b"`).
- People routinely set custom ActiveRuntime values in Wine prefixes (e.g. Monado/VDXR interop discussions: https://github.com/ValveSoftware/Proton/issues/6038).

For our bridge: identical recipe — PE proxy runtime DLL registered via bottle-local JSON + HKLM key; the DLL's unixlib half then negotiates with `liboxrsys-runtime.dylib` on the unix side (via `xrNegotiateLoaderRuntimeInterface`, dlopen'd directly or through the native loader with `XR_RUNTIME_JSON`).

---

## 6. Prior-art scan

**Direct hit — https://github.com/monofunc/wineopenxr:** an out-of-tree wineopenxr **specifically for macOS 15+ / Apple Silicon / CrossOver 26**. PE `wineopenxr.dll` + unix `wineopenxr.so` (x86_64, runs under Rosetta), `__wine_unix_call_dispatcher` bridge, CMake + Homebrew mingw-w64, dlltool-generated ntdll import lib from a custom `ntdll.def`, builtin-signature post-processing script, installs the `.so` into `$CX_WINE/x86_64-unix/`, registers via `drive_c/openxr/wineopenxr64.json` + HKLM key, and bridges to native runtimes exposing **`XR_KHR_metal_enable`**, sharing swapchain textures as **MTLTexture handles zero-copy via DXMT's interop device**. Given that OXRSys ("net.demonixis.oxrsys") is installed on this machine alongside CrossOver 26.2, this repo is either the project in question or its closest sibling — either way it is the blueprint and proves end-to-end viability of the exact architecture proposed.

**Everything else found is negative results, confirming the niche is open:**
- SteamVR-in-CrossOver attempts fail (vrserver.exe crashes; no HMD detected): https://www.codeweavers.com/compatibility/crossover/forum/steamvr?msg=277444 ; https://www.codeweavers.com/compatibility/crossover/forum/steam?msg=235964. SteamVR-for-macOS was discontinued by Valve in 2020.
- ALVR: no macOS streamer; the Windows streamer under CrossOver goes nowhere (https://github.com/alvr-org/ALVR/discussions/2500 ; https://github.com/alvr-org/ALVR/discussions/1169).
- Failed Quest-PCVR-on-Mac attempt write-up (Virtual Desktop / SteamVR / ALVR routes): https://blog.nicholas.clooney.io/notes/quest-pcvr-on-mac-notes/ — concluded no supported pathway existed; its blocker was the missing native runtime, which OXRSys now supplies.
- No OpenComposite or xrizer macOS ports found.

**xrizer vs OpenComposite for the OpenVR leg — confirmed as suspected:**
- **xrizer is Linux-native only.** Rust, `crate-type = ["cdylib"]` (Cargo.toml: openxr 0.21 / openxr-sys 0.13 / ash 0.38), producing a Linux `openvr_api.so` drop-in used via `VR_OVERRIDE` / `openvrpaths.vrpath`; under Proton it replaces the **Linux** half that Proton's vrclient bridges to. No Windows/PE target, no macOS support (https://github.com/Supreeeme/xrizer ; https://raw.githubusercontent.com/Supreeeme/xrizer/main/Cargo.toml ; https://wiki.vronlinux.org/docs/fossvr/xrizer/). There is no macOS vrclient path for a Mach-O cdylib to plug into anyway. **Not usable in our chain as-is.**
- **OpenComposite builds a real Windows PE `openvr_api.dll`** (32- and 64-bit, AppVeyor CI; plus a Linux `.so`) (https://github.com/aashishvasu/OpenComposite ; https://github.com/QuestCraftPlusPlus/OpenComposite/blob/openxr/appveyor.yml). For OpenVR titles in a bottle: game → OpenComposite `openvr_api.dll` (PE, dropped next to the game) → Windows OpenXR loader → our PE proxy runtime → unixlib → OXRSys. **OpenComposite is the correct OpenVR shim; xrizer is not.** (Match the game's bitness — 32-bit games need the 32-bit DLL; under new-WoW64 the unix side stays 64-bit.)

---

## Blockers & risk register

| # | Severity | Item |
|---|---|---|
| B1 | **HARD CONSTRAINT — SATISFIED** | Rosetta processes cannot load arm64-only dylibs; every in-process native component needs an x86_64 slice. OXRSys dylib is already universal (verified), CrossOver's MoltenVK/D3DMetal are x86_64 (verified), system frameworks are universal. Keep enforcing universal/x86_64 builds for OXRSys permanently. |
| B2 | **RESOLVED** | mingw-w64 on Apple Silicon: Homebrew bottles it for arm64; GCC 15.2.0 installed here and produced a valid PE32+ DLL in a smoke test. |
| B3 | **RESOLVED** | No winegcc/winebuild on macOS: monofunc's dlltool-import-lib + builtin-signature-injection technique replaces them; Wine headers via submodule. |
| B4 | Medium | Installing the `.so` requires writing into `CrossOver.app`'s payload (`lib/wine/x86_64-unix/`) → breaks the app code signature; needs ad-hoc re-signing / Gatekeeper handling, and re-installation after every CrossOver update. No user-configurable unixlib search path exists. |
| B5 | Medium | ABI drift: the unixlib dispatcher ABI and builtin-DLL signature check are Wine-internal and version-coupled; a bridge built against CrossOver 26 headers may need rebuilds for CrossOver 27+. Pin per-CrossOver-version releases. |
| B6 | Low–Medium | Wine-internal symbols left undefined at link (`__wine_dbg_*`, `NtQueryPerformanceCounter`) rely on runtime flat-namespace resolution against ntdll.so — fragile across Wine refactors; keep that surface minimal. |
| B7 | Low | Homebrew `wine-stable` formula currently disabled (Gatekeeper) — irrelevant to the CrossOver path, but don't plan on Homebrew Wine for distribution. |
| B8 | Low | Whole-pipeline Rosetta translation is a CPU-side perf tax on the frame loop; GPU/encode is native hardware regardless. Benchmark. |

## Recommended build recipe (concrete)
1. Clone/fork monofunc/wineopenxr (`git submodule update --init`; Wine headers + OpenXR-SDK submodules).
2. `cmake -B build && cmake --build build` → `build/src/pe/wineopenxr.dll` (mingw-w64, ucrt, dlltool ntdll import lib, builtin-signed) + `build/src/unix/wineopenxr.so` (Apple clang, x86_64).
3. Install: `.so` → `/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/lib/wine/x86_64-unix/`; `.dll` + `wineopenxr64.json` → bottle `drive_c/openxr/`; `reg add HKLM\Software\Khronos\OpenXR\1 /v ActiveRuntime /d C:\openxr\wineopenxr64.json`; `WINEDLLOVERRIDES=wineopenxr=b`.
4. Unix side negotiates with `~/liboxrsys-runtime-1.1.0/liboxrsys-runtime.dylib` (x86_64 slice under Rosetta); graphics via winevulkan/MoltenVK Vulkan handles or `XR_KHR_metal_enable` MTLTexture interop (zero-copy via DXMT).
5. For OpenVR titles, add OpenComposite's PE `openvr_api.dll` in front.

## Sources
- https://github.com/monofunc/wineopenxr (+ raw README.md, CMakeLists.txt, src/pe/CMakeLists.txt, src/unix/CMakeLists.txt)
- https://github.com/ValveSoftware/Proton/blob/proton_9.0/wineopenxr/openxr.c
- https://github.com/ValveSoftware/wine/commit/ee64c2b291290753eefdca064b89ad86e2e6fcbe
- https://git.epicm.org/Valve/Proton/src/commit/79a92742a154a1df2b11bb85159c9a7faccb55eb/wineopenxr/wineopenxr64.json
- https://deepwiki.com/ValveSoftware/Proton/5-vrxr-compatibility
- https://github.com/GloriousEggroll/proton-ge-custom/blob/master/wineopenxr/make_openxr
- https://registry.khronos.org/OpenXR/specs/1.0/loader.html
- https://developer.apple.com/documentation/apple-silicon/about-the-rosetta-translation-environment
- https://github.com/LWJGL/lwjgl3/issues/628
- https://www.applegamingwiki.com/wiki/Game_Porting_Toolkit
- https://github.com/Gcenx/game-porting-toolkit
- https://gist.github.com/Frityet/448a945690bd7c8cff5fef49daae858e
- https://www.codeweavers.com/blog/mjohnson/2025/3/11/experience-next-level-gaming-on-mac-with-crossover-25
- https://www.codeweavers.com/crossover/changelog
- https://alternativeto.net/news/2024/2/crossover-24-released-with-wine-9-0-and-support-for-more-games-on-macos
- https://github.com/Gcenx/DXVK-macOS/releases
- https://depal1.github.io/mac-gaming/docs/update-MoltenVK-on-Crossover.html
- https://news.ycombinator.com/item?id=25778677
- https://github.com/Supreeeme/xrizer ; https://raw.githubusercontent.com/Supreeeme/xrizer/main/Cargo.toml ; https://wiki.vronlinux.org/docs/fossvr/xrizer/
- https://github.com/aashishvasu/OpenComposite ; https://github.com/QuestCraftPlusPlus/OpenComposite/blob/openxr/appveyor.yml
- https://blog.nicholas.clooney.io/notes/quest-pcvr-on-mac-notes/
- https://github.com/alvr-org/ALVR/discussions/2500 ; https://github.com/alvr-org/ALVR/discussions/1169
- https://www.codeweavers.com/compatibility/crossover/forum/steamvr?msg=277444
- https://forum.winehq.org/viewtopic.php?t=36387 ; https://forum.winehq.org/viewtopic.php?p=150863
- Local inspection: /Applications/CrossOver.app (26.2), /Applications/Wine Stable.app (wine-11.0), ~/liboxrsys-runtime-1.1.0/, /opt/homebrew (mingw-w64, molten-vk), toolchain smoke tests.
