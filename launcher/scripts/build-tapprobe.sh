#!/bin/bash
# Build the phase-1 TapProbe.app (arm64, ad-hoc signed) into launcher/build/.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HERE/build/TapProbe.app"
rm -rf "$OUT"; mkdir -p "$OUT/Contents/MacOS"
clang -arch arm64 -mmacosx-version-min=14.4 -fobjc-arc -O2 -Wall \
  -I"$HERE/Sources/TapCore/include" \
  "$HERE/Sources/TapCore/OXTapCapture.m" "$HERE/Sources/TapProbe/main.m" \
  -framework Foundation -framework CoreAudio -o "$OUT/Contents/MacOS/TapProbe"
cat > "$OUT/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleIdentifier</key><string>io.oxrsys.tapprobe</string>
  <key>CFBundleName</key><string>TapProbe</string>
  <key>CFBundleExecutable</key><string>TapProbe</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>14.4</string>
  <key>LSUIElement</key><true/>
  <key>NSAudioCaptureUsageDescription</key><string>TapProbe captures game audio to test streaming it to your VR headset.</string>
</dict></plist>
PLIST
codesign --force --sign - "$OUT"
echo "$OUT"
