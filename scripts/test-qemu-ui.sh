#!/usr/bin/env bash
# Headless boot smoke test for the QEMU UI firmware.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
FLASH="${PROJECT_DIR}/build-qemu/qemu_flash.bin"
EFUSE="${PROJECT_DIR}/build-qemu/qemu_efuse.bin"
if [ ! -f "${FLASH}" ] || [ ! -f "${EFUSE}" ]; then
    "${SCRIPT_DIR}/build-qemu.sh"
fi
QEMU_BIN="$("${SCRIPT_DIR}/setup-qemu-macos.sh")"
TEMP_DIR="$(mktemp -d /tmp/somnotrace-qemu-test.XXXXXX)"
LOG="${TEMP_DIR}/uart.log"
QEMU_PID=""
cleanup() {
    if [ -n "${QEMU_PID}" ] && kill -0 "${QEMU_PID}" 2>/dev/null; then
        kill "${QEMU_PID}" 2>/dev/null || true
        for _ in $(seq 1 10); do
            kill -0 "${QEMU_PID}" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "${QEMU_PID}" 2>/dev/null; then
            kill -9 "${QEMU_PID}" 2>/dev/null || true
        fi
    fi
    wait "${QEMU_PID}" 2>/dev/null || true
    rm -rf "${TEMP_DIR}"
}
trap cleanup EXIT

"${QEMU_BIN}" \
    -M esp32s3 \
    -m 8M \
    -drive "file=${FLASH},if=mtd,format=raw" \
    -drive "file=${EFUSE},if=none,format=raw,id=efuse" \
    -global driver=nvram.esp32s3.efuse,property=drive,value=efuse \
    -global driver=timer.esp32s3.timg,property=wdt_disable,value=true \
    -nic user,model=open_eth \
    -display none -monitor none -serial "file:${LOG}" &
QEMU_PID=$!

for _ in $(seq 1 30); do
    if grep -Eq "Invalid drawing area|assert failed|Guru Meditation Error|abort\(\) was called" "${LOG}" 2>/dev/null; then
        echo "QEMU reported a display or firmware failure" >&2
        cat "${LOG}" >&2 || true
        exit 1
    fi
    if grep -q "1024x600 interactive UI preview ready" "${LOG}" 2>/dev/null; then
        sleep 2
        if grep -Eq "Invalid drawing area|assert failed|Guru Meditation Error|abort\(\) was called" "${LOG}"; then
            echo "QEMU reported a display or firmware failure after startup" >&2
            cat "${LOG}" >&2 || true
            exit 1
        fi
        echo "QEMU UI boot smoke test passed"
        exit 0
    fi
    if ! kill -0 "${QEMU_PID}" 2>/dev/null; then
        echo "QEMU exited before the UI became ready" >&2
        cat "${LOG}" >&2 || true
        exit 1
    fi
    sleep 1
done

echo "Timed out waiting for QEMU UI" >&2
cat "${LOG}" >&2 || true
exit 1
