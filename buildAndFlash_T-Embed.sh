#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ESP32_DIR="${SCRIPT_DIR}"
PORT="${ESPPORT:-}"
RUN_MONITOR=0
BUILD_ONLY=0
SKIP_BRUCE=0
EXPORT_SCRIPT="${ESP_IDF_EXPORT_SCRIPT:-${HOME}/esp/esp-idf/export.sh}"

BOARD="lilygo_t_embed_cc1101"
BUILD_DIR="build_t_embed"
IDF_TARGET="esp32s3"
SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.lilygo_t_embed_cc1101"
BRUCE_DIR="${ESP32_DIR}/multi-boot/bruce"
BRUCE_BIN="${BRUCE_DIR}/.pio/build/lilygo-t-embed-cc1101/firmware.bin"

detect_usbmodem_port() {
    local matches=()
    shopt -s nullglob
    matches=(/dev/cu.usbmodem* /dev/ttyACM*)
    shopt -u nullglob

    if [[ "${#matches[@]}" -eq 1 ]]; then
        printf '%s\n' "${matches[0]}"
        return 0
    fi

    if [[ "${#matches[@]}" -eq 0 ]]; then
        if [[ "${BUILD_ONLY}" -eq 0 ]]; then
            echo "No serial device found (searched /dev/cu.usbmodem* and /dev/ttyACM*). Use --port or set ESPPORT." >&2
            return 1
        else
            return 0
        fi
    else
        echo "Multiple serial devices found: ${matches[*]}" >&2
        echo "Use --port or set ESPPORT." >&2
        return 1
    fi
}

usage() {
    cat <<EOF
Usage: $(basename "$0") [--port <device>] [--monitor] [--build-only] [--skip-bruce]

Builds Flipper and Bruce for the LilyGo T-Embed CC1101 dual-boot layout.

Options:
  --port <device>  Serial device to flash. Default: auto-detect /dev/cu.usbmodem* (macOS) or /dev/ttyACM* (Linux)
  --monitor        Open idf.py monitor after flashing
  --build-only     Build both firmware images, skip flashing
  --skip-bruce     Build/flash only Flipper; preserve the existing Bruce slot

Environment:
  ESPPORT                  Overrides the auto-detected serial device
  ESP_IDF_EXPORT_SCRIPT    Overrides the ESP-IDF export.sh path
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port|-p)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for $1" >&2
                usage
                exit 1
            fi
            PORT="$2"
            shift 2
            ;;
        --monitor|-m)
            RUN_MONITOR=1
            shift
            ;;
        --build-only)
            BUILD_ONLY=1
            shift
            ;;
        --skip-bruce)
            SKIP_BRUCE=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage
            exit 1
            ;;
    esac
done

if [[ -z "${PORT}" && "${BUILD_ONLY}" -eq 0 ]]; then
    PORT="$(detect_usbmodem_port)"
fi

if [[ ! -f "${EXPORT_SCRIPT}" ]]; then
    echo "ESP-IDF export script not found: ${EXPORT_SCRIPT}" >&2
    exit 1
fi

echo "Board:          ${BOARD}"
echo "Target:         ${IDF_TARGET}"
echo "Build dir:      ${BUILD_DIR}"
echo "Using ESP-IDF:  ${EXPORT_SCRIPT}"
echo "Serial port:    ${PORT}"

cd "${ESP32_DIR}"

if [[ "${SKIP_BRUCE}" -eq 0 ]]; then
    python3 tools/prepare_bruce.py
    PIO_BIN="$(command -v pio || command -v platformio || true)"
    if [[ -z "${PIO_BIN}" && -x "${HOME}/.platformio/penv/bin/pio" ]]; then
        PIO_BIN="${HOME}/.platformio/penv/bin/pio"
    fi
    if [[ -z "${PIO_BIN}" ]]; then
        echo "PlatformIO is required to build Bruce" >&2
        exit 1
    fi
    export PATH="$(dirname "${PIO_BIN}"):${PATH}"
    "${PIO_BIN}" run -d "${BRUCE_DIR}" -e lilygo-t-embed-cc1101
    if [[ ! -f "${BRUCE_BIN}" ]]; then
        echo "Bruce build did not produce ${BRUCE_BIN}" >&2
        exit 1
    fi
    BRUCE_SIZE="$(stat -f%z "${BRUCE_BIN}" 2>/dev/null || stat -c%s "${BRUCE_BIN}")"
    if (( BRUCE_SIZE > 0x5F0000 )); then
        echo "Bruce image (${BRUCE_SIZE} bytes) exceeds its 0x5F0000-byte slot" >&2
        exit 1
    fi
fi

# Refuse to interrupt another process using the selected serial port.
release_serial_port() {
    local port="$1"
    [[ -z "${port}" || ! -e "${port}" ]] && return 0
    if ! command -v lsof >/dev/null 2>&1; then return 0; fi
    local pids
    pids="$(lsof -t "${port}" 2>/dev/null || true)"
    if [[ -n "${pids}" ]]; then
        echo "Serial port ${port} is in use by PID(s): ${pids}" >&2
        return 1
    fi
}

echo
echo "=== Building this firmware ==="

if [[ "${BUILD_ONLY}" -eq 0 ]]; then
    release_serial_port "${PORT}"
fi

# shellcheck source=/dev/null
source "${EXPORT_SCRIPT}"

cd "${ESP32_DIR}"

# Set target again when an older single-app/dual-OTA sdkconfig is still present.
if [[ ! -f "${BUILD_DIR}/build.ninja" || ! -f sdkconfig ]] || \
   ! grep -q '^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_multiboot.csv"$' sdkconfig; then
    echo "Configuring ${IDF_TARGET} with the Bruce partition table..."
    idf.py -B "${BUILD_DIR}" -DSDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS}" set-target "${IDF_TARGET}"
fi

if [[ "${BUILD_ONLY}" -eq 1 ]]; then
    idf.py -B "${BUILD_DIR}" -DFLIPPER_BOARD="${BOARD}" \
        -DSDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS}" reconfigure build
    echo
    echo "Build complete (--build-only). Nothing flashed."
    exit 0
fi

idf.py -B "${BUILD_DIR}" -DFLIPPER_BOARD="${BOARD}" \
    -DSDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS}" -p "${PORT}" reconfigure build flash

if [[ "${SKIP_BRUCE}" -eq 0 ]]; then
    # ota_1 contains Bruce; erase otadata last to select Flipper's ota_0 as
    # the default even when the previous v2.0 firmware was running from ota_1.
    esptool.py --chip "${IDF_TARGET}" -p "${PORT}" --before default_reset --after no_reset \
        write_flash --flash_size detect 0x600000 "${BRUCE_BIN}"
    esptool.py --chip "${IDF_TARGET}" -p "${PORT}" --before default_reset --after hard_reset \
        erase_region 0xBF0000 0x2000
fi

if [[ "${RUN_MONITOR}" -eq 1 ]]; then
    release_serial_port "${PORT}"
    idf.py -B "${BUILD_DIR}" -p "${PORT}" monitor
fi
