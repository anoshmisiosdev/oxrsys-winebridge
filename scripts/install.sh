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
# DXMT fork check: the bottle must use a DXMT with IMTLD3D11InteropDevice
if ! grep -rqs "IMTLD3D11InteropDevice" "$CX_WINE/x86_64-windows/"*.dll 2>/dev/null && \
   ! grep -rqs "IMTLD3D11InteropDevice" "$WINEPREFIX/drive_c/windows/system32/"*.dll 2>/dev/null; then
  echo "WARNING: no DXMT with IMTLD3D11InteropDevice detected."
  echo "         Install https://github.com/monofunc/dxmt and select DXMT as Graphics for this bottle,"
  echo "         or D3D11 session creation will fail (Metal interop unavailable)."
fi

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
