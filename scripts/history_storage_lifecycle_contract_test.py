#!/usr/bin/env python3
"""Contracts for safe History reads across a therapy-session stop."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
WRITER = (ROOT / "main/session_writer.c").read_text(encoding="utf-8")
HISTORY = (ROOT / "main/touch_history.c").read_text(encoding="utf-8")
HOST_TEST = (ROOT / "scripts/test-host.sh").read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if not match:
        raise AssertionError(f"missing function: {name}")
    depth = 1
    cursor = match.end()
    while cursor < len(source) and depth:
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
        cursor += 1
    if depth:
        raise AssertionError(f"unterminated function: {name}")
    return source[match.end(): cursor - 1]


finalize = function_body(WRITER, "storage_finalize")
request_finalize = function_body(WRITER, "sw_request_finalize")
post_task = function_body(WRITER, "sw_post_task")
history_load = function_body(HISTORY, "touch_history_load")
history_trace = function_body(HISTORY, "touch_history_load_flow_trace")

# `active` stops producers immediately, but the global recording claim remains
# held until all FILE*s and the terminal manifest are durable.
assert "sd_storage_recording_end" not in request_finalize, (
    "stop request publishes storage-idle before finalisation"
)
assert "recording_claim_held" in WRITER, "missing per-session recording claim"
close_at = finalize.find("fclose(")
manifest_at = finalize.find("write_manifest(")
idle_at = finalize.find("sd_storage_recording_end(")
assert -1 not in (close_at, manifest_at, idle_at), "incomplete finalise lifecycle"
assert close_at < manifest_at < idle_at, (
    "recording claim must outlive file close and terminal manifest"
)

# The post task may not time out and free a session still owned by sw_storage.
assert "xTaskNotifyGive(cmd.done_task)" in WRITER
assert "ulTaskNotifyTake(pdTRUE, portMAX_DELAY)" in post_task
assert "xSemaphoreTake(done, pdMS_TO_TICKS(60000))" not in post_task

# Both metadata and trace reads share one bounded wait across raw finalisation
# and the mutually-exclusive EDF/upload lease.
assert re.search(r"HISTORY_STORAGE_WAIT_MS\s+15000U", HISTORY)
assert "history_lease_acquire()" in history_load
assert "history_lease_acquire()" in history_trace
lease_helper = function_body(HISTORY, "history_lease_acquire")
assert "sd_storage_recording_active()" in lease_helper
assert "sd_storage_lease_acquire(SD_LEASE_UPLOAD, remaining_ms)" in lease_helper

# A FATFS enumeration error is not an empty night and is logged with errno.
assert "cannot open history root" in history_load
assert "cannot enumerate history root" in history_load
assert re.search(r"result\s*=\s*ESP_FAIL", history_load)
assert "history_storage_lifecycle_contract_test.py" in HOST_TEST

print("history storage lifecycle contract passed")
