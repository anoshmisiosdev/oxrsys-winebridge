#!/bin/bash
set -euo pipefail
CX_WINE=/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/lib/wine
rm -f "$CX_WINE/x86_64-unix/wineopenxr.so" "$CX_WINE/x86_64-windows/wineopenxr.dll"
echo "Removed bridge from CrossOver payload. Bottle registry entries left in place;"
echo "delete HKLM\\Software\\Khronos\\OpenXR\\1\\ActiveRuntime per-bottle to fully revert."
