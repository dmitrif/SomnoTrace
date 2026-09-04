#!/usr/bin/env python3
"""Static contracts for the persisted display inactivity timeout."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "main/device_settings.h").read_text(encoding="utf-8")
SOURCE = (ROOT / "main/device_settings.c").read_text(encoding="utf-8")
PORTAL = (ROOT / "main/portal.html").read_text(encoding="utf-8")
DISPLAY_HEADER = (ROOT / "main/bsp_display.h").read_text(encoding="utf-8")
TOUCH_BSP = (ROOT / "main/bsp_display_7b.c").read_text(encoding="utf-8")
COMPACT_BSP = (ROOT / "main/bsp_display.c").read_text(encoding="utf-8")
HOST_TEST = (ROOT / "scripts/test-host.sh").read_text(encoding="utf-8")
BODY_FONT = (
    ROOT / "assets/fonts/generated/somnotrace_space_grotesk_medium_15.c"
).read_text(encoding="utf-8")


def require(text: str, pattern: str, description: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(f"missing screen-timeout contract: {description}")


require(HEADER, r"uint16_t\s+screen_timeout_s\s*;",
        "timeout is stored as explicit seconds")
require(HEADER,
        r"device_settings_set_screen_timeout_s\s*\(\s*uint16_t\s+seconds\s*\)",
        "validated public setter")

require(SOURCE, r'#define\s+NVS_KEY_SCREEN_TIMEOUT\s+"scr_tmo"',
        "stable NVS key")
require(SOURCE,
        r"CONFIG_SOMNOTRACE_BOARD_WAVESHARE_7B\s*\|\|\s*"
        r"CONFIG_SOMNOTRACE_BOARD_QEMU.*?DEFAULT_SCREEN_TIMEOUT_S\s+300"
        r".*?#else.*?DEFAULT_SCREEN_TIMEOUT_S\s+0",
        "five-minute 7B/QEMU default and disabled compact-board default")

validator = re.search(
    r"static bool screen_timeout_is_valid\s*\([^)]*\)\s*\{(.*?)\n\}",
    SOURCE, re.MULTILINE | re.DOTALL)
if not validator:
    raise AssertionError("screen-timeout validator is missing")
allowed = {int(value) for value in re.findall(r"case\s+(\d+)\s*:",
                                               validator.group(1))}
assert allowed == {0, 60, 300, 900, 1800}, (
    f"screen-timeout values changed: {sorted(allowed)}")

require(SOURCE,
        r"cfg->screen_timeout_s\s*=\s*DEFAULT_SCREEN_TIMEOUT_S.*?"
        r"nvs_get_u16\s*\(\s*h\s*,\s*NVS_KEY_SCREEN_TIMEOUT",
        "default-first backward-compatible NVS load")
require(SOURCE,
        r"nvs_set_u16\s*\(\s*h\s*,\s*NVS_KEY_SCREEN_TIMEOUT\s*,\s*"
        r"cfg\.screen_timeout_s\s*\)",
        "NVS persistence")
require(SOURCE,
        r"device_settings_set_screen_timeout_s.*?screen_timeout_is_valid\(seconds\)"
        r".*?s_settings\.screen_timeout_s\s*=\s*seconds"
        r".*?bsp_display_restart_idle_timeout\(\)"
        r".*?bsp_display_apply_backlight_policy\(false\)",
        "setter validation, fresh idle window, and immediate policy application")
require(SOURCE,
        r'cJSON_AddNumberToObject\(root,\s*"screen_timeout_s",\s*'
        r'settings\.screen_timeout_s\)',
        "JSON GET uses a coherent settings snapshot")
require(SOURCE,
        r'device_settings_t\s+cfg\s*=\s*s_settings.*?'
        r'cJSON_GetObjectItem\(root,\s*'
        r'"screen_timeout_s"\).*?screen_timeout_is_valid\(val\).*?'
        r'cfg\.screen_timeout_s\s*=\s*\(uint16_t\)val',
        "validated partial JSON POST")
require(HEADER, r"device_settings_save_current\s*\(void\)",
        "interactive settings can persist the current state")
require(HEADER,
        r"device_settings_snapshot\s*\(\s*device_settings_t\s*\*out\s*\)",
        "readers can obtain a coherent settings snapshot")
require(SOURCE,
        r"copy_current_settings.*?\*revision\s*=\s*s_settings_revision",
        "settings snapshots carry their revision")
require(SOURCE,
        r"persist_current_locked.*?copy_current_settings\(&s_save_work,\s*&revision\).*?"
        r"nvs_writer_run\(do_device_settings_save,\s*&s_save_work\).*?"
        r"revision\s*==\s*s_settings_revision.*?if\s*\(stable\)\s*return ESP_OK",
        "a concurrent change causes the durable snapshot to be retried")
require(TOUCH_BSP,
        r"save_settings_task.*?device_settings_save_current\(\)",
        "touch debounce worker never writes a stale structure copy")

require(PORTAL,
        r'id="screen-timeout".*?onchange="onScreenTimeoutChange\(this\)"',
        "web timeout select")
timeout_select = re.search(
    r'<select id="screen-timeout".*?</select>', PORTAL,
    re.MULTILINE | re.DOTALL)
if not timeout_select:
    raise AssertionError("screen-timeout web select is missing")
options = dict(re.findall(
    r'<option value="(0|60|300|900|1800)"(?: selected)?>([^<]+)</option>',
    timeout_select.group(0)))
assert options == {
    "0": "Never",
    "60": "1 minute",
    "300": "5 minutes",
    "900": "15 minutes",
    "1800": "30 minutes",
}, f"screen-timeout web choices changed: {options}"
require(PORTAL,
        r"function onScreenTimeoutChange.*?JSON\.stringify\(\{\s*"
        r"screen_timeout_s:\s*seconds\s*\}\)",
        "immediate web save")
require(PORTAL,
        r"loadDeviceSettings.*?d\.screen_timeout_s.*?timeout\.value",
        "web setting hydration")
require(PORTAL,
        r"function renderDeviceSettings\(d\).*?d\.screen_timeout_s.*?"
        r"function loadDeviceSettings\(\).*?renderDeviceSettings\(d\)",
        "single device-settings renderer supports direct and aggregate hydration")
require(PORTAL,
        r"fetch\('/api/settings/all'\).*?"
        r"if \(d\.device\) renderDeviceSettings\(d\.device\)",
        "aggregate settings load hydrates the display controls without fallback")
require(PORTAL,
        r"Only while therapy is stopped\..*?first touch wakes",
        "web UI explains therapy and wake-only behavior")

require(DISPLAY_HEADER, r"void\s+bsp_display_restart_idle_timeout\s*\(void\)",
        "cross-board idle-window hook")
require(TOUCH_BSP,
        r"void\s+bsp_display_restart_idle_timeout\s*\(void\).*?"
        r"s_last_touch_activity_us\s*=\s*now_us",
        "touch BSP restarts the monotonic activity window")
require(COMPACT_BSP,
        r"void\s+bsp_display_restart_idle_timeout\s*\(void\)",
        "compact BSP keeps the shared settings API linkable")

runtime_options = re.search(
    r"s_screen_timeout_options\s*\[[^]]+\]\s*=\s*\{([^}]+)\}",
    TOUCH_BSP, re.MULTILINE | re.DOTALL)
if not runtime_options:
    raise AssertionError("touchscreen timeout option table is missing")
assert {int(value) for value in re.findall(r"\d+", runtime_options.group(1))} == allowed, (
    "touchscreen choices differ from persisted values")
require(TOUCH_BSP,
        r'"Never\\n1 minute\\n5 minutes\\n15 minutes\\n30 minutes"',
        "touchscreen exposes every persisted timeout")

display_section = TOUCH_BSP.split(
    "static void build_display_section", 1)[1].split(
        "static void build_alerts_section", 1)[0]
display_rows = [
    (int(y), int(height))
    for y, height in re.findall(
        r"make_manage_row\(scroll,\s*(\d+),\s*(\d+)\)", display_section)
]
assert len(display_rows) == 3, f"unexpected Display row geometry: {display_rows}"
assert max(y + height for y, height in display_rows) <= 340, (
    f"Display controls overflow the 340 px viewport: {display_rows}")
require(display_section,
        r"lv_obj_set_size\(s_settings_screen_timeout,\s*172,\s*56\)",
        "main timeout selector is at least 56 px tall")

font_height_match = re.search(r"\.line_height\s*=\s*(\d+)", BODY_FONT)
if not font_height_match:
    raise AssertionError("body font line height is unavailable")
font_height = int(font_height_match.group(1))
list_style = TOUCH_BSP.split(
    "static void screen_timeout_list_ready_cb", 1)[1].split("\n}\n", 1)[0]
line_spaces = [
    int(value) for value in re.findall(
        r"lv_obj_set_style_text_line_space\([^,]+,\s*(\d+)", list_style)
]
assert line_spaces and min(line_spaces) + font_height >= 44, (
    "timeout popup options must retain at least 44 px touch pitch")

wake_input = TOUCH_BSP.split(
    "static bool screen_wake_input_available", 1)[1].split(
        "static bool begin_ble_operation", 1)[0]
touch_reader = TOUCH_BSP.split(
    "static void touch_read_cb", 1)[1].split(
        "static void tick_cb", 1)[0]
update_ui = TOUCH_BSP.split(
    "static void update_ui", 1)[1].split(
        "static void ui_task", 1)[0]
backlight_apply = TOUCH_BSP.split(
    "static void apply_pending_backlight_locked", 2)[2].split(
        "uint8_t bsp_display_get_brightness", 1)[0]

require(TOUCH_BSP, r"#define\s+TOUCH_FAILURE_THRESHOLD\s+3",
        "touch health changes only after a consecutive failure threshold")
require(wake_input,
        r"CONFIG_SOMNOTRACE_BOARD_QEMU.*?return true;.*?"
        r"if\s*\(\s*!s_touch\s*\)\s*return false;.*?"
        r"portENTER_CRITICAL\(&s_state_lock\).*?"
        r"s_touch_consecutive_errors\s*<\s*TOUCH_FAILURE_THRESHOLD.*?"
        r"portEXIT_CRITICAL\(&s_state_lock\).*?return healthy;",
        "wake input requires a present and currently healthy GT911")
require(touch_reader,
        r"bool\s+read_ok\s*=\s*read_result\s*==\s*ESP_OK\s*&&\s*"
        r"point_result\s*==\s*ESP_OK.*?if\s*\(read_ok\)\s*\{\s*"
        r"s_touch_consecutive_errors\s*=\s*0\s*;",
        "one successful controller read recovers touch health")
require(touch_reader,
        r"else if\s*\(touch\).*?s_touch_read_errors\+\+.*?"
        r"s_touch_consecutive_errors\s*<\s*UINT8_MAX.*?"
        r"s_touch_consecutive_errors\+\+.*?"
        r"previous\s*<\s*TOUCH_FAILURE_THRESHOLD\s*&&\s*"
        r"s_touch_consecutive_errors\s*>=\s*TOUCH_FAILURE_THRESHOLD",
        "failed reads build a saturated consecutive-failure streak")
require(touch_reader,
        r"touch_became_unavailable.*?"
        r'bsp_display_set_notice\("Touch unavailable - screen kept on"\)',
        "the transition to unhealthy touch is surfaced once")
require(TOUCH_BSP,
        r"request_idle_sleep_if_due.*?!screen_wake_input_available\(\).*?"
        r"portENTER_CRITICAL\(&s_state_lock\).*?!s_backlight_force_on.*?"
        r"s_backlight_requested\s*=\s*false",
        "idle-off eligibility and request are atomic and fail open without touch")
require(TOUCH_BSP,
        r"bsp_display_apply_backlight_policy.*?!screen_wake_input_available\(\)"
        r".*?bsp_display_set_backlight\(true\)",
        "therapy display policy also fails open without touch")
require(update_ui,
        r"if\s*\(\s*!screen_wake_input_available\(\)\s*&&\s*!backlight\s*\)"
        r".*?bsp_display_set_backlight\(true\).*?"
        r"apply_pending_backlight_locked\(\).*?"
        r"portENTER_CRITICAL\(&s_state_lock\).*?"
        r"backlight\s*=\s*s_backlight",
        "runtime touch failure actively wakes a dark touch-only device")
require(display_section,
        r"!can_wake_screen.*?LV_STATE_DISABLED.*?LV_STATE_DISABLED",
        "timeout and Off now controls disable when touch is absent")

require(TOUCH_BSP, r"#define\s+POLICY_PEEK_TIMEOUT_S\s+60",
        "therapy-off policies get a bounded control-window wake")
require(TOUCH_BSP,
        r"policy_prefers_off.*?LCD_THERAPY_ALWAYS_OFF.*?LCD_THERAPY_OFF.*?"
        r"POLICY_PEEK_TIMEOUT_S",
        "Screen off and Always off re-sleep after their wake window")
require(TOUCH_BSP,
        r"visual_alarm.*?bsp_display_set_backlight\(true\).*?"
        r"device_settings_snapshot\(&display_settings\).*?"
        r"request_idle_sleep_if_due\(&display_settings,\s*visual_alarm,\s*now_us\)",
        "alerts wake before idle sleep is evaluated")

require(TOUCH_BSP,
        r"s_wake_overlay\s*=\s*lv_obj_create\(lv_layer_top\(\)\).*?"
        r"lv_obj_set_style_bg_opa\(s_wake_overlay,\s*LV_OPA_TRANSP.*?"
        r"LV_OBJ_FLAG_CLICKABLE.*?LV_OBJ_FLAG_PRESS_LOCK.*?"
        r"wake_overlay_cb,\s*LV_EVENT_PRESSED",
        "transparent wake surface stays above dialogs and intercepts presses")
require(TOUCH_BSP,
        r"wake_overlay_cb.*?lv_indev_wait_release\(indev\).*?"
        r"bsp_display_restart_idle_timeout\(\).*?"
        r"bsp_display_set_backlight\(true\)",
        "first dark-screen press is consumed before waking")
require(TOUCH_BSP,
        r"lv_obj_move_foreground\(s_wake_overlay\)",
        "wake surface is raised whenever the display goes dark")

require(TOUCH_BSP, r"#define\s+BACKLIGHT_RETRY_US\s+250000",
        "failed wake writes use a bounded retry interval")
require(backlight_apply,
        r"if\s*\(now_us\s*<\s*retry_after_us\)\s*return;.*?"
        r"esp_err_t\s+backlight_result\s*=\s*"
        r"waveshare_7b_set_backlight\(requested\).*?"
        r"if\s*\(backlight_result\s*!=\s*ESP_OK\).*?"
        r"s_backlight_write_errors\+\+",
        "backlight writes are checked, rate-limited, and diagnosed")
require(backlight_apply,
        r"if\s*\(requested\)\s*\{\s*"
        r"s_backlight_retry_after_us\s*=\s*now_us\s*\+\s*"
        r"BACKLIGHT_RETRY_US.*?else\s*\{.*?"
        r"s_backlight_requested\s*=\s*true.*?"
        r"s_last_touch_activity_us\s*=\s*now_us.*?"
        r'Could not turn screen off - kept on',
        "failed wakes retry while failed optional sleeps fail open")
require(backlight_apply,
        r"esp_err_t\s+brightness_result\s*=\s*ESP_OK.*?"
        r"waveshare_7b_set_brightness.*?"
        r"if\s*\(brightness_result\s*!=\s*ESP_OK\).*?"
        r"s_backlight_write_errors\+\+",
        "wake-time brightness restoration is checked")
require(backlight_apply,
        r"if\s*\(backlight_result\s*!=\s*ESP_OK\).*?return;\s*\}.*?"
        r"portENTER_CRITICAL\(&s_state_lock\).*?"
        r"s_backlight\s*=\s*requested\s*;.*?"
        r"s_backlight_retry_after_us\s*=\s*0",
        "authoritative backlight state changes only after hardware success")
require(TOUCH_BSP,
        r"s_rendered_screen_timeout_s.*?!lv_dropdown_is_open.*?"
        r"settings\.screen_timeout_s.*?lv_dropdown_set_selected",
        "web timeout changes refresh the touch selector safely")

require(HOST_TEST, r"python3 scripts/screen_timeout_contract_test\.py",
        "screen-timeout contracts run in the host suite")

print("screen timeout contracts: PASS")
