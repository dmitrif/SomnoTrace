#!/usr/bin/env bash
# Launch the SomnoTrace ESP32-S3 UI preview in Espressif QEMU.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
if [ "${1:-}" = "--build" ]; then
    "${SCRIPT_DIR}/build-qemu.sh"
elif [ "$#" -ne 0 ]; then
    echo "Usage: $0 [--build]" >&2
    exit 2
fi

FLASH="${PROJECT_DIR}/build-qemu/qemu_flash.bin"
EFUSE="${PROJECT_DIR}/build-qemu/qemu_efuse.bin"
if [ ! -f "${FLASH}" ] || [ ! -f "${EFUSE}" ]; then
    "${SCRIPT_DIR}/build-qemu.sh"
fi
QEMU_BIN="$("${SCRIPT_DIR}/setup-qemu-macos.sh")"

exec "${QEMU_BIN}" \
    -M esp32s3 \
    -m 8M \
    -drive "file=${FLASH},if=mtd,format=raw" \
    -drive "file=${EFUSE},if=none,format=raw,id=efuse" \
    -global driver=nvram.esp32s3.efuse,property=drive,value=efuse \
    -global driver=timer.esp32s3.timg,property=wdt_disable,value=true \
    -nic user,model=open_eth \
    -display sdl,show-cursor=on \
    -serial mon:stdio
