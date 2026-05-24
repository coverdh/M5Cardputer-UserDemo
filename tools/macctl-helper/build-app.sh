#!/bin/sh
set -eu

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "${ROOT_DIR}"

CONFIGURATION="${1:-release}"
BUILD_APP_DIR=".build/${CONFIGURATION}/ADVCtl.app"
BUILD_APP_PATH="${ROOT_DIR}/${BUILD_APP_DIR}"
APP_DIR="${ADVCTL_APP_DIR:-/Applications/ADVCtl.app}"
EXECUTABLE=".build/${CONFIGURATION}/ADVCtl"
DRIVER_PATH="$(./AudioDriver/build-driver.sh "${CONFIGURATION}")"
INSTALL_AUDIO_DRIVER="${ADVCTL_INSTALL_AUDIO_DRIVER:-1}"
OPEN_APP="${ADVCTL_OPEN_APP:-1}"

export CLANG_MODULE_CACHE_PATH="${TMPDIR:-/tmp}/advctl-clang-module-cache"

swift build -c "${CONFIGURATION}"

if [ "${ADVCTL_CODESIGN_IDENTITY:-}" ]; then
    CODESIGN_IDENTITY="${ADVCTL_CODESIGN_IDENTITY}"
else
    CODESIGN_IDENTITY="$(/usr/bin/security find-identity -v -p codesigning 2>/dev/null \
        | /usr/bin/sed -n 's/.*"\(Apple Development: cover_dh@qq.com (PHXBQGMZS8)\)".*/\1/p' \
        | /usr/bin/head -n 1)"
fi
if [ -z "${CODESIGN_IDENTITY}" ]; then
    CODESIGN_IDENTITY="$(/usr/bin/security find-identity -v -p codesigning 2>/dev/null \
        | /usr/bin/sed -n 's/.*"\(Developer ID Application:[^"]*\)".*/\1/p' \
        | /usr/bin/head -n 1)"
fi
if [ -z "${CODESIGN_IDENTITY}" ]; then
    CODESIGN_IDENTITY="$(/usr/bin/security find-identity -v -p codesigning 2>/dev/null \
        | /usr/bin/sed -n 's/.*"\(Apple Development:[^"]*\)".*/\1/p' \
        | /usr/bin/head -n 1)"
fi
if [ -z "${CODESIGN_IDENTITY}" ]; then
    CODESIGN_IDENTITY="-"
    echo "warning: using ad-hoc code signature; macOS privacy permissions may need re-approval after each build" >&2
fi

rm -rf "${BUILD_APP_PATH}"
mkdir -p "${BUILD_APP_PATH}/Contents/MacOS" "${BUILD_APP_PATH}/Contents/Resources"
cp "${EXECUTABLE}" "${BUILD_APP_PATH}/Contents/MacOS/ADVCtl"
cp Sources/MacCtlHelper/Info.plist "${BUILD_APP_PATH}/Contents/Info.plist"
cp -R "${DRIVER_PATH}" "${BUILD_APP_PATH}/Contents/Resources/ADVCtlAudio.driver"
/usr/bin/codesign --force --sign "${CODESIGN_IDENTITY}" "${BUILD_APP_PATH}/Contents/Resources/ADVCtlAudio.driver" >/dev/null
/usr/bin/codesign --force --deep --sign "${CODESIGN_IDENTITY}" "${BUILD_APP_PATH}" >/dev/null

install_audio_driver() {
    INSTALL_DIR="/Library/Audio/Plug-Ins/HAL"
    INSTALL_PATH="${INSTALL_DIR}/ADVCtlAudio.driver"
    ADMIN_SCRIPT="$(mktemp /tmp/advctl-install-hal.XXXXXX)"
    cat >"${ADMIN_SCRIPT}" <<EOF
#!/bin/sh
set -eu
mkdir -p "${INSTALL_DIR}"
rm -rf "${INSTALL_PATH}"
rm -f /tmp/advctl_audio_pcm.ring
ditto "${BUILD_APP_PATH}/Contents/Resources/ADVCtlAudio.driver" "${INSTALL_PATH}"
chown -R root:wheel "${INSTALL_PATH}"
chmod -R go-w "${INSTALL_PATH}"
killall coreaudiod || true
EOF
    chmod +x "${ADMIN_SCRIPT}"
    if sudo -n true 2>/dev/null; then
        sudo "${ADMIN_SCRIPT}"
    else
        /usr/bin/osascript -e "do shell script \"${ADMIN_SCRIPT}\" with administrator privileges"
    fi
    rm -f "${ADMIN_SCRIPT}"
    echo "${INSTALL_PATH}"
}

mkdir -p "$(dirname "${APP_DIR}")"
if [ -w "$(dirname "${APP_DIR}")" ]; then
    rm -rf "${APP_DIR}"
    cp -R "${BUILD_APP_PATH}" "${APP_DIR}"
else
    /usr/bin/osascript -e "do shell script \"rm -rf '${APP_DIR}' && ditto '${BUILD_APP_PATH}' '${APP_DIR}'\" with administrator privileges"
fi

echo "${APP_DIR}"

if [ "${INSTALL_AUDIO_DRIVER}" != "0" ]; then
    install_audio_driver
fi

if [ "${OPEN_APP}" != "0" ]; then
    /usr/bin/osascript -e 'tell application id "dev.cardputer.advctl" to quit' >/dev/null 2>&1 || true
    /usr/bin/osascript -e 'tell application "ADVCtl" to quit' >/dev/null 2>&1 || true
    /bin/sleep 0.5
    /usr/bin/open "${APP_DIR}"
fi
