#!/bin/bash
# Build the DXMT D3D11->Metal translation layer for the bridge.
#
# We track the *default* upstream fork (monofunc/dxmt, feature/openxr) as the
# `dxmt/` submodule and carry our single OpenXR fix as patches/0001-*.patch,
# applied here before building. This keeps us on the default fork with no
# personal fork to maintain. After building, run scripts/install-dxmt.sh to
# overlay the built DLLs into CrossOver.
#
# Requires: meson, ninja, mingw-w64 (x86_64-w64-mingw32-*). On macOS:
#   brew install meson ninja mingw-w64
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DXMT="$ROOT/dxmt"
PATCH="$(ls "$ROOT"/patches/0001-*.patch | head -1)"

[ -d "$DXMT/src" ] || { echo "ERROR: dxmt submodule not checked out. Run: git submodule update --init --recursive"; exit 1; }

# Apply our OpenXR interop patch if it isn't already in the working tree.
# (Relaxes ImportMTLTexture2D validation so the runtime's sRGB swapchain
# textures import zero-copy; see the patch header and patches/NOTE-oxrsys.md.)
cd "$DXMT"
if git apply --reverse --check "$PATCH" >/dev/null 2>&1; then
  echo "DXMT OpenXR patch: already applied."
else
  git apply "$PATCH"
  echo "DXMT OpenXR patch: applied $(basename "$PATCH")."
fi

# Configure (once) and build the win64 cross target. The airconv shader compiler
# needs a native LLVM 15, and winemetal needs Wine headers; both are vendored
# under dxmt/toolchains/ (built once, gitignored). Override the paths if yours
# live elsewhere: LLVM15=/path WINE=/path scripts/build-dxmt.sh
LLVM_PATH="${LLVM15:-toolchains/llvm-darwin}"
WINE_PATH="${WINE:-toolchains/wine}"
if [ ! -d build ]; then
  meson setup build --cross-file build-win64.txt \
    -Dnative_llvm_path="$LLVM_PATH" \
    -Dwine_install_path="$WINE_PATH"
fi
ninja -C build

echo ""
echo "Built DXMT DLLs under dxmt/build/src. Next: scripts/install-dxmt.sh"
