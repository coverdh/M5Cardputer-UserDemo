#!/bin/sh
set -eu

CONFIGURATION="${1:-release}"
BUILD_APP_DIR=".build/${CONFIGURATION}/ADVCtl.app"
APP_DIR="${ADVCTL_APP_DIR:-${HOME}/Applications/ADVCtl.app}"
EXECUTABLE=".build/${CONFIGURATION}/ADVCtl"

export CLANG_MODULE_CACHE_PATH="${TMPDIR:-/tmp}/advctl-clang-module-cache"

swift build -c "${CONFIGURATION}"

if [ "${ADVCTL_CODESIGN_IDENTITY:-}" ]; then
    CODESIGN_IDENTITY="${ADVCTL_CODESIGN_IDENTITY}"
else
    CODESIGN_IDENTITY="$(/usr/bin/security find-identity -v -p codesigning 2>/dev/null \
        | /usr/bin/sed -n 's/.*"\(Apple Development:[^"]*\)".*/\1/p' \
        | /usr/bin/head -n 1)"
fi
if [ -z "${CODESIGN_IDENTITY}" ]; then
    CODESIGN_IDENTITY="-"
    echo "warning: using ad-hoc code signature; macOS privacy permissions may need re-approval after each build" >&2
fi

rm -rf "${BUILD_APP_DIR}"
mkdir -p "${BUILD_APP_DIR}/Contents/MacOS"
cp "${EXECUTABLE}" "${BUILD_APP_DIR}/Contents/MacOS/ADVCtl"
cp Sources/MacCtlHelper/Info.plist "${BUILD_APP_DIR}/Contents/Info.plist"
/usr/bin/codesign --force --deep --sign "${CODESIGN_IDENTITY}" "${BUILD_APP_DIR}" >/dev/null

mkdir -p "$(dirname "${APP_DIR}")"
rm -rf "${APP_DIR}"
cp -R "${BUILD_APP_DIR}" "${APP_DIR}"

echo "${APP_DIR}"

/usr/bin/osascript -e 'tell application id "dev.cardputer.advctl" to quit' >/dev/null 2>&1 || true
/usr/bin/osascript -e 'tell application "ADVCtl" to quit' >/dev/null 2>&1 || true
/bin/sleep 0.5
/usr/bin/open "${APP_DIR}"
