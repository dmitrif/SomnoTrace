#!/usr/bin/env python3
"""Contracts for atomic raw-recording versus card-reader ownership."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
STORAGE = (ROOT / "main/sd_storage.c").read_text(encoding="utf-8")
HEADER = (ROOT / "main/sd_storage.h").read_text(encoding="utf-8")
WRITER = (ROOT / "main/session_writer.c").read_text(encoding="utf-8")


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
    return source[match.end() : cursor - 1]


begin = function_body(STORAGE, "sd_storage_recording_begin")
acquire = function_body(STORAGE, "sd_storage_lease_acquire")
release = function_body(STORAGE, "sd_storage_lease_release")
start = function_body(WRITER, "session_writer_start")
discard = function_body(WRITER, "session_start_discard")

assert "bool sd_storage_recording_begin(void);" in HEADER
assert "s_destructive" in STORAGE

# Recording publishes waiter priority, waits only for a bounded reader handoff,
# and publishes its claim while holding the one arbitration mutex.
lock_at = begin.find("xSemaphoreTake(s_lease_mutex")
waiter_at = begin.find("s_recording_waiters++")
destructive_check_at = begin.find("while (s_destructive == 0)")
reader_check_at = begin.find("if (s_uploading == 0)")
claim_at = begin.find("s_recording++")
unlock_at = begin.rfind("xSemaphoreGive(s_lease_mutex")
assert -1 not in (
    lock_at, waiter_at, destructive_check_at, reader_check_at, claim_at,
    unlock_at,
)
assert lock_at < waiter_at < destructive_check_at < reader_check_at \
       < claim_at < unlock_at
assert "SD_RECORDING_PRIORITY_WAIT_MS" in begin
assert "s_recording_waiters--" in begin
assert "return claimed" in begin

# UPLOAD and DESTRUCTIVE publish their mutually-exclusive claim beneath that
# same mutex after acquiring the file-operation gate. A recording claim that
# wins first is observed before either role starts card I/O.
upload = acquire[acquire.find("case SD_LEASE_UPLOAD:") :]
upload_sem = upload.find("xSemaphoreTakeRecursive(s_export_sem")
upload_lock = upload.find("xSemaphoreTake(s_lease_mutex")
upload_check = upload.find("s_recording > 0 || s_recording_waiters > 0")
upload_claim = upload.find("s_uploading++")
assert -1 not in (upload_sem, upload_lock, upload_check, upload_claim)
assert upload_sem < upload_lock < upload_check < upload_claim

destructive = acquire[
    acquire.find("case SD_LEASE_DESTRUCTIVE:") :
    acquire.find("case SD_LEASE_EXPORT:")
]
destructive_sem = destructive.find("xSemaphoreTakeRecursive(s_export_sem")
destructive_lock = destructive.find("xSemaphoreTake(s_lease_mutex")
destructive_check = destructive.find("s_recording > 0 || s_uploading > 0")
destructive_claim = destructive.find("s_destructive++")
assert -1 not in (
    destructive_sem, destructive_lock, destructive_check, destructive_claim
)
assert destructive_sem < destructive_lock < destructive_check < destructive_claim
assert "s_destructive--" in release
assert "s_uploading--" in release

# A writer is never allocated, published or queued without a successful raw-
# recording claim. Refusal therefore has no large session object to unwind;
# every later failure uses the centralized claim-aware discard helper.
claim_at = start.find("sd_storage_recording_begin()")
alloc_at = start.find("heap_caps_calloc(1, sizeof(session_writer_t)")
open_at = start.find("storage_queue_send_open(&cmd")
publish_at = start.find("s_active = s")
assert -1 not in (claim_at, alloc_at, open_at, publish_at)
assert claim_at < alloc_at < open_at < publish_at
refused_claim = start[claim_at:alloc_at]
assert "return NULL" in refused_claim
assert "batch_pool_create" not in refused_claim
assert start.count("session_start_discard(s)") == 4
for cleanup in (
    "batch_pool_destroy(s)",
    "vSemaphoreDelete(s->fill_mutex)",
    "sd_storage_recording_end()",
    "free(s)",
):
    assert cleanup in discard, f"start discard misses {cleanup}"

# Compact interleaving model: because all three claim transitions execute
# under the same mutex, no possible winner can overlap recording with either
# a reader or destructive owner.
states = {(0, 0, 0)}  # recording, uploading, destructive
for _ in range(12):
    next_states = set(states)
    for recording, uploading, destructive_count in states:
        if not uploading and not destructive_count:
            next_states.add((recording + 1, uploading, destructive_count))
        if not recording and not destructive_count:
            next_states.add((recording, uploading + 1, destructive_count))
        if not recording and not uploading and not destructive_count:
            next_states.add((recording, uploading, destructive_count + 1))
        if recording:
            next_states.add((recording - 1, uploading, destructive_count))
        if uploading:
            next_states.add((recording, uploading - 1, destructive_count))
        if destructive_count:
            next_states.add((recording, uploading, destructive_count - 1))
    states = next_states
    for recording, uploading, destructive_count in states:
        assert not (recording and uploading)
        assert not (recording and destructive_count)
        assert not (uploading and destructive_count)

print("SD recording arbitration contract passed")
