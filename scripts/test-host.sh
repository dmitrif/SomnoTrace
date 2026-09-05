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

cc -std=c11 -Wall -Wextra \
    -I scripts/test_include -I main \
    scripts/first_run_setup_test.c main/first_run_setup_model.c \
    -o "${TEST_DIR}/first_run_setup_test"
"${TEST_DIR}/first_run_setup_test"

cc -std=c11 -Wall -Wextra -DTIMEZONE_CATALOG_HOST_TEST \
    -I scripts/test_include -I main \
    scripts/timezone_catalog_test.c main/timezone_catalog.c \
    -o "${TEST_DIR}/timezone_catalog_test"
"${TEST_DIR}/timezone_catalog_test"

cc -std=c11 -Wall -Wextra -DTOUCH_HISTORY_MODEL_TEST \
    -I scripts/test_include -I main \
    scripts/touch_history_model_test.c main/touch_history.c -lm \
    -o "${TEST_DIR}/touch_history_model_test"
"${TEST_DIR}/touch_history_model_test"

python3 scripts/waveshare_7b_contract_test.py
python3 scripts/qemu_ui_contract_test.py
python3 scripts/font_asset_contract_test.py
python3 scripts/screen_timeout_contract_test.py
python3 scripts/therapy_alert_ack_contract_test.py
python3 scripts/storage_notice_contract_test.py
python3 scripts/storage_status_memory_contract_test.py
python3 scripts/status_sd_cache_contract_test.py
python3 scripts/upload_progress_snapshot_contract_test.py
python3 scripts/psram_task_lifecycle_contract_test.py
python3 scripts/history_storage_lifecycle_contract_test.py
python3 scripts/history_trace_channels_contract_test.py
python3 scripts/touch_history_service_contract_test.py
python3 scripts/history_responsiveness_contract_test.py
python3 scripts/history_flow_envelope_contract_test.py
python3 scripts/touch_history_ui_contract_test.py
python3 scripts/clinical_ui_language_contract_test.py
python3 scripts/touch_history_controller_contract_test.py
python3 scripts/touch_history_integration_contract_test.py
python3 scripts/rapid_session_lifecycle_contract_test.py
python3 scripts/session_writer_start_resilience_contract_test.py
python3 scripts/sd_recording_arbitration_contract_test.py
python3 scripts/recording_priority_contract_test.py
python3 scripts/sd_mount_fallback_contract_test.py
python3 scripts/clock_snapshot_contract_test.py
python3 scripts/live_flow_units_contract_test.py
python3 scripts/first_run_setup_contract_test.py
python3 scripts/first_run_setup_ui_contract_test.py
python3 scripts/first_run_setup_runtime_contract_test.py
python3 scripts/first_run_setup_lifecycle_contract_test.py
python3 scripts/timezone_catalog_contract_test.py
python3 scripts/netprov_scan_contract_test.py
python3 scripts/ota_memory_safety_contract_test.py
python3 scripts/ota_rollback_contract_test.py
python3 scripts/therapy_lifecycle_race_contract_test.py
python3 scripts/oximetry_contract_test.py
python3 scripts/oximetry_sd_lease_contract_test.py
python3 scripts/oximetry_forget_tombstone_contract_test.py
python3 scripts/uploader_ox_lease_contract_test.py
python3 scripts/log_stream_retained_contract_test.py
python3 scripts/log_stream_recent_contract_test.py
python3 scripts/log_stream_resilience_contract_test.py
python3 scripts/logs_touch_ui_contract_test.py
python3 scripts/touch_logs_ui_contract_test.py
python3 scripts/manage_detail_lifecycle_contract_test.py
echo "All synthetic host tests passed"
