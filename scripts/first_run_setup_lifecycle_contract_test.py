#!/usr/bin/env python3
"""Lifecycle contracts for the self-reclaiming first-run controller."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main/first_run_setup_controller.c").read_text(encoding="utf-8")
HEADER = (ROOT / "main/first_run_setup_controller.h").read_text(encoding="utf-8")


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


start = function_body(SOURCE, "first_run_setup_controller_start")
stop = function_body(SOURCE, "first_run_setup_controller_stop")
worker = function_body(SOURCE, "setup_worker")
acquire = function_body(SOURCE, "controller_acquire")
release = function_body(SOURCE, "controller_release")
snapshot = function_body(SOURCE, "first_run_setup_controller_snapshot")
finished = function_body(SOURCE, "first_run_setup_controller_take_finished")

# A detached controller blocks restart until its worker has actually reclaimed
# every owned object. This is a lifecycle state, not an intentionally retained
# diagnostic snapshot.
assert "s_controller != NULL || s_controller_stopping" in start
assert "reclaims" in HEADER
detach_at = stop.find("s_controller = NULL")
stopping_at = stop.find("s_controller_stopping = true")
enqueue_at = stop.find("xQueueOverwrite")
assert -1 not in (detach_at, stopping_at, enqueue_at)
assert detach_at < stopping_at < enqueue_at
assert "controller->active = false" in stop

# A snapshot pins the controller before dropping the singleton spinlock. The
# worker waits for those short readers; no code blocks on an RTOS semaphore
# while inside a critical section.
assert "controller->references++" in acquire
assert "controller->references--" in release
assert "controller_acquire()" in snapshot
assert "controller_release(controller)" in snapshot
assert "controller_acquire()" in finished
assert "controller_release(controller)" in finished
for body in (acquire, release):
    assert "xSemaphoreTake" not in body

# The final worker owner releases queue, mutex, controller allocation and its
# WithCaps task stack, then reopens the singleton start gate.
stop_case = worker[worker.rfind('ESP_LOGI(TAG, "setup worker stopped")') :]
for cleanup in (
    "controller->references != 0",
    "vQueueDelete(queue)",
    "vSemaphoreDelete(mutex)",
    "free(controller)",
    "s_controller_stopping = false",
    "psram_task_delete(NULL)",
):
    assert cleanup in stop_case, f"worker teardown misses {cleanup}"
assert stop_case.find("free(controller)") < stop_case.find(
    "s_controller_stopping = false"
) < stop_case.find("psram_task_delete(NULL)")

print("first-run setup lifecycle contract passed")
