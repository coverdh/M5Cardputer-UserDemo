#!/bin/sh
set -eu

CONFIGURATION="${1:-release}"
APP_DIR=".build/${CONFIGURATION}/MacCtl Helper.app"
EXECUTABLE=".build/${CONFIGURATION}/macctl-helper"

swift build -c "${CONFIGURATION}"

rm -rf "${APP_DIR}"
mkdir -p "${APP_DIR}/Contents/MacOS"
cp "${EXECUTABLE}" "${APP_DIR}/Contents/MacOS/macctl-helper"
cp Sources/MacCtlHelper/Info.plist "${APP_DIR}/Contents/Info.plist"

echo "${APP_DIR}"
