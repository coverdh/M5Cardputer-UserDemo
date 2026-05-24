#!/bin/sh
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IDF_EXPORT="${IDF_EXPORT:-}"
PORT="${ADVCTL_PORT:-}"
BAUD="${ADVCTL_BAUD:-921600}"
MONITOR=0
FETCH_DEPS=0
EXTRA_IDF_ARGS=""

usage() {
    cat <<EOF
Usage: $0 [--port /dev/cu.usbmodemXXXX] [--baud 921600] [--monitor] [--fetch-deps]

Environment:
  IDF_EXPORT   Path to ESP-IDF export.sh. Auto-detected when omitted.
  ADVCTL_PORT  Serial port override.
  ADVCTL_BAUD  Flash baud rate. Defaults to 921600.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --port|-p)
            PORT="${2:-}"
            shift 2
            ;;
        --baud|-b)
            BAUD="${2:-}"
            shift 2
            ;;
        --monitor|-m)
            MONITOR=1
            shift
            ;;
        --fetch-deps)
            FETCH_DEPS=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            EXTRA_IDF_ARGS="${EXTRA_IDF_ARGS} $1"
            shift
            ;;
    esac
done

find_idf_export() {
    for candidate in \
        "${ROOT_DIR}/../reference/esp-idf-v5.4.2/export.sh" \
        "${ROOT_DIR}/../ESP-Dev/reference/esp-idf-v5.4.2/export.sh" \
        "${HOME}/Projects/ESP-Dev/reference/esp-idf-v5.4.2/export.sh" \
        "${IDF_PATH:-}/export.sh"
    do
        if [ -n "${candidate}" ] && [ -f "${candidate}" ]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done
    return 1
}

find_serial_port() {
    for pattern in \
        "/dev/cu.usbmodem*" \
        "/dev/cu.usbserial*" \
        "/dev/cu.SLAB_USBtoUART*" \
        "/dev/cu.wchusbserial*"
    do
        # shellcheck disable=SC2086
        for candidate in ${pattern}; do
            if [ -e "${candidate}" ]; then
                printf '%s\n' "${candidate}"
                return 0
            fi
        done
    done
    return 1
}

if [ -z "${IDF_EXPORT}" ]; then
    if ! IDF_EXPORT="$(find_idf_export)"; then
        echo "error: ESP-IDF v5.4.2 export.sh not found. Set IDF_EXPORT=/path/to/export.sh" >&2
        exit 1
    fi
fi

if [ -z "${PORT}" ]; then
    if ! PORT="$(find_serial_port)"; then
        echo "error: Cardputer serial port not found. Connect the device or pass --port /dev/cu.*" >&2
        exit 1
    fi
fi

cd "${ROOT_DIR}"

if [ "${FETCH_DEPS}" -eq 1 ]; then
    python3 ./fetch_repos.py
fi

. "${IDF_EXPORT}" >/tmp/advctl-esp-idf-export.log

echo "Using ESP-IDF: ${IDF_EXPORT}"
echo "Using serial port: ${PORT}"

idf.py set-target esp32s3
# shellcheck disable=SC2086
idf.py build ${EXTRA_IDF_ARGS}
idf.py -p "${PORT}" -b "${BAUD}" flash

if [ "${MONITOR}" -eq 1 ]; then
    idf.py -p "${PORT}" monitor
fi
