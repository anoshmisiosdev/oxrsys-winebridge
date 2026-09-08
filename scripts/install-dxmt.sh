#!/bin/bash
# Install the monofunc DXMT fork (with IMTLD3D11InteropDevice) into CrossOver.
# CrossOver keeps DXMT in its own payload dir: lib/dxmt/{x86_64-windows,x86_64-unix}.
# We back up the stock files once, then overlay the fork's builds.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/dxmt/build"
CX=/Applications/CrossOver.app/Contents/SharedSupport/CrossOver
DXMT_WIN="$CX/lib/dxmt/x86_64-windows"
DXMT_UNIX="$CX/lib/dxmt/x86_64-unix"

for f in d3d11.dll d3d10core.dll dxgi.dll winemetal.dll; do
  src=$(find "$BUILD/src" -name "$f" | head -1)
  [ -n "$src" ] || { echo "ERROR: $f not found in build"; exit 1; }
  [ -f "$DXMT_WIN/$f.stock" ] || cp "$DXMT_WIN/$f" "$DXMT_WIN/$f.stock"
  cp "$src" "$DXMT_WIN/$f"
  echo "installed $f"
done
so=$(find "$BUILD/src" -name "winemetal.so" | head -1)
[ -n "$so" ] || { echo "ERROR: winemetal.so not found"; exit 1; }
[ -f "$DXMT_UNIX/winemetal.so.stock" ] || cp "$DXMT_UNIX/winemetal.so" "$DXMT_UNIX/winemetal.so.stock"
cp "$so" "$DXMT_UNIX/winemetal.so"
codesign --force --sign - "$DXMT_UNIX/winemetal.so"
echo "installed winemetal.so (ad-hoc signed)"
echo "Done. Bottle already has CX_GRAPHICS_BACKEND=dxmt."
