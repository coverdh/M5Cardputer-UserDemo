#!/bin/sh
set -eu

CONFIGURATION="${1:-release}"
APP_DIR=".build/${CONFIGURATION}/ADVCtl.app"
EXECUTABLE=".build/${CONFIGURATION}/ADVCtl"

export CLANG_MODULE_CACHE_PATH="${TMPDIR:-/tmp}/advctl-clang-module-cache"

swift build -c "${CONFIGURATION}"

rm -rf "${APP_DIR}"
mkdir -p "${APP_DIR}/Contents/MacOS"
cp "${EXECUTABLE}" "${APP_DIR}/Contents/MacOS/ADVCtl"
cp Sources/MacCtlHelper/Info.plist "${APP_DIR}/Contents/Info.plist"

echo "${APP_DIR}"

/usr/bin/osascript -e 'tell application "ADVCtl" to quit' >/dev/null 2>&1 || true
/bin/sleep 0.5
/usr/bin/open "${APP_DIR}"
