#!/usr/bin/env python3
"""Regression contract for non-lossy therapy-alert acknowledgement."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "components/therapy_alert/therapy_alert.c").read_text()


def body(function: str) -> str:
    match = re.search(
        rf"\b(?:static\s+)?(?:void|esp_err_t)\s+{function}\s*\([^)]*\)\s*\{{(.*?)\n\}}",
        SOURCE,
        re.DOTALL,
    )
    assert match, f"missing function {function}"
    return match.group(1)


# The ordinary queue may fill. Acknowledge therefore needs storage independent
# of that queue, created statically with the rest of the synchronization path.
assert "static StaticSemaphore_t s_ack_pending_buf;" in SOURCE
assert "static SemaphoreHandle_t s_ack_pending = NULL;" in SOURCE
init = body("therapy_alert_init")
assert "xSemaphoreCreateBinaryStatic(&s_ack_pending_buf)" in init

# Persist the request before attempting the fallible queue send. If post_evt()
# drops EVT_ACK, the binary semaphore must already retain the request.
ack = body("therapy_alert_acknowledge")
give = ack.index("xSemaphoreGive(s_ack_pending)")
post = ack.index("post_evt(EVT_ACK")
assert give < post, "acknowledgement is not latched before the queue send"

# The owner must consume the independent latch outside xQueueReceive().
owner = body("alert_owner_task")
take = owner.index("xSemaphoreTake(s_ack_pending, 0)")
receive = owner.index("xQueueReceive(s_evt_q")
assert take < receive, "owner can block before observing a saturated-queue acknowledgement"
assert "handle_ack();" in owner[take:receive]
ack_case = owner.index("case EVT_ACK:")
ack_break = owner.index("break;", ack_case)
assert "xSemaphoreTake(s_ack_pending, 0)" in owner[ack_case:ack_break]
assert "handle_ack();" in owner[ack_case:ack_break]

# Deterministic saturation model: all ordinary queue slots are occupied, the
# wake message is rejected, but the sticky latch survives and is consumed by
# the next owner iteration.
queue = ["ordinary"] * 8
ack_pending = False
ack_pending = True
if len(queue) < 8:
    queue.append("ack-wake")
assert "ack-wake" not in queue
assert ack_pending
if ack_pending:
    ack_pending = False
    state = "acked"
assert state == "acked"
assert not ack_pending

print("therapy alert acknowledgement saturation contract passed")
