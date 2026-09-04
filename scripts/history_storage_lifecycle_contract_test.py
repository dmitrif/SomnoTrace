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
finish_dispatch = function_body(WRITER, "storage_finish_and_dispatch")
request_finalize = function_body(WRITER, "sw_request_finalize")
post_task = function_body(WRITER, "sw_post_task")
storage_task = function_body(WRITER, "sw_storage_task")
start_session = function_body(WRITER, "session_writer_start")
history_load = function_body(HISTORY, "touch_history_load")
history_trace = function_body(HISTORY, "touch_history_load_trace")

# `active` stops producers immediately, but the global recording claim remains
# held until all seven FILE*s and the terminal manifest are durable.
assert "sd_storage_recording_end" not in request_finalize, (
    "stop request publishes storage-idle before finalisation"
)
assert "recording_claim_held" in WRITER, "missing per-session recording claim"
close_targets = (
    "&s->flow.f_l0", "&s->flow.f_l1", "&s->press.f_l0",
    "&s->sa2.f_l0", "&s->pld_f.f_l0", "&s->f_events", "&s->f_ckpt",
)
for target in close_targets:
    assert f"close_session_file(s, {target}" in finalize, (
        f"finalise does not close {target}"
    )
close_at = max(finalize.find(target) for target in close_targets)
manifest_at = finalize.find("write_manifest(")
idle_at = finalize.find("sd_storage_recording_end(")
assert -1 not in (close_at, manifest_at, idle_at), "incomplete finalise lifecycle"
assert close_at < manifest_at < idle_at, (
    "recording claim must outlive file close and terminal manifest"
)

# The storage worker owns the live session through close/free.  Post work gets
# copied metadata and can neither request a second finalise nor free the writer.
assert "storage_finish_and_dispatch(finished, &cmd)" in storage_task
dispatch_finalize_at = finish_dispatch.find("storage_finalize(s, cmd)")
dispatch_post_at = finish_dispatch.find("xQueueSend(s_post_q")
dispatch_free_at = finish_dispatch.rfind("free(s)")
assert -1 not in (dispatch_finalize_at, dispatch_post_at, dispatch_free_at)
assert dispatch_finalize_at < dispatch_post_at < dispatch_free_at
assert "session_writer_t *" not in post_task
assert "SW_CMD_FINALIZE" not in post_task
assert "free(s)" not in post_task
assert "done_task" not in WRITER

# STOP queues a nonblocking terminal command directly to storage while the
# lifecycle mutex is held.  Regular work reserves its slot; START queues OPEN
# before publishing s_active, so a later session cannot overtake the close.
assert "sw_post_job_t" not in request_finalize
assert "SW_CMD_FINALIZE" in request_finalize
assert "SW_STORAGE_TERMINAL_RESERVE 1" in WRITER
assert "xQueueSend(s_storage_q, &fin, 0)" in request_finalize
assert "xQueueSend(s_storage_q, &fin, portMAX_DELAY)" not in request_finalize
assert "storage_finalize(" not in request_finalize
assert "free(s)" not in request_finalize
assert "s->end_time_us = esp_timer_get_time()" in request_finalize
assert "producer_commit(s)" not in request_finalize
open_at = start_session.find("storage_queue_send_open(&cmd")
publish_at = start_session.find("s_active = s")
assert -1 not in (open_at, publish_at) and open_at < publish_at, (
    "OPEN must be queued before the session is published to producers"
)

# If the regular queue was saturated at STOP, the immutable final fill is
# written by the owner before the terminal durability commit.
tail_at = finalize.find("storage_write_batch(s, s->fill)")
commit_at = finalize.find("storage_commit(s)")
assert -1 not in (tail_at, commit_at) and tail_at < commit_at

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
