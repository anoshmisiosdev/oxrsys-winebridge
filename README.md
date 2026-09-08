# oxrsys-winebridge

Run Windows OpenXR and SteamVR (OpenVR) apps on Apple Silicon Macs, displayed
on a Quest headset — CrossOver + a wineopenxr bridge + [OXRSys](https://github.com/demonixis/OXRSys).

    Windows VR game (x86-64, D3D11)
      → CrossOver 26 (Rosetta 2) → DXMT fork (D3D11→Metal, zero-copy interop)
      → wineopenxr.dll (PE builtin) → __wine_unix_call → wineopenxr.so (x86_64 Mach-O)
      → Khronos openxr_loader → liboxrsys-runtime.dylib (XR_KHR_metal_enable)
      → VideoToolbox H.265 → Wi-Fi/USB → Quest

    (SteamVR titles add: OpenComposite openvr_api.dll → the same chain)

No component of this existed as a working end-to-end path before this
project. SteamVR itself has had no macOS build since 2020; the pieces below
were each individually promising but unglued and untested together.

## Status at a glance

| Milestone | Result |
|---|---|
| Bridge builds on Apple Silicon | ✅ verified |
| PE → unixlib → native OpenXR runtime, headless | ✅ verified (`test/smoke.c`) |
| Full D3D11 render loop through DXMT → OXRSys | ✅ verified — 27,000 frames at 90 Hz |
| Visual confirmation (frames actually decode and display) | ✅ verified — user-observed in OXRSys Simulator |
| SteamVR (OpenVR) title support | ✅ built, ⚠️ compatibility varies by title (see below) |
| Two real games attempted | SUPERHOT VR: blocked (Unity legacy-binding bug, not ours). BasaultVR (UE4): one real bug found *and fixed*, second bug open |
| Upstream contributions | 1 PR open ([monofunc/dxmt#1](https://github.com/monofunc/dxmt/pull/1)), 1 more fix ready to submit, 1 compat note drafted for the OXRSys author |

## The chain, and why each link exists

**CrossOver 26** runs the Windows binary under Rosetta 2 — there is no other
way to execute x86-64 Windows code on Apple Silicon. Its **DXMT fork**
(`monofunc/dxmt`, `feature/openxr` branch) translates D3D11 calls to Metal
and — critically — exposes `IMTLD3D11InteropDevice`, an interop interface
absent from stock DXMT, letting native code get at the underlying
`MTLTexture` behind a D3D11 texture without a GPU copy.

**wineopenxr** (`monofunc/wineopenxr`, vendored as `bridge/`) is the actual
bridge: a PE DLL registered as the bottle's OpenXR runtime, paired with a
native `.so` half that talks to a real OpenXR runtime over Wine's
`__wine_unix_call` mechanism — the same technique Valve's Proton uses on
Linux, but reimplemented for macOS with a Metal graphics binding instead of
Vulkan, since it's the only graphics API that lets DXMT's texture handles
cross the boundary without a copy. Building it required a full toolchain
substitution: no `winegcc`/`winebuild` exist for macOS, so this uses
Homebrew mingw-w64 for the PE half, a `dlltool`-generated `ntdll` import
library, and a script that hand-writes the "Wine builtin DLL" signature into
the compiled DLL's header.

**OXRSys** is the native macOS OpenXR runtime on the receiving end — it
creates the session, owns the Vulkan/Metal swapchain, and streams encoded
frames to a Quest client over Wi-Fi or USB.

**OpenComposite**, cross-compiled here as a Windows PE `openvr_api.dll`, is
what lets *SteamVR* (OpenVR) titles use this chain at all: it's an
OpenVR→OpenXR shim, dropped in place of the real `openvr_api.dll` next to a
game's executable. `xrizer`, the more actively-developed alternative, was
ruled out early — it only builds as a Linux `.so`, not a Windows PE DLL, so
it has no path into a Wine bottle.

## What's proven to work

`test/smoke.c` negotiates the OpenXR loader interface, creates an instance,
and confirms it's talking to `OXRSys Runtime` — purely to prove the PE→unixlib
→native-runtime plumbing is alive with no graphics involved.

`test/d3d11test.cpp` is the real test: it creates a D3D11 device, a session
with a Metal graphics binding, two swapchains, and renders 27,000 frames of
a color-cycling stereo pair through the entire chain — CrossOver → DXMT →
wineopenxr → OXRSys → VideoToolbox H.265 encode → network. The user
confirmed watching the color cycle live in the OXRSys Simulator, i.e. frames
were genuinely encoded, transmitted, decoded, and displayed, not just
computed.

## Bugs found, and what they taught us

Every bug below was root-caused with disassembly, register traces, and a
minimal isolated repro before being called a bug — not guessed. That
discipline caught a false lead as often as a real one (see the second entry).

**1. `oovr_log_raw`'s static-init-order crash (fixed).** OpenComposite's
logger held a namespace-scope `std::ofstream` that could be reached by other
globals' constructors before its own constructor ran — invisible under MSVC,
fatal under GCC/MinGW, where translation-unit init order differs. Every use
of the library crashed on `LoadLibrary` before this was fixed. Converted to
construct-on-first-use.

**2. A DXVK/DXMT format-validation mismatch (fixed, PR open).**
`ImportMTLTexture2D` rejected every texture OXRSys handed it, because (a) it
compared pixel formats without accounting for Metal's sRGB/linear
view-compatibility rule, and (b) it required `PixelFormatView` usage on
textures we didn't create and don't need to reformat. Both are real,
narrowly-scoped relaxations, submitted upstream as
[monofunc/dxmt#1](https://github.com/monofunc/dxmt/pull/1) with a note
flagging that a broader existing helper (`Forget_sRGB`) might be the
maintainer's preferred fix instead.

**3. The GCC hidden-return-pointer chase — a genuine dead end, kept here
because the discipline that ruled it out matters more than the answer.**
SUPERHOT VR crashed identically on every launch, always in `UnityPlayer.dll`,
always right after OpenComposite's `GetEyeToHeadTransform` was the last
logged call. The disassembly of the generated forwarding thunk looked
exactly like a textbook GCC/MSVC ABI bug: the hidden struct-return pointer
and the `this` pointer for a virtual member function appeared to be getting
swapped. Two hours were spent building and testing isolated repros of
increasing fidelity — trivial version (worked), multiple-inheritance version
matching the real class hierarchy (worked), version split across separate
translation units to remove any inlining-driven correctness-by-accident
(*still worked*) — before a direct disassembly of the actual compiled
function proved the ground truth: this compiler's ms_abi output puts the
hidden return pointer in **RCX** and `this` in **RDX**, the reverse of the
assumption that had been driving the whole investigation. Once corrected,
every "buggy" instruction was legitimate. SUPERHOT's crash is real but lives
entirely inside Unity's own unsymbolized 2018-era legacy VR binding — not
reachable without Unity debug symbols that don't exist for a shipped title.

**4. Null hidden-area-mesh pointer crash in a UE4 title (fixed).**
BasaultVR crashed on every launch reading address `0x108` — `33 × 8`, the
exact byte offset of `HmdVector2_t[33]` on a null array. OXRSys doesn't
implement `XR_KHR_visibility_mask` (confirmed absent from its advertised
extension list), so OpenComposite correctly falls back to the OpenVR-spec
answer of `{nullptr, 0}` — a path essentially no real headset runtime ever
exercises, since every one of them supports visibility masks. Unreal
Engine's SteamVR plugin evidently doesn't null-check this field before
indexing it. Fixed by returning a valid, merely-empty allocation instead of
`nullptr` — after the fix, BasaultVR ran **90 seconds at 147% CPU doing real
rendering work** (versus an instant crash before) until a second, later,
unrelated null-dereference — encountered with no VR client connected during
that particular test run, an untried variable for next time.

## Repo layout

```
bridge/          wineopenxr, vendored as a git submodule (monofunc/wineopenxr)
dxmt/             DXMT fork, vendored as a git submodule (monofunc/dxmt, feature/openxr)
opencomposite/    NOT tracked — patched clone; see docs/opencomposite.md for the
                  exact clone + patch + build recipe (4 portability patches, all
                  MSVC→GCC/MinGW gap closures, documented and upstreamable)
docs/             DESIGN.md (architecture + risk register) and three research
                  reports (OXRSys internals, Valve's wineopenxr anatomy, the
                  macOS toolchain landscape) that the design was built from
patches/          Exported patches + PR text, ready to submit or already sent
scripts/          install.sh / install-dxmt.sh / install-opencomposite.sh and
                  their restore/uninstall counterparts — all idempotent, all
                  back up what they overwrite
test/             smoke.c (headless PE→unixlib proof) and d3d11test.cpp
                  (full render-loop proof)
```

## Build & install

Prerequisites: CrossOver 26, macOS 15+, Apple Silicon, Xcode with the Metal
toolchain, `brew install cmake ninja mingw-w64`, and OXRSys installed as a
universal (x86_64 + arm64) dylib — the x86_64 slice is what actually runs
under Rosetta; without it nothing here works.

```bash
git submodule update --init --recursive
cmake -B bridge/build bridge -G Ninja && cmake --build bridge/build
./scripts/install.sh <BottleName>        # registers the bridge as the bottle's OpenXR runtime
./scripts/install-dxmt.sh                # overlays the DXMT fork into CrossOver (backs up stock files)
```

For SteamVR titles, see `docs/opencomposite.md` for the OpenComposite build,
then:

```bash
./scripts/install-opencomposite.sh 'C:\path\to\Game'
```

## What's still open

- A second, later null-dereference in BasaultVR, not yet reproduced with a
  VR client actually connected.
- SUPERHOT VR's crash is understood but not fixable from this side — it's
  inside Unity's own closed, unsymbolized legacy VR binding.
- D3D12 is out of scope for now: no DXMT/D3D12 interop path exists, and
  vkd3d-proton (the natural alternative) needs Vulkan features MoltenVK
  doesn't yet expose.
- The one DXMT fix and the one OpenComposite fix from bug #4 above are ready
  to submit upstream but haven't been sent yet.

## Credits

Built on [demonixis/OXRSys](https://github.com/demonixis/OXRSys),
[monofunc/wineopenxr](https://github.com/monofunc/wineopenxr),
[monofunc/dxmt](https://github.com/monofunc/dxmt), and
[aashishvasu/OpenComposite](https://github.com/aashishvasu/OpenComposite),
all of which did the genuinely hard parts. This project's contribution is
gluing them into a working chain on Apple Silicon, and the bug reports/fixes
that came from actually trying to run something real through it.
