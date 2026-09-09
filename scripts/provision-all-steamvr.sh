#!/bin/bash
# Provision EVERY installed SteamVR/OpenVR game in a CrossOver bottle to run on
# OXRSys, by swapping OpenComposite's openvr_api.dll into each game that has one.
#
# The runtime side (OpenXR ActiveRuntime -> wineopenxr -> OXRSys, DXMT, the
# hardware-HEVC helper) is already bottle-wide, so this DLL swap is the only
# per-game step. Native-OpenXR games (no openvr_api.dll) need nothing and are
# skipped automatically.
#
# Usage:
#   provision-all-steamvr.sh [--bottle <name>]            # provision all now (idempotent)
#   provision-all-steamvr.sh [--bottle <name>] --restore  # restore every game's stock DLL
#   provision-all-steamvr.sh [--bottle <name>] --watch    # provision now, then watch for new installs
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DLL="$ROOT/opencomposite/build/bin/openvr_api.dll"
[ -f "$DLL" ] || DLL="$ROOT/opencomposite/build/bin/vrclient_x64.dll"
BOTTLES="$HOME/Library/Application Support/CrossOver/Bottles"

BOTTLE="VR"; RESTORE=0; WATCH=0
while [ $# -gt 0 ]; do case "$1" in
  --bottle) BOTTLE="${2:?}"; shift 2;;
  --restore) RESTORE=1; shift;;
  --watch) WATCH=1; shift;;
  *) echo "unknown arg: $1" >&2; exit 1;;
esac; done

DRIVE_C="$BOTTLES/$BOTTLE/drive_c"
[ -d "$DRIVE_C" ] || { echo "ERROR: bottle '$BOTTLE' not found"; exit 1; }
[ -f "$DLL" ] || { echo "ERROR: OpenComposite build not found; build opencomposite first"; exit 1; }

# Discover every Steam library folder (games may live outside the default one).
find_libraries() {
  local steam="$DRIVE_C/Program Files (x86)/Steam"
  echo "$steam/steamapps/common"
  local vdf="$steam/steamapps/libraryfolders.vdf"
  [ -f "$vdf" ] || return 0
  # extract "path" values, translate the bottle's C:\... to the host path
  grep -iE '"path"' "$vdf" | sed -E 's/.*"path"[[:space:]]*"([^"]+)".*/\1/' | while read -r p; do
    # C:\Foo\Bar -> $DRIVE_C/Foo/Bar
    local rel="${p#?:\\\\}"; rel="${p#*:\\}"; rel="${rel//\\\\//}"; rel="${rel//\\//}"
    [ -d "$DRIVE_C/$rel/steamapps/common" ] && echo "$DRIVE_C/$rel/steamapps/common"
  done
}

provision_game() {  # $1 = game dir
  local dir="$1" name did=0
  name="$(basename "$dir")"
  # every real openvr_api.dll (skip our own .stock backups)
  while IFS= read -r -d '' f; do
    if [ "$RESTORE" = 1 ]; then
      [ -f "$f.stock" ] && { cp "$f.stock" "$f"; did=1; }
    else
      [ -f "$f.stock" ] || cp "$f" "$f.stock"      # back up stock once
      cmp -s "$DLL" "$f" || { cp "$DLL" "$f"; }      # swap in OpenComposite if different
      did=1
    fi
  done < <(find "$dir" -iname "openvr_api.dll" ! -iname "*.stock" -print0 2>/dev/null)
  [ "$did" = 1 ] && echo "  $([ "$RESTORE" = 1 ] && echo restored || echo provisioned): $name"
}

run_once() {
  local n=0 seen=":"
  while IFS= read -r libdir; do
    [ -d "$libdir" ] || continue
    for g in "$libdir"/*/; do
      [ -d "$g" ] || continue
      # dedupe by canonical path (a library may be reached via >1 spelling)
      local canon; canon="$(cd "$g" 2>/dev/null && pwd -P)" || continue
      case "$seen" in *":$canon:"*) continue;; esac
      seen="$seen$canon:"
      # only games that actually have an openvr_api.dll (= OpenVR/SteamVR titles)
      find "$g" -iname "openvr_api.dll" ! -iname "*.stock" -print -quit 2>/dev/null | grep -q . || continue
      provision_game "$g"; n=$((n+1))
    done
  done < <(find_libraries)
  echo "$([ "$RESTORE" = 1 ] && echo Restored || echo Provisioned) $n OpenVR game(s) in bottle '$BOTTLE'."
}

run_once

if [ "$WATCH" = 1 ] && [ "$RESTORE" = 0 ]; then
  echo "Watching for new SteamVR installs (Ctrl-C to stop)..."
  # cheap poll: re-provision whenever steamapps/common changes
  last=""
  while true; do
    cur="$(find_libraries | while read -r l; do ls -la "$l" 2>/dev/null; done | shasum | cut -d' ' -f1)"
    if [ "$cur" != "$last" ]; then last="$cur"; run_once >/dev/null 2>&1 || true; fi
    sleep 15
  done
fi
