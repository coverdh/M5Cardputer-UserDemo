#!/bin/sh
set -eu

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
DRIVER_PATH="$("${ROOT_DIR}/build-driver.sh" "${1:-debug}")"
INSTALL_DIR="/Library/Audio/Plug-Ins/HAL"
INSTALL_PATH="${INSTALL_DIR}/ADVCtlAudio.driver"

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
    echo "warning: using ad-hoc code signature for ADVCtlAudio.driver" >&2
fi

codesign --force --sign "${CODESIGN_IDENTITY}" "${DRIVER_PATH}"

install_with_sudo() {
    sudo mkdir -p "${INSTALL_DIR}"
    sudo rm -rf "${INSTALL_PATH}"
    sudo rm -f /tmp/advctl_audio_pcm.ring
    sudo cp -R "${DRIVER_PATH}" "${INSTALL_PATH}"
    sudo chown -R root:wheel "${INSTALL_PATH}"
    sudo chmod -R go-w "${INSTALL_PATH}"
    sudo killall coreaudiod || true
}

install_with_admin_prompt() {
    ADMIN_SCRIPT="$(mktemp /tmp/advctl-install-hal.XXXXXX)"
    cat >"${ADMIN_SCRIPT}" <<EOF
#!/bin/sh
set -eu
mkdir -p "${INSTALL_DIR}"
rm -rf "${INSTALL_PATH}"
rm -f /tmp/advctl_audio_pcm.ring
cp -R "${DRIVER_PATH}" "${INSTALL_PATH}"
chown -R root:wheel "${INSTALL_PATH}"
chmod -R go-w "${INSTALL_PATH}"
killall coreaudiod || true
EOF
    chmod +x "${ADMIN_SCRIPT}"
    osascript -e "do shell script \"${ADMIN_SCRIPT}\" with administrator privileges"
    rm -f "${ADMIN_SCRIPT}"
}

if sudo -n true 2>/dev/null; then
    install_with_sudo
else
    install_with_admin_prompt
fi

echo "Installed ${INSTALL_PATH}"
echo "Check logs with: log show --last 5m --predicate 'eventMessage CONTAINS \"ADVCtlAudio\"'"
