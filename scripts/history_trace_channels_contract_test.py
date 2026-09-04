#!/usr/bin/env python3
"""Regression contracts for the native History overnight channel picker."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HISTORY = (ROOT / "main/touch_history.c").read_text(encoding="utf-8")
HEADER = (ROOT / "main/touch_history.h").read_text(encoding="utf-8")
DISPLAY = (ROOT / "main/bsp_display_7b.c").read_text(encoding="utf-8")


def require(source: str, pattern: str, label: str) -> None:
    if not re.search(pattern, source, re.DOTALL):
        raise AssertionError(f"missing {label}")


# One selected trace stays compact; it is not multiplied across 30 day rows.
day_model = HEADER[HEADER.index("typedef struct {", HEADER.index("touch_history_trace_t")):
                   HEADER.index("} touch_history_day_t;")]
assert "points[" not in day_model, "every History day must not own channel samples"
require(HEADER, r"TOUCH_HISTORY_CHANNEL_FLOW.*?TOUCH_HISTORY_CHANNEL_SPO2.*?"
                r"TOUCH_HISTORY_CHANNEL_LEAK", "three native History channels")
require(DISPLAY, r"touch_history_trace_t\s+history_trace\s*;",
        "single selected-channel service cache")

# The v2 flow file has two int16 values per record. Index with the header's
# channel width, then preserve both bin extrema as parallel time series instead
# of flattening each bin or fabricating an alternating sawtooth.
require(HISTORY, r"record\s*=\s*&records\[r\s*\*\s*best\.n_channels\]",
        "channel-width-aware record stride")
assert "records[HISTORY_READ_RECORDS][4]" not in HISTORY
require(HEADER, r"upper_points\[TOUCH_HISTORY_TRACE_POINTS\]",
        "bounded upper edge for flow envelope")
require(HISTORY, r"points\[i\]\s*=.*?low_lpm.*?"
                 r"upper_points\[i\]\s*=.*?high_lpm",
        "parallel flow min/max envelope")
assert "points[i * 2]" not in HISTORY, \
       "flow extrema must not be serialised into one sawtooth series"
require(DISPLAY, r"s_history_trace_upper_series\s*=\s*lv_chart_add_series.*?"
                 r"upper_points\[i\].*?s_history_trace_upper_series",
        "second LVGL series for the flow envelope")
require(DISPLAY, r"s_history_channel\s*==\s*TOUCH_HISTORY_CHANNEL_FLOW\)\s*return;",
        "single-trace area fill disabled for flow envelope")

# SpO2 comes from the ready canonical O2 Ring SNT3 track, not the empty AS11
# SA2 placeholder. Desaturation nadirs and leak spikes survive decimation.
require(HISTORY, r"SD_OXYMETRY_DIR\s+\"/recordings/%s\".*?"
                 r"generations/%d/data/vitals\.snt", "canonical O2 Ring track")
require(HISTORY, r"OXIMETRY_CANONICAL_VITALS_SPO2.*?"
                 r"OXIMETRY_CANONICAL_VITALS_STATUS.*?&\s*1U",
        "canonical SpO2 value and quality gate")
require(HISTORY, r"while\s*\(checked\s*<\s*actual\).*?valid_spo2\+\+.*?"
                 r"candidate->valid_records\s*=\s*valid_spo2",
        "full-recording valid SpO2 coverage count")
require(HISTORY, r"candidate_valid_coverage_us\(&current\).*?"
                 r"coverage\s*>\s*best_coverage.*?duration\s*>\s*best_duration",
        "coverage-first O2 Ring candidate ranking")
require(HISTORY, r"channel\s*==\s*TOUCH_HISTORY_CHANNEL_SPO2.*?"
                 r"value\s*<\s*aggregate->trend\.extreme.*?"
                 r"else if\s*\(value\s*>\s*aggregate->trend\.extreme",
        "SpO2 nadir and Leak peak aggregation")
assert '"%s/%s_sa2.snt"' not in HISTORY, "History SpO2 must not use AS11 SA2"

# A published canonical package is either genuinely absent or readable.
# Allocation, partial reads, malformed pointers/manifests, and missing files
# referenced by a ready generation remain retryable failures.
require(HISTORY, r"static esp_err_t read_json_text.*?ESP_ERR_NO_MEM.*?ESP_FAIL",
        "typed JSON discovery failures")
require(HISTORY, r"validate_generation_manifest.*?"
                 r"result\s*==\s*ESP_ERR_NOT_FOUND\)\s*return\s+ESP_FAIL",
        "missing published generation is not cached as no-data")
require(HISTORY, r"pointer_result\s*==\s*ESP_ERR_NOT_FOUND\)\s*"
                 r"pointer_result\s*=\s*ESP_FAIL",
        "observed recording without pointer remains retryable")
require(HISTORY, r"discovery_error\s*!=\s*ESP_OK\)\s*return\s+discovery_error.*?"
                 r"return\s+ESP_ERR_NOT_FOUND",
        "discovery failures propagate before genuine absence")

# Rapid pill taps are latest-request-wins. Missing Ring data is a completed,
# truthful state while card/read failures remain retryable.
require(DISPLAY, r"request_generation\s*==\s*s_history_trace_request_generation",
        "generation-gated worker completion")
require(DISPLAY, r"queue_history_trace_load\(s_history_selected_day,\s*"
                 r"s_history_channel\)", "channel-specific touch request")
require(DISPLAY, r'"No O₂ Ring data for this night"', "truthful Ring empty state")
require(DISPLAY, r'"O₂ Ring data unavailable - tap SpO₂ to retry"',
        "retryable Ring read error")
assert "lv_obj_add_state(s_history_channel_buttons[i], LV_STATE_DISABLED)" not in DISPLAY

print("history trace channel contract passed")
