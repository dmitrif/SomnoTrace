#!/usr/bin/env python3
"""Structural safety/behavior contract for async rich History wiring."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "main/touch_history_controller.h").read_text(encoding="utf-8")
SOURCE = (ROOT / "main/touch_history_controller.c").read_text(encoding="utf-8")
SERVICE_H = (ROOT / "main/touch_history.h").read_text(encoding="utf-8")
SERVICE = (ROOT / "main/touch_history.c").read_text(encoding="utf-8")
UI = (ROOT / "main/touch_history_ui.c").read_text(encoding="utf-8")


def require(text: str, pattern: str, label: str) -> None:
    if not re.search(pattern, text, re.DOTALL):
        raise AssertionError(f"missing {label}")


def function_body(text: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", text, re.DOTALL)
    if not match:
        raise AssertionError(f"missing function body {name}")
    depth = 1
    index = match.end()
    while index < len(text) and depth:
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
        index += 1
    if depth:
        raise AssertionError(f"unterminated function body {name}")
    return text[match.start():index]


for api in (
    "touch_history_controller_create",
    "touch_history_controller_destroy",
    "touch_history_controller_set_active",
    "touch_history_controller_refresh",
    "touch_history_controller_handle_intent",
    "touch_history_controller_apply",
    "touch_history_controller_revision",
):
    require(HEADER, rf"\b{api}\b", f"public {api}")
    require(SOURCE, rf"\b{api}\b", f"implementation {api}")

# One long-lived serialized mailbox and one external-stack worker. Rapid
# intents replace pending work, while the in-flight generation is immutable.
require(SOURCE, r"HISTORY_CONTROLLER_QUEUE_LENGTH\s+1U", "one-slot mailbox")
require(SOURCE, r"xQueueCreate\(", "internal serialized queue")
require(SOURCE, r"xQueueOverwrite", "newest pending job wins")
require(SOURCE, r"psram_task_create\(.*?history_controller_worker",
        "PSRAM worker stack")
assert SOURCE.count("psram_task_create(") == 1
require(SOURCE, r"heap_caps_calloc\(.*?sizeof\(\*controller\).*?"
                r"MALLOC_CAP_SPIRAM", "PSRAM controller/model")
require(SOURCE, r"heap_caps_malloc\(.*?sizeof\(\*result\).*?"
                r"MALLOC_CAP_SPIRAM", "PSRAM inactive publication model")
require(SOURCE, r"vQueueDelete\(controller->queue\).*?"
                r"vSemaphoreDelete\(controller->mutex\).*?"
                r"heap_caps_free\(controller\)",
        "kernel object teardown before PSRAM context free")
assert SOURCE.count("history_model_t model;") == 1, (
    "only the PSRAM controller field may own the 480-bin publication model"
)

require(SOURCE, r"controller->generation\+\+.*?job->generation\s*=.*?"
                r"controller->generation", "generation assignment")
require(SOURCE, r"controller->generation\s*==\s*job->generation.*?"
                r"controller->model\s*=\s*\*result", "stale publish rejection")
require(SOURCE, r"history_controller_should_cancel.*?"
                r"history_controller_generation_current",
        "cancellable initial operation")
require(SOURCE, r"job->non_cancellable\s*\?\s*NULL\s*:\s*&operation",
        "non-cancellable ranged read")
require(SOURCE, r"ZOOM_LOADING", "explicit zoom-loading state")

# Complete navigation surface: initial newest selection, seven-row paging,
# calendar months, cross-page destination selection, fixed zoom tier and cursor.
require(SOURCE, r"HISTORY_JOB_INITIAL.*?result->days\[0\]\.day.*?"
                r"global_index\s*=\s*0", "newest auto-selection")
require(SOURCE, r"TOUCH_HISTORY_UI_LIST_ROWS", "seven-row page boundary")
require(SOURCE, r"HISTORY_JOB_MONTH.*?touch_history_load_month",
        "calendar month worker")
require(SOURCE, r"job->global_index.*?result->page\.offset.*?"
                r"history_controller_load_view", "cross-page prev/next")
require(SOURCE, r"global_index\s*==\s*SIZE_MAX.*?"
                r"touch_history_find_day_index.*?TOUCH_HISTORY_UI_LIST_ROWS",
        "calendar selection resolves its global page")
require(SOURCE, r"HISTORY_CONTROLLER_ZOOM_22_MIN_MS\s*"
                r"\(22LL\s*\*\s*60LL\s*\*\s*1000LL\)", "22-minute zoom")
require(SOURCE, r"cursor_ms.*?TOUCH_HISTORY_UI_INTENT_SELECT_CHANNEL.*?"
                r"window_start_ms", "cursor retained across channel reload")

# Graph, exact stats, and event markers all use the selected visible window.
require(SOURCE, r"touch_history_load_view_ex\(.*?window_start_ms.*?"
                r"window_end_ms", "therapy-aware overview/range load")
require(SOURCE, r"touch_history_load_stats_ex\(.*?window_start_ms.*?"
                r"window_end_ms", "visible-window source stats")
require(SOURCE, r"for \(;;\).*?touch_history_load_events_ex.*?"
                r"page\.has_more", "complete event paging")
require(SOURCE, r"event->end_ms\s*<\s*model->window_start_ms.*?"
                r"event->start_ms\s*>=\s*model->window_end_ms",
        "visible event filtering")
event_loader = function_body(SOURCE, "history_controller_load_events")
assert "if (result == ESP_ERR_NOT_FOUND) return ESP_OK;" not in event_loader, (
    "missing event sources must not collapse into a zero-event success"
)
require(
    event_loader,
    r"event_total_count\s*=\s*page\.total_count.*?"
    r"page\.totals\.complete.*?EVENT_STATE_COMPLETE.*?EVENT_STATE_INCOMPLETE",
    "complete-zero versus incomplete event provenance",
)
require(
    SOURCE,
    r"events_result\s*!=\s*ESP_OK.*?event_count\s*=\s*0.*?"
    r"event_total_count\s*=\s*0.*?EVENT_STATE_UNAVAILABLE",
    "unavailable event publication",
)
require(
    SOURCE,
    r"\.event_total_count\s*=\s*model->event_total_count.*?"
    r"\.event_state\s*=\s*model->event_state.*?"
    r"\.events_truncated\s*=\s*model->events_truncated",
    "event provenance forwarded to the UI",
)
assert "Event markers are unavailable for this night." not in SOURCE, (
    "event-only availability belongs in the non-overlay marker status"
)

for signal in ("PRESSURE", "LEAK", "FLOW_LIMIT", "SNORE"):
    require(SERVICE, rf"TOUCH_HISTORY_SIGNAL_{signal}", f"{signal} stats source")
for stat in (
    "ABSOLUTE_P50", "ABSOLUTE_P95", "ABSOLUTE_P995",
    "P50", "P95", "P995", "MINIMUM", "P5", "P05",
    "TIME_BELOW_88", "MEDIAN", "MAXIMUM",
):
    assert f"TOUCH_HISTORY_STAT_{stat}" in SERVICE_H
require(SERVICE, r"HISTORY_STATS_BIN_COUNT\s+32768U.*?"
                 r"heap_caps_calloc\(.*?MALLOC_CAP_SPIRAM",
        "bounded PSRAM source histogram")
require(SERVICE, r"history_flow_raw_candidate.*?"
                 r"history_stats_accumulate_as11", "raw Flow stats")
require(SERVICE, r"scaled\s*<\s*8800.*?below_88_ms",
        "SpO2 time below 88 percent")
require(SOURCE, r"TIME_BELOW_88.*?\?\s*\"min\"", "independent duration unit")
require(SERVICE, r"signal\s*==\s*TOUCH_HISTORY_SIGNAL_MOTION.*?"
                 r"stats->loaded\s*=\s*true", "truthful unavailable Motion")

# Accepted rich graph copy and corruption hardening.
assert '"Breathing / Flow"' in UI and '"L/s"' in UI
require(SERVICE, r"Rich Flow remains source-native hundredths L/s",
        "Flow L/s scale")
require(SERVICE, r"maximum_drift_ms.*?INT64_MAX\s*-\s*drift_ms.*?"
                 r"INT64_MIN\s*-\s*drift_ms", "checked drift addition")
require(SERVICE, r"clock_drift_usable\s*&&\s*"
                 r"touch_history_apply_clock_drift\(",
        "checked drift helper used by event publication")
require(SERVICE, r"event_dropped.*?touch_history_deduplicate_events.*?"
                 r"totals->complete\s*=\s*false.*?"
                 r"totals->has_indices\s*=\s*false",
        "dropped/replayed event truthfulness")
require(SERVICE_H, r"has_session_errors.*?has_oximetry_error.*?has_summary_error.*?has_event_loss",
        "night degradation provenance")
require(SOURCE, r"has_event_loss.*?ST AHI is unavailable",
        "event-loss degraded UI copy")
require(SOURCE, r"TOUCH_HISTORY_UI_INTENT_CLEAR_CURSOR.*?cursor_valid\s*=\s*false",
        "dismissible opt-in graph cursor")
require(SOURCE, r"stats_warning.*?Percentiles unavailable.*?"
                r"model->state\s*=\s*TOUCH_HISTORY_UI_STATE_READY",
        "statistics warning does not replace the usable graph")
require(SERVICE, r"retain_overlap_owners.*?has_selected_data.*?"
                 r"qsort\(candidates", "missing-aware O2 overlap ownership")

print("touch history controller contract passed")
