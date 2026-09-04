#!/usr/bin/env python3
"""Static contracts for the Rev B3 native Logs screen and Manage rail."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
DISPLAY = (ROOT / "main/bsp_display_7b.c").read_text(encoding="utf-8")
HEADER = (ROOT / "main/log_stream.h").read_text(encoding="utf-8")


def require(text: str, pattern: str, description: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(f"missing native Logs contract: {description}")


require(DISPLAY, r"#define\s+MANAGE_SECTION_COUNT\s+8\b",
        "eight-destination Manage rail")
rail = DISPLAY.split("static void build_manage_page", 1)[1].split(
    "static void update_nav_styles", 1)[0]
for destination in (
    "Devices", "Connectivity", "Alerts", "Uploads",
    "Storage", "System", "Logs", "Advanced",
):
    assert f'"{destination}"' in rail, f"Manage rail omits {destination}"
require(rail, r"i\s*\*\s*52,\s*212,\s*48",
        "compact rail rows retain 48px touch height")
assert '"Display"' not in rail, "Display must be folded under System"
require(DISPLAY,
        r"build_system_section.*?build_display_controls\(scroll,\s*244\)",
        "display preferences live under System")

logs = DISPLAY.split("static void build_logs_section", 1)[1].split(
    "static void build_system_section", 1)[0]
require(DISPLAY, r"#define\s+LOG_VISIBLE_ROWS\s+10\b",
        "ten fixed visible log rows")
for label in (
    "Pause", "Clear", "Save to card", "Filter by tag or message",
    "Error", "Warn", "Info", "Debug", "Jump to newest",
):
    assert f'"{label}"' in logs, f"Logs screen omits {label}"
require(logs, r"make_touch_button\([^;]*?76,\s*44,\s*\"Pause\"",
        "Pause has at least a 44px target")
require(logs, r"make_touch_button\([^;]*?72,\s*44,\s*\"Clear\"",
        "Clear has at least a 44px target")
require(logs, r"lv_obj_set_size\(s_logs_query,\s*330,\s*44\)",
        "search field has a 44px target")
require(logs, r"s_logs_times\[i\].*?s_logs_levels\[i\].*?"
              r"s_logs_tags\[i\].*?s_logs_messages\[i\]",
        "fixed time, level, tag, and message columns")

# Search freezes the readable viewport. Filtering and pausing are UI state;
# they never stop capture in the independent retained feed.
require(DISPLAY,
        r"logs_query_focus_cb.*?logs_set_paused\(true\).*?open_keyboard_sheet",
        "search pauses before opening the keyboard")
pause_body = DISPLAY.split("static void logs_set_paused", 1)[1].split(
    "static void logs_pause_cb", 1)[0]
for forbidden in ("log_stream_retained_clear", "log_stream_init", "vTaskSuspend"):
    assert forbidden not in pause_body, f"Pause incorrectly calls {forbidden}"
require(DISPLAY,
        r"s_logs_level_mask\s*=\s*LOG_STREAM_RETAINED_LEVEL_ERROR\s*\|"
        r".*?WARN\s*\|.*?INFO;",
        "Debug is off by default")
require(DISPLAY,
        r"Stream disconnected - therapy and recording continue",
        "disconnected state says bedside functions continue")

# Card export runs away from LVGL, reports determinate progress, and Clear is
# disabled while that snapshot owns its source view.
require(HEADER, r"typedef void \(\*log_stream_retained_progress_fn\)",
        "card-save progress callback")
require(DISPLAY,
        r"logs_save_task.*?log_stream_retained_save_to_sd\(.*?"
        r"logs_save_progress.*?psram_task_delete",
        "card export runs on a PSRAM-stack worker")
require(DISPLAY, r"Saving log snapshot to card - %u%%",
        "determinate save progress copy")
require(DISPLAY,
        r"set_control_disabled\(s_logs_clear_button,\s*"
        r"save_busy\s*\|\|",
        "Clear cannot race an active card snapshot")

print("native Logs and Manage rail contract passed")
