#!/bin/bash
# Build OpenComposite as a native macOS libopenvr_api.dylib (x86_64).
#
# This lets native macOS OpenVR apps that render with OpenGL (Vivecraft / Minecraft
# via LWJGL) run against OXRSys without SteamVR: OpenComposite translates OpenVR to
# OpenXR, binds the session with Metal (XR_KHR_metal_enable) and copies GL frames
# through IOSurface into the Metal swapchains.
#
# x86_64 only: the OXRSys runtime dylib is x86_64 and is loaded in-process, so the
# whole app (e.g. the JVM, run under Rosetta) must be x86_64 too.
#
# Usage: scripts/build-opencomposite-macos.sh
# Output: opencomposite/build-macos/bin/libopenvr_api.dylib
#
# Vivecraft (Prism Launcher): give the instance an x86_64 JDK 17 and add
#   -Dorg.lwjgl.openvr.libname=<repo>/opencomposite/build-macos/bin/libopenvr_api.dylib
# to its JVM arguments. See docs/vivecraft-macos.md.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/opencomposite"
BUILD="$SRC/build-macos"

cmake -S "$SRC" -B "$BUILD" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_OSX_ARCHITECTURES=x86_64 \
	-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
ninja -C "$BUILD"

echo
echo "Built: $BUILD/bin/libopenvr_api.dylib"
file "$BUILD/bin/libopenvr_api.dylib"
