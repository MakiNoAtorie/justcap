#!/bin/sh
# Usage: ./build.sh [install]
# SIGN_ID selects the codesign identity. A stable identity keeps the Screen Recording permission between builds.
set -e
cd "$(dirname "$0")"
APP=build/justcap.app

mkdir -p "$APP/Contents/MacOS"
swiftc -O -swift-version 5 -target "$(uname -m)-apple-macos14" Sources/*.swift -o "$APP/Contents/MacOS/justcap"
cp Info.plist "$APP/Contents/"
codesign --force --sign "${SIGN_ID:-Apple Development}" "$APP" 2>/dev/null || codesign --force --sign - "$APP"
"$APP/Contents/MacOS/justcap" --selftest

if [ "$1" = install ]; then
    pkill -x justcap || true
    rm -rf /Applications/justcap.app
    cp -R "$APP" /Applications/
    open /Applications/justcap.app
fi
