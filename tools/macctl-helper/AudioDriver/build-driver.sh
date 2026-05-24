#!/bin/sh
set -eu

CONFIGURATION="${1:-debug}"
ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_TYPE="Debug"
if [ "${CONFIGURATION}" = "release" ] || [ "${CONFIGURATION}" = "Release" ]; then
    BUILD_TYPE="Release"
fi

BUILD_DIR="${ROOT_DIR}/.build/${BUILD_TYPE}"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" >/dev/null
cmake --build "${BUILD_DIR}" --target ADVCtlAudio --parallel >/dev/null

echo "${BUILD_DIR}/ADVCtlAudio.driver"
