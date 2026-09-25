#!/bin/bash
# Install the wineopenxr bridge into CrossOver + a bottle, wired to OXRSys.
# Usage: ./install.sh <BottleName>
set -euo pipefail

BOTTLE="${1:?usage: install.sh <BottleName>}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/bridge/build"
CX=/Applications/CrossOver.app/Contents/SharedSupport/CrossOver
CX_WINE="$CX/lib/wine"
WINE="$CX/bin/wine"
WINEPREFIX="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE"
OXR_MANIFEST="$HOME/liboxrsys-runtime-1.1.0/oxrsys-runtime.json"

DLL="$BUILD/src/pe/wineopenxr.dll"
SO="$BUILD/src/unix/wineopenxr.so"

# --- preflight -------------------------------------------------------------
[ -f "$DLL" ] && [ -f "$SO" ] || { echo "ERROR: build first (cmake --build bridge/build)"; exit 1; }
[ -d "$CX_WINE/x86_64-unix" ]  || { echo "ERROR: CrossOver not found"; exit 1; }
[ -d "$WINEPREFIX" ]           || { echo "ERROR: bottle '$BOTTLE' not found. Create it in CrossOver first (Win10, 64-bit)."; exit 1; }
[ -f "$OXR_MANIFEST" ]         || { echo "ERROR: OXRSys runtime manifest not found at $OXR_MANIFEST"; exit 1; }
lipo -archs "$(python3 -c "import json;print(json.load(open('$OXR_MANIFEST'))['runtime']['library_path'])")" | grep -q x86_64 \
  || { echo "ERROR: OXRSys dylib has no x86_64 slice (required under Rosetta)"; exit 1; }
# macOS library-validation policy blocks the loader's dlopen of a non-adhoc-signed
# dylib inside the Wine process; ad-hoc re-sign locally (verified fix, 2026-09-07)
codesign --force --sign - "$(python3 -c "import json;print(json.load(open('$OXR_MANIFEST'))['runtime']['library_path'])")"

# Graphics backend check. The bridge supports both of CrossOver's D3D-to-Metal
# backends and picks the path per game device at runtime:
#   - DXMT (stock, unpatched): D3D11 via DXMT's ordinary shared-resource path
#     (OpenSharedResource + a keyed-mutex sync carrier).
#   - D3DMetal: D3D11 and D3D12 via Metal texture substitution (D3DMetal stubs
#     out resource sharing, so the bridge supplies the runtime's textures itself).
# Anything else (e.g. wined3d or vkd3d) cannot hand textures to the runtime.
BACKEND="$(sed -n 's/^"CX_GRAPHICS_BACKEND" *= *"\([^"]*\)".*/\1/p' "$WINEPREFIX/cxbottle.conf" 2>/dev/null | head -1)"
case "$BACKEND" in
  d3dmetal)
    echo "Graphics backend: D3DMetal (D3D11 + D3D12 via texture substitution)"
    ;;
  dxmt)
    echo "Graphics backend: DXMT (D3D11 via shared resources; D3D12 VR needs D3DMetal)"
    ;;
  *)
    if grep -rqs "winemetal" "$CX_WINE/../dxmt/x86_64-windows/d3d11.dll" 2>/dev/null || \
       grep -rqs "winemetal" "$WINEPREFIX/drive_c/windows/system32/d3d11.dll" 2>/dev/null; then
      echo "Graphics backend: DXMT d3d11 detected (D3D11 only; D3D12 VR needs D3DMetal)"
    else
      echo "WARNING: bottle graphics backend is '${BACKEND:-default}'."
      echo "         Select D3DMetal (recommended; D3D11 + D3D12) or DXMT (D3D11) in the"
      echo "         bottle's CrossOver settings, or VR session creation will fail."
    fi
    ;;
esac

# --- unix side (modifies CrossOver.app payload — breaks its code signature) -
cp "$SO" "$CX_WINE/x86_64-unix/wineopenxr.so"
codesign --force --sign - "$CX_WINE/x86_64-unix/wineopenxr.so"

# --- PE side + registration in the bottle ----------------------------------
cp "$DLL" "$CX_WINE/x86_64-windows/wineopenxr.dll"
cp "$DLL" "$WINEPREFIX/drive_c/windows/system32/wineopenxr.dll"
mkdir -p "$WINEPREFIX/drive_c/openxr"
cp "$ROOT/bridge/manifests/wineopenxr64.json" "$WINEPREFIX/drive_c/openxr/"

WINEPREFIX="$WINEPREFIX" CX_BOTTLE="$BOTTLE" "$WINE" reg add \
  'HKLM\Software\Khronos\OpenXR\1' /v ActiveRuntime /t REG_SZ \
  /d 'C:\openxr\wineopenxr64.json' /f

# --- point the unix-side Khronos loader at OXRSys --------------------------
# (OXRSys Home's LaunchAgent may already set this globally; set per-session too.)
launchctl setenv XR_RUNTIME_JSON "$OXR_MANIFEST" || true

echo
echo "Installed. Run games with:  XR_RUNTIME_JSON=$OXR_MANIFEST"
echo "Debug logs:                 WINEDEBUG=+openxr"
echo "NOTE: re-run this script after every CrossOver update."
