#!/bin/sh
# Usage: ./build.sh [install]
# SIGN_ID selects the codesign identity. A stable identity keeps the Screen Recording permission between builds.
set -e
cd "$(dirname "$0")"
APP=build/Makishot.app

mkdir -p "$APP/Contents/MacOS"
swiftc -O -swift-version 5 -target "$(uname -m)-apple-macos14" Sources/*.swift -o "$APP/Contents/MacOS/Makishot"
cp Info.plist "$APP/Contents/"
codesign --force --sign "${SIGN_ID:-Apple Development}" "$APP" 2>/dev/null || codesign --force --sign - "$APP"
"$APP/Contents/MacOS/Makishot" --selftest

if [ "$1" = install ]; then
    pkill -x Makishot || true
    rm -rf /Applications/Makishot.app
    cp -R "$APP" /Applications/
    open /Applications/Makishot.app
fi
