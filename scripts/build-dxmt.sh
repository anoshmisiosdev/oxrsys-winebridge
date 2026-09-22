#!/bin/bash
# Build the DXMT D3D11->Metal translation layer for the bridge.
#
# DXMT is used STOCK: the `dxmt/` submodule is pinned to a pristine upstream
# commit (3Shain/dxmt) and nothing is patched. The bridge reaches Metal through
# DXMT's ordinary shared-resource path (ID3D11Device::OpenSharedResource plus
# ID3D11Fence sharing), so any DXMT with that support works — including the one
# CrossOver ships. See bridge/src/include/d3dkmt_interop.h for how the OpenXR
# runtime's Metal textures are handed over.
#
# After building, run scripts/install-dxmt.sh to overlay the built DLLs into
# CrossOver. If your CrossOver's bundled DXMT is new enough you do not need to
# build DXMT at all.
#
# Requires: meson, ninja, mingw-w64 (x86_64-w64-mingw32-*). On macOS:
#   brew install meson ninja mingw-w64
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DXMT="$ROOT/dxmt"

[ -d "$DXMT/src" ] || { echo "ERROR: dxmt submodule not checked out. Run: git submodule update --init --recursive"; exit 1; }

cd "$DXMT"
echo "DXMT base commit: $(git rev-parse --short HEAD) $(git log -1 --format=%s)"

# Refuse to build a dirty tree: local edits here would silently become a
# requirement for users, which is exactly what we moved away from.
if ! git diff --quiet || ! git diff --cached --quiet; then
  echo "ERROR: the dxmt submodule has local modifications." >&2
  echo "       DXMT must be built unpatched. Run 'git -C dxmt checkout -- .'" >&2
  echo "       (or commit the change upstream) and try again." >&2
  exit 1
fi

# Configure (once) and build the win64 cross target. The airconv shader compiler
# needs a native LLVM 15, and winemetal needs Wine headers; both are vendored
# under dxmt/toolchains/ (built once, gitignored). Override the paths if yours
# live elsewhere: LLVM15=/path WINE=/path scripts/build-dxmt.sh
LLVM_PATH="${LLVM15:-toolchains/llvm-darwin}"
WINE_PATH="${WINE:-toolchains/wine}"
if [ ! -d build ]; then
  # --buildtype=release (-O3) is essential: meson defaults to a debug/-O0 build,
  # and an unoptimized DXMT is ~4-6x slower at D3D11->Metal translation, which
  # shows up as encode-path latency (~32ms vs ~6ms) and microstutter in the
  # streamed frames even though the local render looks fine.
  meson setup build --cross-file build-win64.txt \
    --buildtype=release \
    -Dnative_llvm_path="$LLVM_PATH" \
    -Dwine_install_path="$WINE_PATH"
fi
ninja -C build

echo ""
echo "Built DXMT DLLs under dxmt/build/src. Next: scripts/install-dxmt.sh"
