#!/usr/bin/env bash
# Build the ESP32-S3 QEMU-only 1024x600 UI preview firmware.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="build-qemu"
SDKCONFIG="sdkconfig.qemu"
DEFAULTS="sdkconfig.defaults;sdkconfig.qemu.defaults"

if [ "${1:-}" = "--clean" ]; then
    "${SCRIPT_DIR}/idf.sh" -B "${BUILD_DIR}" -D "SDKCONFIG=${SDKCONFIG}" fullclean || true
elif [ "$#" -ne 0 ]; then
    echo "Usage: $0 [--clean]" >&2
    exit 2
fi

"${SCRIPT_DIR}/idf.sh" -B "${BUILD_DIR}" \
    -D "SDKCONFIG=${SDKCONFIG}" \
    -D "SDKCONFIG_DEFAULTS=${DEFAULTS}" build

"${SCRIPT_DIR}/idf.sh" exec bash -lc \
    "cd /project/${BUILD_DIR} && python -m esptool --chip esp32s3 merge_bin --output qemu_flash.bin --fill-flash-size 16MB @flash_args"

# Advertise the ESP32-S3 revision expected by ESP-IDF during early startup.
dd if=/dev/zero of="${PROJECT_DIR}/${BUILD_DIR}/qemu_efuse.bin" bs=336 count=1 2>/dev/null
printf '\014' | dd of="${PROJECT_DIR}/${BUILD_DIR}/qemu_efuse.bin" \
    bs=1 seek=38 conv=notrunc 2>/dev/null

echo
echo "QEMU UI firmware ready:"
echo "  ${PROJECT_DIR}/${BUILD_DIR}/qemu_flash.bin"
