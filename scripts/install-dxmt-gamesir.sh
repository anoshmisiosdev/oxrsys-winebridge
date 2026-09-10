#!/bin/bash
# Install the gamesir-labs/dxmt build (D3D12 support + our OpenXR interop port)
# into CrossOver so a native D3D12 game (e.g. Hitman 3) can be tried.
#
# CrossOver's dxmt graphics backend only manages d3d10core/d3d11/dxgi/winemetal
# (loaded from lib/dxmt); it has NO d3d12 path - D3D12 normally falls to Apple's
# D3DMetal. Because our winemetal thunk table gained MTLTexture_getInfo (index
# 151), the whole DXMT DLL set must move together, so we:
#   1. overlay lib/dxmt with the gamesir build (d3d11/d3d10core/dxgi/winemetal)
#   2. drop gamesir d3d12.dll + d3d12core.dll into the target bottle's system32
#   3. force those two to "native" via a DLL override
#
# Everything is backed up to *.pre-gamesir so revert-dxmt-gamesir.sh can undo it.
# The current working build is the 3Shain-pin build (build-3shain); this replaces
# it globally with the experimental gamesir build (which still carries our D3D11
# interop, so SteamVR D3D11 titles should keep working).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/dxmt/build-gamesir}"
BOTTLE="${BOTTLE:-$HOME/Library/Application Support/CrossOver/Bottles/VR}"
CX=/Applications/CrossOver.app/Contents/SharedSupport/CrossOver
DXMT_WIN="$CX/lib/dxmt/x86_64-windows"
DXMT_UNIX="$CX/lib/dxmt/x86_64-unix"
SYS32="$BOTTLE/drive_c/windows/system32"

[ -d "$BUILD/src" ] || { echo "ERROR: build dir $BUILD/src not found (build the gamesir spike first)"; exit 1; }
[ -d "$SYS32" ]     || { echo "ERROR: bottle system32 $SYS32 not found"; exit 1; }

echo "== overlaying shared dxmt set (lib/dxmt) with gamesir build =="
for f in d3d11.dll d3d10core.dll dxgi.dll winemetal.dll; do
  src=$(find "$BUILD/src" -name "$f" ! -name "*_dxmt.dll" | head -1)
  [ -n "$src" ] || { echo "ERROR: $f not found in build"; exit 1; }
  [ -f "$DXMT_WIN/$f.pre-gamesir" ] || cp "$DXMT_WIN/$f" "$DXMT_WIN/$f.pre-gamesir"
  cp "$src" "$DXMT_WIN/$f"
  echo "  installed $f"
done
so=$(find "$BUILD/src" -name "winemetal.so" | head -1)
[ -n "$so" ] || { echo "ERROR: winemetal.so not found"; exit 1; }
[ -f "$DXMT_UNIX/winemetal.so.pre-gamesir" ] || cp "$DXMT_UNIX/winemetal.so" "$DXMT_UNIX/winemetal.so.pre-gamesir"
cp "$so" "$DXMT_UNIX/winemetal.so"
codesign --force --sign - "$DXMT_UNIX/winemetal.so"
echo "  installed winemetal.so (ad-hoc signed)"

# gamesir's D3D12 path uses a separate Metal 4 backend, winemetal4 (PE + unixlib).
# CrossOver ships no winemetal4, so d3d12.dll can't resolve it and Wine falls back
# to builtin vkd3d. Install it into the same builtin dxmt dirs as winemetal.
m4dll=$(find "$BUILD/src" -name "winemetal4.dll" | head -1)
m4so=$(find "$BUILD/src" -name "winemetal4.so" | head -1)
if [ -n "$m4dll" ] && [ -n "$m4so" ]; then
  [ -f "$DXMT_WIN/winemetal4.dll.pre-gamesir" ] || { [ -f "$DXMT_WIN/winemetal4.dll" ] && cp "$DXMT_WIN/winemetal4.dll" "$DXMT_WIN/winemetal4.dll.pre-gamesir"; }
  cp "$m4dll" "$DXMT_WIN/winemetal4.dll"
  [ -f "$DXMT_UNIX/winemetal4.so.pre-gamesir" ] || { [ -f "$DXMT_UNIX/winemetal4.so" ] && cp "$DXMT_UNIX/winemetal4.so" "$DXMT_UNIX/winemetal4.so.pre-gamesir"; }
  cp "$m4so" "$DXMT_UNIX/winemetal4.so"
  codesign --force --sign - "$DXMT_UNIX/winemetal4.so"
  echo "  installed winemetal4.dll + winemetal4.so (ad-hoc signed)"
else
  echo "WARNING: winemetal4 not found in build - d3d12 will fall back to vkd3d"
fi

echo "== installing gamesir d3d12 into bottle system32 =="
for f in d3d12.dll d3d12core.dll; do
  src=$(find "$BUILD/src" -name "$f" | head -1)
  [ -n "$src" ] || { echo "ERROR: $f not found in build"; exit 1; }
  [ -f "$SYS32/$f.pre-gamesir" ] || { [ -f "$SYS32/$f" ] && cp "$SYS32/$f" "$SYS32/$f.pre-gamesir"; }
  cp "$src" "$SYS32/$f"
  echo "  installed $f"
done

echo "== setting DLL overrides (d3d12, d3d12core = native) =="
BOTTLE_NAME="$(basename "$BOTTLE")"
"$CX/bin/wine" --bottle "$BOTTLE_NAME" reg add 'HKCU\Software\Wine\DllOverrides' /v d3d12     /d native,builtin /f 2>/dev/null || echo "  (set d3d12 override manually if this failed)"
"$CX/bin/wine" --bottle "$BOTTLE_NAME" reg add 'HKCU\Software\Wine\DllOverrides' /v d3d12core /d native,builtin /f 2>/dev/null || echo "  (set d3d12core override manually if this failed)"

echo
echo "Done. Launch Hitman 3 from Steam in the VR bottle."
echo "Revert with: scripts/revert-dxmt-gamesir.sh"
