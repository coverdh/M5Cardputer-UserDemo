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
