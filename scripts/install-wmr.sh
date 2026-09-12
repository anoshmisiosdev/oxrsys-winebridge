#!/bin/bash
# Install the wired Windows Mixed Reality pieces of OXRSys next to the runtime
# the bridge uses, switch the runtime config to the wired headset, and
# (re)start the headset helper. Idempotent; re-run after rebuilding.
#
# Usage: ./scripts/install-wmr.sh [--arm64-build DIR] [--x86-build DIR] [--basalt PATH|none]
#
#   --arm64-build DIR  oxrsys-src build configured natively with
#                      -DOXRSYS_WMR_OPENCV=ON (helper + VIT monitor shim).
#                      Default: oxrsys-src/build
#   --x86-build DIR    oxrsys-src build with -DCMAKE_OSX_ARCHITECTURES=x86_64
#                      (the runtime dylib the Wine process loads).
#                      Default: oxrsys-src/build/x86
#   --basalt PATH      libbasalt.dylib from oxrsys-src/drivers/tools/build_basalt.sh
#                      for 6DoF head tracking; "none" skips it (IMU only).
#                      Default: ~/Library/Caches/OXRSys/basalt/build/libbasalt.dylib if present.
#
# See oxrsys-src/docs/platforms/wmr.md for the whole picture (EDID override,
# controllers, camera monitor, troubleshooting).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARM64_BUILD="$ROOT/oxrsys-src/build"
X86_BUILD="$ROOT/oxrsys-src/build/x86"
BASALT="$HOME/Library/Caches/OXRSys/basalt/build/libbasalt.dylib"
while [ $# -gt 0 ]; do
  case "$1" in
    --arm64-build) ARM64_BUILD="$2"; shift 2 ;;
    --x86-build)   X86_BUILD="$2"; shift 2 ;;
    --basalt)      BASALT="$2"; shift 2 ;;
    *) echo "usage: $0 [--arm64-build DIR] [--x86-build DIR] [--basalt PATH|none]"; exit 2 ;;
  esac
done

OXR_MANIFEST="$HOME/liboxrsys-runtime-1.1.0/oxrsys-runtime.json"
DST="$(dirname "$OXR_MANIFEST")"
CONFIG="$HOME/Library/Application Support/OXRSys/oxrsys-runtime.toml"
HELPER="$ARM64_BUILD/runtime/headset_helper/oxrsys-headset-helper"
SHIM="$ARM64_BUILD/runtime/headset_helper/liboxrsys-vit-monitor.dylib"
DYLIB="$X86_BUILD/runtime/liboxrsys-runtime.dylib"

# --- preflight -------------------------------------------------------------
[ -f "$OXR_MANIFEST" ] || { echo "ERROR: OXRSys runtime manifest not found at $OXR_MANIFEST (install OXRSys first)"; exit 1; }
[ -x "$HELPER" ] || { echo "ERROR: helper not built at $HELPER"; echo "       cmake -S oxrsys-src -B $ARM64_BUILD -G Ninja -DOXRSYS_WMR_OPENCV=ON && cmake --build $ARM64_BUILD --target oxrsys-headset-helper oxrsys_vit_monitor"; exit 1; }
[ -f "$DYLIB" ] || { echo "ERROR: x86_64 runtime not built at $DYLIB (see docs/FRESH-INSTALL.md)"; exit 1; }
lipo -archs "$DYLIB" | grep -q x86_64 || { echo "ERROR: $DYLIB has no x86_64 slice"; exit 1; }
file "$HELPER" | grep -q arm64 || { echo "ERROR: $HELPER is not arm64"; exit 1; }
if [ "$BASALT" != "none" ] && [ ! -f "$BASALT" ]; then
  echo "NOTE: no Basalt library at $BASALT; head tracking will be orientation only."
  echo "      Build it with oxrsys-src/drivers/tools/build_basalt.sh, then re-run."
  BASALT=none
fi

# --- stop the running helper (it holds the headset) --------------------------
pkill -f oxrsys-headset-helper 2>/dev/null || true
sleep 2

# --- files ------------------------------------------------------------------
[ -f "$DST/liboxrsys-runtime.dylib.pre-wmr.bak" ] || cp "$DST/liboxrsys-runtime.dylib" "$DST/liboxrsys-runtime.dylib.pre-wmr.bak" 2>/dev/null || true
cp "$DYLIB" "$DST/liboxrsys-runtime.dylib"
cp "$HELPER" "$DST/oxrsys-headset-helper"
[ -f "$SHIM" ] && cp "$SHIM" "$DST/liboxrsys-vit-monitor.dylib"
if [ "$BASALT" != "none" ]; then
  cp "$(python3 -c "import os,sys;print(os.path.realpath(sys.argv[1]))" "$BASALT")" "$DST/libbasalt.dylib"
else
  rm -f "$DST/libbasalt.dylib"
fi
# macOS library validation inside the Wine process wants ad-hoc signatures.
for f in liboxrsys-runtime.dylib oxrsys-headset-helper liboxrsys-vit-monitor.dylib libbasalt.dylib; do
  [ -f "$DST/$f" ] && codesign --force --sign - "$DST/$f" 2>/dev/null
done

# --- config: wired headset on ---------------------------------------------
mkdir -p "$(dirname "$CONFIG")"
[ -f "$CONFIG" ] || cp "$ROOT/oxrsys-src/runtime/oxrsys-runtime.toml" "$CONFIG"
[ -f "$CONFIG.pre-wired.bak" ] || cp "$CONFIG" "$CONFIG.pre-wired.bak"
if grep -q '^wired_headset *=' "$CONFIG"; then
  sed -i '' 's/^wired_headset *=.*/wired_headset = true/' "$CONFIG"
else
  printf '\n[wired]\nwired_headset = true\nwired_eye_height_m = 1.6\n' >> "$CONFIG"
fi

# --- EDID override reminder --------------------------------------------------
if ! ls /Library/Displays/Contents/Resources/Overrides/DisplayVendorID-*/DisplayProductID-* >/dev/null 2>&1; then
  echo "NOTE: no display override installed; macOS will hide the headset panel."
  echo "      sudo python3 oxrsys-src/drivers/tools/wmr_edid_override.py --install   (then replug the video cable)"
fi

# --- start the helper the way the runtime does --------------------------------
nohup "$DST/oxrsys-headset-helper" --socket "/tmp/oxrsys-headset-$(id -u).sock" \
  > /dev/null 2>&1 < /dev/null &
sleep 8
echo
echo "Installed into $DST:"
ls -1 "$DST" | grep -E '^(oxrsys-headset-helper|liboxrsys-vit-monitor.dylib|libbasalt.dylib|liboxrsys-runtime.dylib)$' | sed 's/^/  /'
echo "Config: $CONFIG (wired_headset = true; backup $CONFIG.pre-wired.bak)"
echo "Helper log: ~/Library/Application Support/OXRSys/oxrsys-headset-helper.log"
grep -E "open:|head |no usable|EDID|6DoF" "$HOME/Library/Application Support/OXRSys/oxrsys-headset-helper.log" 2>/dev/null | tail -3 | sed 's/^/  /'
echo "Camera monitor:  $DST/oxrsys-headset-helper --monitor   (stop the running helper first)"
echo "Games: launch as before; the runtime connects to the helper instead of streaming."
