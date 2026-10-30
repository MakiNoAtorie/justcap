#!/bin/sh
# Usage: ./build.sh [install]
# SIGN_ID selects the codesign identity. A stable identity keeps the Screen Recording permission between builds.
# HARDENED=1 adds the hardened runtime and a secure timestamp (needed for notarization, which this script does not do).
set -e
cd "$(dirname "$0")"
APP=build/justcap.app

mkdir -p "$APP/Contents/MacOS"
# Universal binary: one build per architecture, then lipo.
for arch in arm64 x86_64; do
    swiftc -O -swift-version 5 -target "$arch-apple-macos14" Sources/*.swift -o "build/justcap-$arch"
done
lipo -create build/justcap-arm64 build/justcap-x86_64 -output "$APP/Contents/MacOS/justcap"
cp Info.plist "$APP/Contents/"
plutil -replace CFBundleVersion -string "$(git rev-list --count HEAD 2>/dev/null || echo 1)" "$APP/Contents/Info.plist"
codesign --force ${HARDENED:+--options runtime --timestamp} --sign "${SIGN_ID:-Apple Development}" "$APP" 2>/dev/null \
    || codesign --force --sign - "$APP"
"$APP/Contents/MacOS/justcap" --selftest

if [ "$1" = install ]; then
    pkill -x justcap || true
    rm -rf /Applications/justcap.app
    cp -R "$APP" /Applications/
    open /Applications/justcap.app
fi
