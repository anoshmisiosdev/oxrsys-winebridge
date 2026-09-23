#!/bin/bash
# PROTOTYPE harness: run a Windows exe in a THROWAWAY CrossOver bottle with the
# D3DMetal-substitution bridge build, without touching CrossOver.app or any
# other bottle.
#
# The installed wineopenxr.dll in CrossOver's lib/wine always wins by name (its
# dll dir precedes WINEDLLPATH), so the prototype is staged as wineopxdms.{dll,so}
# with its PE export name patched to match, under $STAGE/wine, and a builtin-
# flagged copy is placed at $STAGE/wineopxdms.dll for LoadLibrary / the OpenXR
# manifest to point at.
#
# Setup once:
#   cxbottle --bottle ProtoD3DMetal --create --template win10_64
#   add "CX_GRAPHICS_BACKEND" = "d3dmetal" under [EnvironmentVariables] in its cxbottle.conf
# Usage: BUILD=<bridge build dir> run-in-bottle.sh <exe> [args...]
#   EXTRA_CX_ENV="CX_GRAPHICS_BACKEND=dxmt" to run the same bottle on DXMT
set -e
CX=/Applications/CrossOver.app/Contents/SharedSupport/CrossOver
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD=${BUILD:-$HERE/../../bridge/build}
STAGE=${STAGE:-/tmp/d3dproto}
BOTTLE=${BOTTLE:-ProtoD3DMetal}
mkdir -p "$STAGE/wine/x86_64-windows" "$STAGE/wine/x86_64-unix"
python3 "$HERE/rename_pe_export.py" "$BUILD/src/pe/wineopenxr.dll" "$STAGE/wine/x86_64-windows/wineopxdms.dll" wineopxdms.dll >/dev/null
cp "$BUILD/src/unix/wineopenxr.so" "$STAGE/wine/x86_64-unix/wineopxdms.so"
cp "$STAGE/wine/x86_64-windows/wineopxdms.dll" "$STAGE/wineopxdms.dll"
codesign --force --sign - "$STAGE/wine/x86_64-unix/wineopxdms.so" 2>/dev/null
export CX_ENV="WINEDLLPATH=$CX/lib/wine/x86_64-windows:$CX/lib/wine/i386-windows:$CX/lib/wine:$STAGE/wine ${EXTRA_CX_ENV:-}"
export XR_RUNTIME_JSON=${XR_RUNTIME_JSON:-$HOME/liboxrsys-runtime-1.1.0/oxrsys-runtime.json}
export DMS_TEST_DLL=${DMS_TEST_DLL:-wineopxdms.dll}
exec "$CX/bin/wine" --bottle "$BOTTLE" ${WINEDEBUG:+--debugmsg "$WINEDEBUG"} "$@"
