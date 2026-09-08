#!/bin/bash
# Install (or restore) the OpenComposite openvr_api.dll shim into a game directory.
#
# OpenComposite translates a game's OpenVR calls to OpenXR, so inside a
# CrossOver bottle whose active OpenXR runtime is the wineopenxr bridge, a
# SteamVR game runs against OXRSys without SteamVR.
#
# Usage:
#   install-opencomposite.sh [--bottle <name>] <game-dir>
#   install-opencomposite.sh [--bottle <name>] --restore <game-dir>
#
# <game-dir> is the game's install directory, either:
#   - a POSIX path (e.g. "$HOME/Library/Application Support/CrossOver/Bottles/
#     MyBottle/drive_c/Program Files (x86)/Steam/steamapps/common/Game")
#   - a Windows-style path inside the bottle (e.g.
#     'C:\Program Files (x86)\Steam\steamapps\common\Game') - requires
#     --bottle, unless exactly one bottle exists.
#
# Install: every openvr_api.dll found in the game dir at the usual win64
# locations (next to the exe, <game>/bin/win64/, plus any others discovered in
# the tree) is backed up once to openvr_api.dll.stock and replaced with the
# OpenComposite build. 32-bit copies (bin/win32 etc.) are left alone.
# Restore: puts every .stock backup back.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DLL="$ROOT/opencomposite/build/bin/openvr_api.dll"
[ -f "$DLL" ] || DLL="$ROOT/opencomposite/build/bin/vrclient_x64.dll"
BOTTLES_DIR="$HOME/Library/Application Support/CrossOver/Bottles"

usage() {
	sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
	exit 1
}

BOTTLE=""
RESTORE=0
GAME_ARG=""
while [ $# -gt 0 ]; do
	case "$1" in
	--bottle)
		BOTTLE="${2:?--bottle needs a value}"
		shift 2
		;;
	--restore)
		RESTORE=1
		shift
		;;
	-h | --help) usage ;;
	*)
		[ -z "$GAME_ARG" ] || { echo "ERROR: unexpected argument: $1" >&2; usage; }
		GAME_ARG="$1"
		shift
		;;
	esac
done
[ -n "$GAME_ARG" ] || usage

# --- Resolve the game directory ------------------------------------------
case "$GAME_ARG" in
[A-Za-z]:\\* | [A-Za-z]:/*)
	# Windows-style path: map through the bottle's drive_c (dosdevices)
	if [ -z "$BOTTLE" ]; then
		# If exactly one bottle exists, use it
		count=0
		only=""
		for b in "$BOTTLES_DIR"/*/; do
			[ -d "$b" ] || continue
			count=$((count + 1))
			only="$(basename "$b")"
		done
		if [ "$count" -eq 1 ]; then
			BOTTLE="$only"
			echo "Using sole bottle: $BOTTLE"
		else
			echo "ERROR: Windows-style path needs --bottle <name> ($count bottles found in $BOTTLES_DIR)" >&2
			exit 1
		fi
	fi
	[ -d "$BOTTLES_DIR/$BOTTLE" ] || { echo "ERROR: bottle '$BOTTLE' not found in $BOTTLES_DIR" >&2; exit 1; }
	drive="$(printf '%s' "${GAME_ARG:0:1}" | tr '[:upper:]' '[:lower:]')"
	rest="${GAME_ARG:2}"          # strip "C:"
	rest="${rest//\\//}"          # backslashes -> slashes
	if [ "$drive" = "c" ]; then
		GAME_DIR="$BOTTLES_DIR/$BOTTLE/drive_c$rest"
	else
		GAME_DIR="$BOTTLES_DIR/$BOTTLE/dosdevices/${drive}:$rest"
	fi
	;;
*)
	GAME_DIR="$GAME_ARG"
	;;
esac

[ -d "$GAME_DIR" ] || { echo "ERROR: game directory not found: $GAME_DIR" >&2; exit 1; }
echo "Game directory: $GAME_DIR"

# --- Collect the 64-bit openvr_api.dll locations --------------------------
# Usual spots first, then anything else in the tree (excluding win32 dirs).
targets=()
seen=$'\n'
add_target() {
	local dir="$1"
	case "$seen" in *$'\n'"$dir"$'\n'*) return ;; esac
	seen="${seen}${dir}"$'\n'
	targets+=("$dir")
}
if [ -f "$GAME_DIR/openvr_api.dll" ] || [ -f "$GAME_DIR/openvr_api.dll.stock" ]; then
	add_target "$GAME_DIR"
fi
if [ -f "$GAME_DIR/bin/win64/openvr_api.dll" ] || [ -f "$GAME_DIR/bin/win64/openvr_api.dll.stock" ]; then
	add_target "$GAME_DIR/bin/win64"
fi
while IFS= read -r -d '' f; do
	dir="$(dirname "$f")"
	case "$dir" in
	*/win32 | */Win32 | */x86) continue ;; # 32-bit: not our build
	esac
	add_target "$dir"
done < <(find "$GAME_DIR" \( -name openvr_api.dll -o -name openvr_api.dll.stock \) -print0 2>/dev/null)

[ "${#targets[@]}" -gt 0 ] || { echo "ERROR: no openvr_api.dll (or .stock backup) found under $GAME_DIR - is this a SteamVR game directory?" >&2; exit 1; }

# --- Restore or install ----------------------------------------------------
if [ "$RESTORE" -eq 1 ]; then
	restored=0
	for dir in "${targets[@]}"; do
		if [ -f "$dir/openvr_api.dll.stock" ]; then
			mv -f "$dir/openvr_api.dll.stock" "$dir/openvr_api.dll"
			echo "restored: $dir/openvr_api.dll"
			restored=$((restored + 1))
		fi
	done
	[ "$restored" -gt 0 ] || { echo "ERROR: no .stock backups found - nothing to restore" >&2; exit 1; }
	echo "Done: restored $restored file(s)."
else
	[ -f "$DLL" ] || { echo "ERROR: built DLL not found ($DLL). Build it first - see docs/opencomposite.md" >&2; exit 1; }
	for dir in "${targets[@]}"; do
		if [ -f "$dir/openvr_api.dll" ] && [ ! -f "$dir/openvr_api.dll.stock" ]; then
			cp -p "$dir/openvr_api.dll" "$dir/openvr_api.dll.stock"
			echo "backed up: $dir/openvr_api.dll -> .stock"
		fi
		cp "$DLL" "$dir/openvr_api.dll"
		echo "installed: $dir/openvr_api.dll"
	done
	echo "Done: installed OpenComposite into ${#targets[@]} location(s)."
	echo "Restore with: $0 ${BOTTLE:+--bottle \"$BOTTLE\" }--restore \"$GAME_ARG\""
fi
