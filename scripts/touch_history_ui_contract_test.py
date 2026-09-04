#!/usr/bin/env python3
"""Structural contracts for the bounded Rev B native History surface."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "main/touch_history_ui.h").read_text(encoding="utf-8")
SOURCE = (ROOT / "main/touch_history_ui.c").read_text(encoding="utf-8")


def require(text: str, pattern: str, description: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(f"missing History UI contract: {description}")


# The surface consumes the exact persistent-content geometry.
for pattern, description in (
    (r"TOUCH_HISTORY_UI_WIDTH\s+992\b", "992-pixel content width"),
    (r"TOUCH_HISTORY_UI_HEIGHT\s+450\b", "450-pixel content height"),
    (r"TOUCH_HISTORY_UI_LIST_WIDTH\s+288\b", "288-pixel list"),
    (r"TOUCH_HISTORY_UI_COLUMN_GAP\s+12\b", "12-pixel gutter"),
    (r"TOUCH_HISTORY_UI_DETAIL_X\s+300\b", "detail x coordinate"),
    (r"TOUCH_HISTORY_UI_DETAIL_WIDTH\s+692\b", "692-pixel detail"),
    (r"HISTORY_UI_HIT\s+44\b", "44-pixel minimum controls"),
    (r"HISTORY_UI_SUMMARY_H\s+44\b", "44-pixel summary strip"),
    (r"HISTORY_UI_LIST_ROW_H\s+60\b", "60-pixel recycled rows"),
):
    require(HEADER + SOURCE, pattern, description)

# Object count stays bounded on an ESP32: seven recycled rows, eight recycled
# channel controls, one graph draw object, and no chart/canvas point objects.
require(HEADER, r"TOUCH_HISTORY_UI_LIST_ROWS\s+7\b", "seven list rows")
require(
    HEADER,
    r"TOUCH_HISTORY_UI_CHANNEL_CONTROLS\s+TOUCH_HISTORY_SIGNAL_COUNT",
    "all eight channel controls",
)
require(
    SOURCE,
    r"rows\[TOUCH_HISTORY_UI_LIST_ROWS\].*?"
    r"channels\[TOUCH_HISTORY_UI_CHANNEL_CONTROLS\]",
    "fixed object arrays",
)
require(
    SOURCE,
    r"for\s*\(size_t i = 0; i < TOUCH_HISTORY_UI_LIST_ROWS; \+\+i\)",
    "reused row creation loop",
)
require(
    SOURCE,
    r"for\s*\(size_t i = 0; i < TOUCH_HISTORY_UI_CHANNEL_CONTROLS; \+\+i\)",
    "reused channel creation loop",
)
assert "lv_chart_create" not in SOURCE, "History must not allocate an LVGL chart"
assert "lv_canvas_create" not in SOURCE, "History must not allocate a framebuffer canvas"
assert SOURCE.count("history_ui_graph_draw,") == 1, "exactly one graph draw object"

# The handoff's left rail is a List/Calendar segmented control over a clipped,
# scroll-recycled list. It never regresses to a numeric pager footer.
for copy in ("List", "Calendar", "RECORDED\\nNIGHTS", "Jump to date"):
    assert copy in SOURCE, f"missing left-rail copy {copy}"
require(
    SOURCE,
    r"list_viewport.*?LV_OBJ_FLAG_SCROLLABLE.*?"
    r"LV_EVENT_SCROLL_END",
    "scroll-driven page recycling",
)
assert "page_previous" not in SOURCE
assert "page_next" not in SOURCE
assert "page_label" not in SOURCE
require(
    SOURCE,
    r"channels\[i\]\.pill = history_ui_container\(.*?,\s*31,",
    "31-pixel visual channel pill inside its 44-pixel target",
)
assert "on target" in SOURCE and "usage_target_known" in HEADER

# Calendar cells are one lazy draw/touch grid rather than 42 LVGL buttons.
require(
    SOURCE,
    r"if\s*\(ui->calendar_overlay\)\s*return ESP_OK",
    "lazy calendar allocation guard",
)
require(
    SOURCE,
    r"state == TOUCH_HISTORY_UI_STATE_CALENDAR.*?"
    r"history_ui_ensure_calendar",
    "calendar allocation only on calendar state",
)
assert SOURCE.count("ui->calendar_grid = history_ui_container(") == 1
assert "42 calendar cells from becoming 42 objects" in SOURCE

# The retained 480-point model and all copied arrays live in PSRAM only.
require(
    SOURCE,
    r"heap_caps_calloc\(\s*1,\s*sizeof\(\*ui\),\s*"
    r"MALLOC_CAP_SPIRAM\s*\|\s*MALLOC_CAP_8BIT\)",
    "PSRAM-only context allocation",
)
assert SOURCE.count("heap_caps_calloc(") == 1, "internal-RAM fallback is forbidden"
assert re.search(r"touch_history_overview_t\s+overview;", SOURCE)
require(
    SOURCE,
    r"ui->overview\s*=\s*\*snapshot->overview",
    "deep-copied graph snapshot",
)
require(SOURCE, r"heap_caps_free\(ui\)", "PSRAM release")

# All required worker/BSP states and touch intents are explicit and typed.
for state in (
    "EMPTY",
    "AUTO_LOADING",
    "READY",
    "CALENDAR",
    "ZOOM_LOADING",
    "READ_ERROR",
    "DEGRADED_UNKNOWN",
):
    assert f"TOUCH_HISTORY_UI_STATE_{state}" in HEADER, f"missing state {state}"

for intent in (
    "SELECT_DAY",
    "PAGE_RELATIVE",
    "OPEN_CALENDAR",
    "CLOSE_CALENDAR",
    "MONTH_RELATIVE",
    "SELECT_CALENDAR_DAY",
    "SELECT_CHANNEL",
    "PREVIOUS_NIGHT",
    "NEXT_NIGHT",
    "CANCEL_AUTO_LOAD",
    "RETRY_READ",
    "OPEN_CARD",
    "FIT_NIGHT",
    "ZOOM_RELATIVE",
    "PAN_RELATIVE",
    "SET_CURSOR",
    "CLEAR_CURSOR",
    "TOGGLE_THERAPY_ONLY",
):
    assert f"TOUCH_HISTORY_UI_INTENT_{intent}" in HEADER, f"missing intent {intent}"

require(
    SOURCE,
    r"history_ui_cancel_pressed.*?"
    r"ui->state != TOUCH_HISTORY_UI_STATE_AUTO_LOADING",
    "cancel is restricted to initial night load",
)
require(
    SOURCE,
    r"LV_EVENT_SHORT_CLICKED.*?TOUCH_HISTORY_UI_INTENT_CLEAR_CURSOR",
    "repeat short tap dismisses the graph cursor without stealing pan",
)
require(
    SOURCE,
    r"stats_warning_text.*?stats_warning.*?"
    r"history_ui_set_hidden\(ui->stat_labels",
    "stats-only warning is compact in the header rather than graph overlay",
)
require(
    SOURCE,
    r"TOUCH_HISTORY_UI_STATE_ZOOM_LOADING.*?"
    r"Intentionally no cancel control exists in this overlay",
    "zoom loading is honestly non-cancellable",
)
for callback in (
    "history_ui_channel_pressed",
    "history_ui_open_calendar_pressed",
    "history_ui_retry_pressed",
    "history_ui_fit_pressed",
    "history_ui_zoom_in_pressed",
):
    require(
        SOURCE,
        rf"{callback}.*?LV_EVENT_PRESSED",
        f"{callback} emits from touch-down",
    )
require(
    SOURCE,
    r"row->button,\s*history_ui_row_pressed,\s*"
    r"LV_EVENT_SHORT_CLICKED",
    "scrollable rows select only after a short click",
)
assert not re.search(
    r"row->button,\s*history_ui_row_pressed,\s*LV_EVENT_PRESSED",
    SOURCE,
), "row touch-down must not race scroll arbitration"
assert "history_ui_list_scroll_end" in SOURCE

# Truthful graph states: full-night Flow envelope, 22-minute raw zoom source,
# Pressure/EPR companion, therapy-only SpO2, explicit gaps and event taxonomy.
assert '"Breathing / Flow"' in SOURCE, "accepted rich Flow title is required"
assert '"L/s"' in SOURCE, "rich Flow must remain source-native L/s"
assert "source_raw" in SOURCE and "raw 25 Hz" in SOURCE
assert "1 Hz fallback" in SOURCE
assert "TOUCH_HISTORY_AGGREGATION_ENVELOPE" in SOURCE
assert "TOUCH_HISTORY_POINT_UPPER_VALID" in SOURCE
assert "TOUCH_HISTORY_POINT_COMPANION_VALID" in SOURCE
assert "Pressure + EPR" in SOURCE
assert "availability_y" in SOURCE, "O2 channels need a binary availability strip"
require(
    SOURCE,
    r"signal == TOUCH_HISTORY_SIGNAL_SPO2.*?therapy_only.*?"
    r"TOUCH_HISTORY_POINT_THERAPY",
    "SpO2 during-therapy-only filter",
)
require(
    SOURCE,
    r"else\s*\{\s*previous = SIZE_MAX;\s*\}",
    "missing bins break trace segments",
)
assert "TOUCH_HISTORY_EVENT_GENERIC_APNEA" in SOURCE
assert '"A"' in SOURCE, "generic apnea keeps a distinct marker"
assert "cursor_valid" in SOURCE and "history_ui_cursor_event" in SOURCE
for rich_graph_copy in (
    "SESSION %u · %s–%s",
    "Markers: OA · CA · H · A · RERA",
    "Trend review only. Not a diagnosis or a prescription.",
):
    assert rich_graph_copy in SOURCE, f"missing rich graph detail: {rich_graph_copy}"
require(
    SOURCE,
    r"for\s*\(unsigned tick = 0; tick <= 4; \+\+tick\).*?"
    r"history_ui_format_clock",
    "five labelled time ticks",
)
require(
    SOURCE,
    r"minimum = -magnitude;\s*maximum = magnitude;",
    "Flow axis includes an exact zero tick",
)

# Page/month metadata avoids the old 30-night product cap. Unknown stays an
# em dash, and ST AHI never silently replaces the dim Device AHI.
assert "TOUCH_HISTORY_MAX_DAYS" not in HEADER + SOURCE
assert "page.total_days" in SOURCE and "page.has_more" in SOURCE
assert r'"\xE2\x80\x94"' in SOURCE
for label in ("Usage", "ST AHI", "Device AHI", "Recorded", "O₂ coverage"):
    assert f'"{label}"' in SOURCE, f"missing summary field {label}"
require(
    SOURCE,
    r"i == 2 \? HISTORY_UI_COLOR_TERTIARY",
    "Device AHI is visually secondary",
)
assert "stats[TOUCH_HISTORY_UI_STAT_COUNT]" in HEADER
assert "not invent percentiles" in HEADER
require(HEADER, r"TOUCH_HISTORY_UI_STAT_COUNT\s+4\b", "four SpO2 stat slots")
for label in (
    "P50 |Flow|", "P95 |Flow|", "P99.5 |Flow|",
    "P50", "P95", "P99.5", "Minimum", "P5", "P0.5", "Time <88%",
    "Median", "Maximum",
):
    assert f'"{label}"' in SOURCE, f"missing source-stat label {label}"
require(
    SOURCE,
    r"i < 3 \|\| ui->signal == TOUCH_HISTORY_SIGNAL_SPO2",
    "fourth stat is reserved for SpO2",
)
assert "const char *unit;" in HEADER, "time-below-88 needs a duration unit"
assert "history_ui_set_hidden(ui->graph_title" not in SOURCE
assert "history_ui_set_hidden(ui->graph_source" not in SOURCE

# Snapshot bounds are rejected, not truncated, and zoom can retain the last
# resolved plot while the raw SD reread runs.
for bound in (
    "TOUCH_HISTORY_UI_LIST_ROWS",
    "TOUCH_HISTORY_UI_MAX_SESSIONS",
    "TOUCH_HISTORY_UI_MAX_VISIBLE_EVENTS",
):
    require(
        SOURCE,
        rf"snapshot->(?:day_count|session_count|event_count) > {bound}",
        f"reject overflow for {bound}",
    )
require(
    SOURCE,
    r"snapshot->overview.*?state != TOUCH_HISTORY_UI_STATE_ZOOM_LOADING",
    "retain resolved waveform during zoom read",
)

print("Rev B native History UI contract passed")
