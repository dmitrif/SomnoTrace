#!/usr/bin/env bash
# Synthetic host regressions; does not require ESP-IDF or patient data.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
TEST_DIR="$(mktemp -d /tmp/somnotrace-host-tests.XXXXXX)"
cleanup() {
    rm -R "${TEST_DIR}"
}
trap cleanup EXIT

cd "${PROJECT_DIR}"

cc -std=c11 -Wall -Wextra -I main \
    scripts/vld3_decoder_test.c main/oximetry_vld3.c \
    -o "${TEST_DIR}/vld3_decoder_test"
"${TEST_DIR}/vld3_decoder_test"

cc -std=c11 -Wall -Wextra -I scripts/test_include -I main \
    scripts/as11_events_test.c -o "${TEST_DIR}/as11_events_test"
"${TEST_DIR}/as11_events_test"

cc -std=c11 -Wall -Wextra -D_DARWIN_C_SOURCE \
    -I scripts/test_include -I main \
    scripts/as11_time_test.c main/as11_time.c \
    -o "${TEST_DIR}/as11_time_test"
"${TEST_DIR}/as11_time_test"

python3 scripts/waveshare_7b_contract_test.py
python3 scripts/qemu_ui_contract_test.py
python3 scripts/font_asset_contract_test.py
python3 scripts/screen_timeout_contract_test.py
python3 scripts/therapy_alert_ack_contract_test.py
python3 scripts/storage_notice_contract_test.py
python3 scripts/history_storage_lifecycle_contract_test.py
echo "All synthetic host tests passed"
