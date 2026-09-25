#!/bin/bash
# Build "OXRSys Launcher.app" (arm64) into launcher/build/.
#
#   scripts/build-launcher.sh              # ad-hoc signed
#   SIGN_IDENTITY="Apple Development: …" scripts/build-launcher.sh
#
# With an ad-hoc signature macOS ties the System Audio Recording grant to this exact
# build, so every rebuild asks again. A real signing identity keeps the grant.
# Nothing is installed into /Applications; copy the app there yourself if you like.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE"
SWIFT=(env -u TOOLCHAINS xcrun swift)
"${SWIFT[@]}" build -c release --arch arm64
BIN="$("${SWIFT[@]}" build -c release --arch arm64 --show-bin-path)"

APP="$HERE/build/OXRSys Launcher.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN/OXRSysLauncher" "$APP/Contents/MacOS/OXRSysLauncher"
VERSION="$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo dev)"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleIdentifier</key><string>io.oxrsys.launcher</string>
  <key>CFBundleName</key><string>OXRSys Launcher</string>
  <key>CFBundleDisplayName</key><string>OXRSys Launcher</string>
  <key>CFBundleExecutable</key><string>OXRSysLauncher</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>CFBundleVersion</key><string>${VERSION}</string>
  <key>LSMinimumSystemVersion</key><string>14.4</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSPrincipalClass</key><string>NSApplication</string>
  <key>NSAudioCaptureUsageDescription</key><string>OXRSys Launcher captures your game's audio to stream it to your VR headset.</string>
</dict></plist>
PLIST
codesign --force --sign "${SIGN_IDENTITY:--}" "$APP"
codesign --verify --verbose=1 "$APP"
echo "Built: $APP"
