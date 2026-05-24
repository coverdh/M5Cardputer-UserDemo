#!/bin/sh
set -eu

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONFIGURATION="${1:-release}"

cd "${ROOT_DIR}"
ADVCTL_INSTALL_AUDIO_DRIVER="${ADVCTL_INSTALL_AUDIO_DRIVER:-1}" \
ADVCTL_OPEN_APP="${ADVCTL_OPEN_APP:-1}" \
./build-app.sh "${CONFIGURATION}"
