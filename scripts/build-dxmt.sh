#!/bin/bash
# Build the DXMT D3D11->Metal translation layer for the bridge.
#
# Model: the `dxmt/` submodule is pinned to a *pristine* upstream commit
# (3Shain/dxmt) that we've verified works with our patches. Our changes are NOT
# a fork branch — they live as patch files in patches/ and are applied here, in
# order, before building. The pin is frozen until a newer upstream commit is
# verified against these patches. After building, run scripts/install-dxmt.sh to
# overlay the built DLLs into CrossOver.
#
# Patches (applied in sorted order):
#   patches/0001-metal-interop.patch     IMTLD3D11InteropDevice (external MTLTexture
#                                        + fence sharing) — originally by @monofunc
#   patches/0002-relax-srgb-import.patch relax ImportMTLTexture2D sRGB/linear import
#
# Requires: meson, ninja, mingw-w64 (x86_64-w64-mingw32-*). On macOS:
#   brew install meson ninja mingw-w64
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DXMT="$ROOT/dxmt"

[ -d "$DXMT/src" ] || { echo "ERROR: dxmt submodule not checked out. Run: git submodule update --init --recursive"; exit 1; }

cd "$DXMT"
echo "DXMT base commit: $(git rev-parse --short HEAD) $(git log -1 --format=%s)"

# Apply each patch in patches/, in sorted order, idempotently. `git apply
# --reverse --check` succeeds when a patch is already present, so re-running the
# script (or building after `submodule update`) is safe.
shopt -s nullglob
for patch in "$ROOT"/patches/*.patch; do
  name="$(basename "$patch")"
  if git apply --reverse --check "$patch" >/dev/null 2>&1; then
    echo "  patch already applied: $name"
  elif git apply --check "$patch" >/dev/null 2>&1; then
    git apply "$patch"
    echo "  applied: $name"
  else
    echo "ERROR: $name does not apply cleanly to $(git rev-parse --short HEAD)." >&2
    echo "       The pinned DXMT commit likely moved. Re-pin to a verified commit" >&2
    echo "       or refresh the patch. Aborting." >&2
    exit 1
  fi
done
shopt -u nullglob

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
