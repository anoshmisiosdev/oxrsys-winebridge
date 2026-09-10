#!/bin/bash
# Undo install-dxmt-gamesir.sh: restore the pre-gamesir (3Shain-pin) DXMT build
# and remove the gamesir d3d12 DLLs + overrides from the bottle.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BOTTLE="${BOTTLE:-$HOME/Library/Application Support/CrossOver/Bottles/VR}"
CX=/Applications/CrossOver.app/Contents/SharedSupport/CrossOver
DXMT_WIN="$CX/lib/dxmt/x86_64-windows"
DXMT_UNIX="$CX/lib/dxmt/x86_64-unix"
SYS32="$BOTTLE/drive_c/windows/system32"

echo "== restoring shared dxmt set =="
for f in d3d11.dll d3d10core.dll dxgi.dll winemetal.dll; do
  if [ -f "$DXMT_WIN/$f.pre-gamesir" ]; then
    cp "$DXMT_WIN/$f.pre-gamesir" "$DXMT_WIN/$f"; echo "  restored $f"
  fi
done
if [ -f "$DXMT_UNIX/winemetal.so.pre-gamesir" ]; then
  cp "$DXMT_UNIX/winemetal.so.pre-gamesir" "$DXMT_UNIX/winemetal.so"
  codesign --force --sign - "$DXMT_UNIX/winemetal.so"
  echo "  restored winemetal.so"
fi

echo "== removing gamesir d3d12 from bottle =="
for f in d3d12.dll d3d12core.dll; do
  if [ -f "$SYS32/$f.pre-gamesir" ]; then
    cp "$SYS32/$f.pre-gamesir" "$SYS32/$f"; echo "  restored $f"
  else
    rm -f "$SYS32/$f"; echo "  removed $f (no prior version)"
  fi
done

echo "== removing DLL overrides =="
BOTTLE_NAME="$(basename "$BOTTLE")"
"$CX/bin/wine" --bottle "$BOTTLE_NAME" reg delete 'HKCU\Software\Wine\DllOverrides' /v d3d12     /f 2>/dev/null || true
"$CX/bin/wine" --bottle "$BOTTLE_NAME" reg delete 'HKCU\Software\Wine\DllOverrides' /v d3d12core /f 2>/dev/null || true

echo "Done. Back to the 3Shain-pin working build."
