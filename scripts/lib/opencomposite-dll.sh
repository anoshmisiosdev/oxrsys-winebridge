# shellcheck shell=bash
# Pick the OpenComposite build to install. Sourced by install-opencomposite.sh and
# provision-all-steamvr.sh.
#
# The DLL that CMake builds is <build dir>/bin/vrclient_x64.dll. openvr_api.dll is the
# same file, stripped and renamed (docs/opencomposite.md), but no build target makes it,
# so a build/bin/openvr_api.dll left over from an earlier manual copy goes stale while
# vrclient_x64.dll keeps being rebuilt. This helper therefore never uses openvr_api.dll
# from a build dir. It works as follows:
#
#   1. $OC_DLL, if set: that exact file (any name) is used.
#   2. Otherwise, the newest (by mtime) <repo>/opencomposite/build*/bin/vrclient_x64.dll.
#      The canonical dir is opencomposite/build (Release, see docs/opencomposite.md), and
#      other build dirs such as build-dbg (Release + -g) are candidates too, so
#      whichever one you rebuilt last wins. Set $OC_BUILD_DIR to use only one dir.
#
# Every candidate is printed with its mtime and embedded OpenComposite revision, plus
# the chosen one. A warning is printed if the revision is not the opencomposite
# checkout's HEAD. The choice is copied to a temp file and stripped with
# x86_64-w64-mingw32-strip --strip-unneeded when available (the PE timestamp is pinned
# to the build's mtime, so an unchanged build gives an identical file). The result is in
# $OC_INSTALL_DLL.

oc_dll_revision() { # $1 = dll; prints e.g. "492d147-dirty (subject, 2026-09-24)"
	local r
	# grep -m1 can SIGPIPE strings; don't let pipefail turn a hit into a miss.
	r=$(strings -a "$1" 2>/dev/null | grep -m1 -E '^[0-9a-f]{7,40}(-dirty)? \(.*, [0-9]{4}-[0-9]{2}-[0-9]{2}\)$' || true)
	echo "${r:-(unknown revision)}"
}

oc_dll_mtime() { stat -f '%Sm' -t '%Y-%m-%d %H:%M:%S' "$1"; }

oc_select_dll() { # $1 = repo root
	local root="$1" oc="$1/opencomposite" chosen="" f newest_m=0 m
	local -a cands=()

	if [ -n "${OC_DLL:-}" ]; then
		[ -f "$OC_DLL" ] || { echo "ERROR: OC_DLL=$OC_DLL does not exist" >&2; return 1; }
		chosen="$OC_DLL"
		echo "OpenComposite: using \$OC_DLL"
	else
		if [ -n "${OC_BUILD_DIR:-}" ]; then
			cands=("$OC_BUILD_DIR/bin/vrclient_x64.dll")
		else
			for f in "$oc"/build*/bin/vrclient_x64.dll; do cands+=("$f"); done
		fi
		echo "OpenComposite build candidates:"
		for f in "${cands[@]}"; do
			[ -f "$f" ] || continue
			m=$(stat -f '%m' "$f")
			echo "  $(oc_dll_mtime "$f")  ${f#"$root"/}  $(oc_dll_revision "$f")"
			if [ "$m" -gt "$newest_m" ]; then
				newest_m=$m
				chosen="$f"
			fi
		done
		[ -n "$chosen" ] || {
			echo "ERROR: no OpenComposite build found (${OC_BUILD_DIR:-$oc/build*}/bin/vrclient_x64.dll)." >&2
			echo "       Build it first - see docs/opencomposite.md" >&2
			return 1
		}
	fi

	local rev head
	rev=$(oc_dll_revision "$chosen")
	echo "Chosen: $chosen"
	echo "        built $(oc_dll_mtime "$chosen"), revision $rev"
	if head=$(git -C "$oc" rev-parse --short=7 HEAD 2>/dev/null); then
		case "$rev" in
		"$head"*) ;;
		*) echo "WARNING: that build's revision is not the opencomposite checkout's HEAD ($head); rebuild if that is unexpected." >&2 ;;
		esac
	fi

	OC_INSTALL_DLL="$(mktemp -t openvr_api.XXXXXX)"
	# shellcheck disable=SC2064
	trap "rm -f '$OC_INSTALL_DLL'" EXIT
	cp "$chosen" "$OC_INSTALL_DLL"
	if command -v x86_64-w64-mingw32-strip >/dev/null 2>&1; then
		# Pin the PE header timestamp so the same build always strips to the same bytes.
		SOURCE_DATE_EPOCH=$(stat -f %m "$chosen") x86_64-w64-mingw32-strip --strip-unneeded "$OC_INSTALL_DLL"
		echo "        stripped copy: $(stat -f '%z' "$OC_INSTALL_DLL") bytes, sha1 $(shasum "$OC_INSTALL_DLL" | cut -c1-12)"
	else
		echo "        (x86_64-w64-mingw32-strip not found; installing unstripped, $(stat -f '%z' "$OC_INSTALL_DLL") bytes)"
	fi
}
