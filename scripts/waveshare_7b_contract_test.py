#!/usr/bin/env python3
"""Static hardware-contract checks for the Waveshare 7B board profile.

The expected values come from Waveshare's ESP32-S3-Touch-LCD-7B ESP-IDF
reference at commit c652c902db607f7ffb376257393cfd7657aa6428. These checks do not
replace a physical bring-up, but they make accidental pin, timing, framebuffer,
or target-config regressions fail loudly during ordinary host testing.
"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def source(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(text: str, pattern: str, description: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(f"missing 7B contract: {description}")


board = source("main/board_waveshare_7b.c")
display = source("main/bsp_display_7b.c")
storage = source("main/sd_storage.c")
defaults = source("sdkconfig.7b.defaults")
cmake = source("main/CMakeLists.txt")
root_cmake = source("CMakeLists.txt")
lvgl_allocator = source("main/somnotrace_lvgl_psram.h")
fonts = source("main/somnotrace_fonts.h")
board_defaults = source("sdkconfig.7b.defaults")
history = source("main/touch_history.c")
history_header = source("main/touch_history.h")
edf = source("main/edf_gen.c")
main = source("main/main.c")
as11 = source("main/as11_ble.c")
device_settings = source("main/device_settings.c")
net_provision = source("main/net_provision.c")

expected_scalars = {
    r"#define\s+I2C_SDA\s+GPIO_NUM_8\b": "I2C SDA GPIO8",
    r"#define\s+I2C_SCL\s+GPIO_NUM_9\b": "I2C SCL GPIO9",
    r"#define\s+IOX_ADDR\s+0x24\b": "CH32V003 controller address 0x24",
    r"\.pclk_hz\s*=\s*18000000\b": "redraw-safe 18 MHz pixel clock",
    r"\.hsync_pulse_width\s*=\s*162\b": "HSYNC pulse",
    r"\.hsync_back_porch\s*=\s*152\b": "HSYNC back porch",
    r"\.hsync_front_porch\s*=\s*48\b": "HSYNC front porch",
    r"\.vsync_pulse_width\s*=\s*45\b": "VSYNC pulse",
    r"\.vsync_back_porch\s*=\s*13\b": "VSYNC back porch",
    r"\.vsync_front_porch\s*=\s*3\b": "VSYNC front porch",
    r"\.hsync_gpio_num\s*=\s*GPIO_NUM_46\b": "HSYNC GPIO46",
    r"\.vsync_gpio_num\s*=\s*GPIO_NUM_3\b": "VSYNC GPIO3",
    r"\.de_gpio_num\s*=\s*GPIO_NUM_5\b": "DE GPIO5",
    r"\.pclk_gpio_num\s*=\s*GPIO_NUM_7\b": "PCLK GPIO7",
    r"\.num_fbs\s*=\s*2\b": "double framebuffer",
    r"\.bounce_buffer_size_px\s*=\s*WAVESHARE_7B_H_RES\s*\*\s*10\b":
        "cache-sized ten-line bounce buffer",
    r"\.flags\.fb_in_psram\s*=\s*true\b": "PSRAM framebuffers",
    r"\.flags\.pclk_active_neg\s*=\s*true\b": "negative PCLK edge",
}
for pattern, description in expected_scalars.items():
    require(board, pattern, description)

rgb_match = re.search(r"\.data_gpio_nums\s*=\s*\{(.*?)\}", board, re.DOTALL)
if not rgb_match:
    raise AssertionError("missing 7B RGB data pin array")
rgb_pins = [int(value) for value in re.findall(r"GPIO_NUM_(\d+)", rgb_match.group(1))]
expected_rgb = [14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40]
assert rgb_pins == expected_rgb, f"RGB pin order changed: {rgb_pins}"
assert len(set(rgb_pins)) == 16, "RGB data pins must be unique"

rgb_bus = set(rgb_pins) | {3, 5, 7, 46}
control_bus = {4, 8, 9, 11, 12, 13}
assert not rgb_bus & control_bus, f"RGB/control GPIO collision: {rgb_bus & control_bus}"

require(board, r"\.x_max\s*=\s*WAVESHARE_7B_H_RES", "GT911 X range")
require(board, r"\.y_max\s*=\s*WAVESHARE_7B_V_RES", "GT911 Y range")
require(board, r"\.int_gpio_num\s*=\s*GPIO_NUM_4", "GT911 interrupt GPIO4")
require(board, r"IOX_TOUCH_RST,\s*false.*pdMS_TO_TICKS\(100\).*GPIO_NUM_4,\s*0.*pdMS_TO_TICKS\(100\).*IOX_TOUCH_RST,\s*true.*pdMS_TO_TICKS\(200\)",
        "Waveshare GT911 reset/address-selection timing")

require(storage, r"s\.clk\s*=\s*GPIO_NUM_12", "TF CLK GPIO12")
require(storage, r"s\.cmd\s*=\s*GPIO_NUM_11", "TF CMD GPIO11")
require(storage, r"s\.d0\s*=\s*GPIO_NUM_13", "TF D0 GPIO13")
require(storage, r"s\.width\s*=\s*1", "TF one-bit SDMMC mode")
require(board, r"iox_output\(IOX_SD_CS,\s*true\)", "TF DAT3/CS held high")
require(board,
        r"attenuation\s*=\s*\(uint8_t\)\(100U\s*-\s*percent\).*?"
        r"attenuation\s*>\s*97.*?IOX_REG_PWM,\s*pwm",
        "active-low backlight PWM mapping with vendor attenuation limit")
require(display,
        r"physical_brightness.*?tenth_percent\s*\+\s*1U\)\s*/\s*2U",
        "7B legacy brightness range mapped to 1-100 percent")
require(display, r'waveshare_7b_set_brightness\(100\)',
        "steady full-brightness display initialization")
require(display, r'"100% - steady"', "steady endpoint identified in touch UI")
require(device_settings,
        r"CONFIG_SOMNOTRACE_BOARD_WAVESHARE_7B\s*\|\|\s*"
        r"CONFIG_SOMNOTRACE_BOARD_QEMU.*?DEFAULT_BRIGHTNESS\s+200",
        "7-inch default is 100 percent steady backlight")
require(board,
        r"waveshare_7b_set_panel_pclk\s*\(uint32_t\s+hz\).*?"
        r"hz\s*!=\s*18000000U\s*&&\s*hz\s*!=\s*30850000U.*?"
        r"esp_lcd_rgb_panel_set_pclk\(s_panel,\s*hz\)",
        "runtime PCLK diagnostic restricted to the two A/B clocks")
require(net_provision,
        r'strcmp\(action,\s*"display-pclk"\)\s*==\s*0.*?'
        r'hz_item->valuedouble\s*!=\s*18000000\.0.*?'
        r'hz_item->valuedouble\s*!=\s*30850000\.0.*?'
        r'waveshare_7b_set_panel_pclk\(hz\)',
        "non-persistent display PCLK action with a strict two-clock allowlist")
assert '"/api/diagnostics/display-pclk"' not in net_provision, \
       "PCLK diagnostic must reuse /api/actions without consuming a URI slot"

require(display, r"\.on_frame_buf_complete\s*=\s*on_vsync", "frame-buffer handoff")
require(display, r"display_driver\.hor_res\s*=\s*WAVESHARE_7B_H_RES", "LVGL width")
require(display, r"display_driver\.ver_res\s*=\s*WAVESHARE_7B_V_RES", "LVGL height")
require(display, r"esp_lcd_rgb_panel_get_frame_buffer\(s_panel,\s*2,\s*&fb1,\s*&fb2\)",
        "two panel-owned framebuffers")
require(display, r"display_driver\.direct_mode\s*=\s*1", "dirty-region direct rendering")
require(display, r"if\s*\(!lv_disp_flush_is_last\(drv\)\).*?lv_disp_flush_ready\(drv\).*?return",
        "one panel handoff after the final dirty area")
require(display,
        r"ulTaskNotifyTake\(pdTRUE,\s*0\).*?esp_lcd_panel_draw_bitmap.*?"
        r"ulTaskNotifyTake\(pdTRUE,\s*pdMS_TO_TICKS\(100\)\)",
        "clear-before-submit frame-boundary wait")
require(root_cmake, r"LV_MEM_CUSTOM_ALLOC=somnotrace_lvgl_alloc",
        "7-inch LVGL allocator override")
require(lvgl_allocator, r"heap_caps_malloc_prefer.*?MALLOC_CAP_SPIRAM.*?MALLOC_CAP_INTERNAL",
        "LVGL PSRAM-first allocation with internal fallback")
require(lvgl_allocator, r"heap_caps_realloc_prefer", "matched LVGL reallocator")
require(lvgl_allocator, r"heap_caps_free", "matched LVGL free")
require(board_defaults, r"CONFIG_LV_USE_FONT_COMPRESSED=y",
        "compressed custom-font renderer")
require(display, r'#include\s+"somnotrace_fonts\.h"', "custom bedside fonts")
for family in ("space_grotesk", "ibm_plex_mono"):
    assert family in fonts, f"missing {family} font declarations"

for setting in (
    "CONFIG_SOMNOTRACE_BOARD_WAVESHARE_7B=y",
    "CONFIG_ESP32S3_DATA_CACHE_64KB=y",
    "CONFIG_ESP32S3_DATA_CACHE_SIZE=0x10000",
    "CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y",
    "CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y",
    "CONFIG_SPIRAM_RODATA=y",
    "CONFIG_LV_COLOR_DEPTH_16=y",
    "CONFIG_LV_INDEV_DEF_READ_PERIOD=10",
    "CONFIG_LV_DISP_DEF_REFR_PERIOD=16",
    "CONFIG_LV_SPRINTF_USE_FLOAT=y",
    "CONFIG_LV_USE_CHART=y",
    "CONFIG_LV_USE_MSGBOX=y",
):
    assert setting in defaults, f"missing 7B sdkconfig default: {setting}"

for unit in ("board_waveshare_7b.c", "bsp_display_7b.c", "bsp_power_7b.c", "bsp_audio_7b.c"):
    assert f'"{unit}"' in cmake, f"7B build omits {unit}"

# The 7-inch build follows the fixed three-screen bedside handoff.  Navigation
# is custom rather than an LVGL tabview so the centred 172x58 pills and the
# separate six-section Manage rail can be represented without tab chrome.
for pattern, description in (
    (r"#define\s+UI_HEADER_H\s+70\b", "70px shared header"),
    (r"#define\s+UI_CONTENT_Y\s+70\b", "content begins below the 70px header"),
    (r"#define\s+UI_CONTENT_H\s+448\b", "448px shared content region"),
    (r"#define\s+UI_NAV_H\s+82\b", "82px shared navigation region"),
    (r"s_pages\s*\[\s*3\s*\]", "three primary screen containers"),
    (r"s_nav_buttons\s*\[\s*3\s*\]", "three custom navigation buttons"),
    (r"set_active_page\s*\(", "custom page selection"),
):
    require(display, pattern, description)
assert "lv_tabview_create" not in display, "bedside shell must use custom navigation"
assert "lv_tabview_add_tab" not in display, "legacy five-tab navigation remains"
for page in ("Home", "History", "Manage"):
    require(display, rf'"{page}"', f"{page} primary navigation label")
for section_label in ("Devices", "Connectivity", "Display", "Alerts", "Storage", "System"):
    require(display, rf'"{section_label}"', f"{section_label} Manage rail label")

# The shared header status capsule sizes itself from the rendered status text.
# Keep every state on one line, vertically centre dots against that line, and
# preserve the fixed screen-right edge plus an explicit chevron inset.
for pattern, description in (
    (r"status_label_width.*?lv_txt_get_size\(&size,\s*lv_label_get_text\(label\),\s*"
     r"FONT_BODY,\s*0,\s*0,\s*LV_COORD_MAX,\s*LV_TEXT_FLAG_NONE\).*?"
     r"return\s+LV_MAX\(size\.x,\s*1\)",
     "content-measured status labels"),
    (r"layout_status_capsule.*?font_h\s*=\s*lv_font_get_line_height\(FONT_BODY\).*?"
     r"label_y\s*=\s*\(STATUS_CAPSULE_H\s*-\s*font_h\)\s*/\s*2.*?"
     r"dot_y\s*=\s*label_y\s*\+\s*"
     r"\(font_h\s*-\s*STATUS_CAPSULE_DOT_SIZE\)\s*/\s*2",
     "vertically centred status dots and labels"),
    (r"layout_status_capsule.*?text_w\s*=\s*status_label_width\(labels\[i\]\).*?"
     r"lv_label_set_long_mode\(labels\[i\],\s*"
     r"LV_LABEL_LONG_CLIP\).*?lv_obj_set_size\(labels\[i\],\s*text_w,\s*font_h\)",
     "single-line non-wrapping status labels"),
    (r"#define\s+STATUS_CAPSULE_RIGHT\s+1006\b.*?"
     r"#define\s+STATUS_CAPSULE_RIGHT_PAD\s+18\b.*?"
     r"lv_obj_set_pos\(s_status_chevron,\s*cursor,.*?"
     r"cursor\s*\+=\s*14\s*\+\s*STATUS_CAPSULE_RIGHT_PAD.*?"
     r"lv_obj_set_pos\(s_status_capsule,\s*STATUS_CAPSULE_RIGHT\s*-\s*cursor,\s*7\).*?"
     r"lv_obj_set_size\(s_status_capsule,\s*cursor,\s*STATUS_CAPSULE_H\)",
     "right-anchored status capsule with chevron padding"),
):
    require(display, pattern, description)

require(display,
        r"static\s+bool\s+set_label_text_if_changed.*?"
        r"strcmp\(current,\s*text\)\s*==\s*0\)\s*return\s+false;.*?"
        r"lv_label_set_text\(label,\s*text\);\s*return\s+true;",
        "status relayout change signal")
require(display,
        r"bool\s+status_capsule_layout_dirty\s*=\s*false;.*?"
        r"status_capsule_layout_dirty\s*\|=.*?s_sd_label.*?"
        r"status_capsule_layout_dirty\s*\|=.*?s_wifi_label.*?"
        r"status_capsule_layout_dirty\s*\|=.*?s_ble_label.*?"
        r"if\s*\(status_capsule_layout_dirty\)\s*layout_status_capsule\(\);",
        "status capsule relayout only after visible text changes")

# Pure destination navigation responds at touch-down; operational controls keep
# the generic completed-click behavior so a drag/cancel cannot trigger them.
require(display,
        r"make_destination_button.*?lv_obj_remove_event_cb_with_user_data.*?"
        r"lv_obj_add_event_cb\(button,\s*callback,\s*LV_EVENT_PRESSED",
        "pressed-event destination button factory")
require(display,
        r"lv_obj_add_event_cb\(button,\s*callback,\s*LV_EVENT_CLICKED",
        "completed-click generic action button factory")
require(display, r"s_nav_buttons\[i\]\s*=\s*make_destination_button",
        "immediate bottom navigation")
require(display, r"s_manage_buttons\[i\]\s*=\s*make_destination_button",
        "immediate Manage section rail")
require(display, r"s_status_capsule\s*=\s*make_destination_button",
        "immediate non-destructive status tray")
require(display,
        r"static void set_destination_surface.*?"
        r"LV_STYLE_TRANSLATE_Y,\s*0,\s*LV_STATE_PRESSED.*?"
        r"LV_STYLE_BG_OPA,\s*resting_opa,\s*LV_STATE_PRESSED",
        "destination controls do not replay generic press feedback on release")
require(display,
        r"lv_obj_add_event_cb\(s_status_scrim,\s*status_tray_close_cb,\s*"
        r"LV_EVENT_PRESSED",
        "immediate non-destructive status tray dismissal")
for control, description in (
    ("s_therapy_button", "therapy command"),
    ("s_alert_ack_button", "alert acknowledgement"),
    ("s_reboot_button", "restart command"),
):
    require(display, rf"{control}\s*=\s*make_touch_button",
            f"{description} remains completed-click")
require(display, r"static\s+int\s+s_active_page\s*=\s*-1",
        "unselected page sentinel for first render")
require(display, r"static\s+int\s+s_active_manage_section\s*=\s*-1",
        "unselected Manage-section sentinel for first render")
require(display,
        r"if\s*\(section\s*==\s*s_active_manage_section\)\s*return",
        "current Manage-section navigation no-op")
active_page_start = display.index("static void set_active_page(int page)\n{")
active_page_source = display[
    active_page_start:display.index("static void nav_cb", active_page_start)
]
manage_section_start = display.index("static void set_manage_section(int section)\n{")
manage_section_source = display[
    manage_section_start:
    display.index("static void manage_section_cb", manage_section_start)
]
require(active_page_source,
        r"set_destination_surface\(s_nav_buttons\[i\]",
        "bottom navigation uses stable destination surfaces")
require(active_page_source,
        r"portENTER_CRITICAL\(&s_state_lock\);\s*"
        r"bool\s+already_active\s*=\s*page\s*==\s*s_active_page;\s*"
        r"if\s*\(!already_active\)\s*s_active_page\s*=\s*page;\s*"
        r"portEXIT_CRITICAL\(&s_state_lock\);\s*"
        r"if\s*\(already_active\)\s*return;",
        "page navigation publishes and checks the active page under lock")
require(active_page_source, r"if\s*\(page\s*==\s*1\)\s*start_history_load\(\);",
        "History navigation requests a metadata refresh")
for selection_source, description in (
    (active_page_source, "page navigation"),
    (manage_section_source, "Manage section navigation"),
):
    assert "lv_obj_set_style_" not in selection_source, \
           f"{description} bypasses changed-only style helpers"
    assert "lv_obj_add_flag(" not in selection_source and \
           "lv_obj_clear_flag(" not in selection_source, \
           f"{description} bypasses changed-only visibility helper"
require(display,
        r"s_storage_refresh_button\s*=\s*make_touch_button.*?storage_refresh_cb",
        "storage refresh remains a distinct completed-click action")
require(display,
        r'xTaskCreatePinnedToCore\(lvgl_task,\s*"display_7b",\s*12288,\s*'
        r"NULL,\s*5,\s*&s_lvgl_task,\s*1\)",
        "responsive priority-5 display task with measured stack headroom")

# QEMU keeps the full-fidelity handoff, while the physical build compiles out
# large software-blurred shadows and expensive flow fill/glow layers.
require(display,
        r"#if\s+CONFIG_SOMNOTRACE_BOARD_QEMU.*?"
        r"UI_STATUS_SCRIM_OPA\s+LV_OPA_60.*?"
        r"#else.*?UI_STATUS_SCRIM_OPA\s+LV_OPA_COVER",
        "opaque physical status scrim avoids full-screen alpha blending")
status_open_start = display.index("static void status_tray_open_cb(lv_event_t *event)\n{")
status_open_source = display[
    status_open_start:display.index("static void status_tray_route_cb", status_open_start)
]
assert "lv_obj_move_foreground" not in status_open_source, \
       "opening status tray must not invalidate the screen through reordering"
require(display,
        r"lv_obj_move_foreground\(s_status_scrim\);\s*"
        r"lv_obj_move_foreground\(s_status_tray\);.*?"
        r"set_manage_section\(0\);",
        "status overlay z-order established before first frame")
require(display,
        r"active_tab\s*==\s*0\s*&&\s*!status_tray_open.*?"
        r"status_tray_just_closed",
        "live chart redraw pauses behind status tray and resyncs on close")
require(display,
        r"#if\s+CONFIG_SOMNOTRACE_BOARD_QEMU.*?"
        r"UI_DECORATIVE_SHADOW_WIDTH\(pixels\)\s+\(pixels\).*?"
        r"FLOW_RENDER_POINTS\s+FLOW_POINTS.*?#else.*?"
        r"UI_DECORATIVE_SHADOW_WIDTH\(pixels\).*?,\s*0\).*?"
        r"UI_DECORATIVE_SHADOW_OPA\(opacity\).*?LV_OPA_TRANSP\).*?"
        r"FLOW_RENDER_POINTS\s+150.*?FLOW_RENDER_FILL\s+0.*?"
        r"FLOW_RENDER_GLOW\s+0",
        "physical low-cost rendering profile")
for target, description in (
    ("card", "generic cards"),
    ("s_ambient_glow", "ambient glow"),
    ("s_alert_banner", "alert banner"),
    ("s_keyboard_sheet", "keyboard sheet"),
):
    require(display,
            rf"lv_obj_set_style_shadow_width\(\s*{target}.*?"
            r"UI_DECORATIVE_SHADOW_WIDTH",
            f"physical shadow suppression for {description}")
require(display,
        r"set_style_num_if_changed\(\s*s_history_rows\[i\],\s*"
        r"LV_STYLE_SHADOW_WIDTH,\s*UI_DECORATIVE_SHADOW_WIDTH",
        "changed-only physical shadow suppression for selected History row")
for target, description in (
    ("s_nav_buttons\\[i\\]", "selected navigation"),
    ("s_manage_buttons\\[i\\]", "selected Manage rail"),
    ("s_therapy_hero", "therapy hero"),
    ("s_therapy_orb", "therapy orb"),
    ("s_therapy_button", "therapy action"),
):
    require(display,
            rf"{target},\s*LV_STYLE_SHADOW_WIDTH,.*?"
            r"UI_DECORATIVE_SHADOW_WIDTH",
            f"physical dynamic shadow suppression for {description}")
require(display, r"lv_point_t\s+points\[FLOW_RENDER_POINTS\]",
        "bounded physical flow point array")
require(display,
        r"source_index\s*=.*?FLOW_POINTS\s*-\s*1.*?"
        r"FLOW_RENDER_POINTS\s*-\s*1",
        "endpoint-preserving physical flow downsampling")

# Concrete state/component coverage from the bedside handoff. Keeping this in
# the hardware contract ensures the native build cannot silently regress to a
# three-tab mock with none of the real loading, empty, fault, or pairing states.
for literal, description in (
    ("Therapy active", "active therapy state"),
    ("Therapy stopped", "stopped therapy state"),
    ("Starting...", "therapy start busy state"),
    ("Stopping...", "therapy stop busy state"),
    ("Pair a device", "unpaired primary state"),
    ("Therapy stopped unexpectedly", "interruption alert state"),
    ("Acknowledge", "interruption acknowledgement control"),
    ("History not loaded", "History initial state"),
    ("Reading history...", "History busy state"),
    ("Load 7 more", "bounded History pagination"),
    ("No completed sessions yet", "History empty state"),
    ("Could not read the card", "History error state"),
    ("microSD is busy", "History card-busy state"),
    ("Select a night", "History unselected state"),
    ("Retry", "History error recovery action"),
    ("For trend review. Not a diagnosis or a prescription.", "History safety copy"),
    ("Searching for nearby machines", "AirSense scanning state"),
    ("Connecting securely", "AirSense connecting state"),
    ("Enter the 4-digit code shown on your AirSense", "AirSense passcode state"),
    ("Confirming the code", "AirSense confirmation state"),
    ("Pairing failed · enable pairing mode first", "actionable AirSense pairing error state"),
    ("First on AirSense: More › MyAir App › OK, downloaded › Connect",
     "machine-first AirSense pairing prerequisite"),
    ("AirSense is ready", "explicit AirSense pairing-mode acknowledgement"),
    ("Safe to save now · restart will be deferred", "Wi-Fi deferred-restart state"),
    ("Wi-Fi saved; restart deferred while recording", "saved Wi-Fi deferred notice"),
    ("Send test push", "alert test control"),
    ("microSD capacity and upload queue", "storage summary"),
    ("Advanced diagnostics", "system disclosure row"),
):
    assert literal in display, f"missing bedside state: {description}"
require(display, r'"Waiting for (?:therapy|breathing) data(?:\.\.\.|…)?"',
        "first-sample loading state")
require(display, r"flow_count\s*>=\s*FLOW_READY_POINTS",
        "valid sample threshold before chart becomes live")
require(display, r'"(?:No recent flow sample|Live data delayed)"', "stale flow state")
for metric in ("AHI", "PRESSURE 95%", "LEAK 95%", "EVENTS PER HOUR"):
    assert metric in display, f"missing History component: {metric}"

# Home has one contextual therapy action.  Administration and acknowledgement
# live in Manage or transient banners, never in a permanent Home utility row.
home_match = re.search(
    r"static\s+void\s+build_home_page\s*\([^)]*\)\s*\{(.*?)\n\}",
    display,
    re.MULTILINE | re.DOTALL,
)
assert home_match, "missing build_home_page() for static Home acceptance checks"
home_source = home_match.group(1)
for forbidden in ('"Wi-Fi setup"', '"Acknowledge"', '"Screen off"'):
    assert forbidden not in home_source, f"permanent Home action remains: {forbidden}"
require(display, r"xTaskCreate\(device_scan_task", "non-blocking BLE scan worker")
require(display, r"as11_ble_start_pair\(job->addr\)", "native AirSense pairing action")
require(display, r"as11_ble_confirm_pair\(job->passkey\)", "native AirSense passkey action")
require(display,
        r"action\s*==\s*DEVICE_PAIR_AS11\s*&&\s*!pairing_mode_confirmed",
        "AirSense connect is gated on machine pairing-mode acknowledgement")
require(display, r"oximeter_pair\(job->addr,\s*OX_DRIVER_AUTO\)",
        "auto-detected native O2 pairing action")
require(display, r"device_settings_set_brightness", "native brightness control")
require(display, r"device_settings_set_lcd_therapy_mode", "native therapy display policy")
require(display, r"netprov_save_config\(&cfg\)", "native Wi-Fi credential save")
require(display, r"LV_KEYBOARD_MODE_TEXT_LOWER", "on-screen Wi-Fi keyboard")
require(display, r"touch_history_load", "background SD history load")
require(history, r"edf_gen_summary_json", "history summary metrics adapter")
assert '"touch_history.c"' in cmake, "7B build omits touch history adapter"
require(main, r"oximeter_init\(\).*?bsp_display_enable_touch_services\(as11_ready,\s*oximeter_ready\)",
        "touch BLE controls enabled only after service initialization")

# Review-derived safety contracts that can be checked without physical hardware.
require(as11, r"as11_ble_get_values.*?xSemaphoreTake\(s_cmd_mtx.*?clear_response\(\)",
        "serialized AS11 Get RPC")
require(as11, r"therapy_command.*?xSemaphoreTake\(s_cmd_mtx.*?clear_response\(\)",
        "serialized therapy RPC")
require(display, r"s_therapy_command_busy", "single-flight therapy control")
assert '"Therapy command sent"' not in display, \
       "successful start/stop must not raise a redundant confirmation notice"
for toast in ('"Starting therapy..."', '"Stopping therapy..."'):
    assert toast not in display, \
           "therapy progress belongs in the hero card, not a toast"
require(display,
        r"if\s*\(result\s*!=\s*ESP_OK\)\s*"
        r"bsp_display_set_notice\(\"Therapy command failed\"\)",
        "therapy command failures remain visible")
require(display, r"s_wake_overlay.*?LV_EVENT_PRESSED", "wake-only touch interception")
require(display, r"s_backlight_requested", "sticky backlight desired state")
require(display, r"sd_storage_lease_acquire\(SD_LEASE_UPLOAD,\s*250\)",
        "leased UI free-space query")
require(display, r"flow_sample_us\s*=\s*esp_timer_get_time\(\)",
        "flow freshness uses source arrival time")
require(display, r"sd_storage_recording_active\(\)",
        "Home recording copy follows writer state")
require(display, r"sd_storage_deinit\(\);\s*esp_restart\(\)",
        "clean SD unmount before UI restart")

# The physical RGB panel is sensitive to large redundant redraws: unchanged
# Home presentation state must not invalidate labels, visibility, or styles on
# every 500 ms pass. Keep these checks close to the hardware contracts because
# this is also a panel-stability requirement, not just a rendering optimization.
require(display,
        r"set_label_text_if_changed.*?lv_label_get_text.*?strcmp.*?"
        r"lv_label_set_text",
        "state-aware label updates")
require(display,
        r"set_hidden.*?lv_obj_has_flag\(obj,\s*LV_OBJ_FLAG_HIDDEN\)\s*==\s*hidden",
        "state-aware visibility updates")
require(display, r"lv_obj_get_local_style_prop",
        "exact local-style comparison before Home style mutation")
update_start = display.index("static void update_ui(void)")
update_end = display.index("static void lvgl_task(void", update_start)
update_source = display[update_start:update_end]
assert "lv_label_set_text(" not in update_source, \
       "update_ui must use state-aware label setters"
assert "lv_label_set_text_fmt(" not in update_source, \
       "update_ui must use state-aware formatted label setters"
assert "lv_obj_set_style_" not in update_source, \
       "update_ui must compare Home style values before mutation"
assert "lv_obj_add_flag(" not in update_source and \
       "lv_obj_clear_flag(" not in update_source, \
       "update_ui must compare visibility before mutation"
require(update_source,
        r"lv_bar_get_value\(s_metric_bars\[i\]\)\s*!=\s*bar_values\[i\]",
        "state-aware Home metric bars")
require(update_source,
        r"alert_visibility_changed\s*=\s*set_hidden\(s_alert_banner",
        "state-aware alert banner visibility")
require(history, r"sd_storage_lease_acquire\(SD_LEASE_UPLOAD", "leased history reads")
require(history_header, r"TOUCH_HISTORY_TRACE_POINTS\s+48\b",
        "bounded native overnight trace")
require(history_header, r"int16_t\s+points\[TOUCH_HISTORY_TRACE_POINTS\]",
        "compact selected-channel trace storage")
require(history_header, r"int16_t\s+upper_points\[TOUCH_HISTORY_TRACE_POINTS\]",
        "bounded parallel flow-envelope storage")
require(history_header, r"bool\s+has_data\s*;",
        "truthful trace availability")
require(history, r"terminal_session.*?completed.*?interrupted.*?timed_out.*?rotated.*?split",
        "trace excludes active and failed sessions")
require(history, r"_flow_mm\.snt.*?_brp_mm\.snt.*?_pld\.snt",
        "v2 and legacy bounded overview sources")
require(history, r"touch_history_load_trace", "selection-driven trace API")
require(history, r"HISTORY_TRACE_MAX_RECORDS\s+\(24U\s*\*\s*60U\s*\*\s*60U\)",
        "one-day hard bound on trace input")
require(history, r"HISTORY_READ_VALUES\s+512U", "bounded block-read buffer")
require(history, r"fread\(records,.*?wanted.*?for\s*\(size_t r = 0; r < got;.*?processed \+= \(uint32_t\)got",
        "only complete buffered records populate trace bins")
require(history, r"record\s*=\s*&records\[r\s*\*\s*best\.n_channels\]",
        "channel-width-aware v2 record stride")
require(history, r"fmt >= 2.*?channels = 2;.*?channels = 4;",
        "v2 flow pair and legacy v1 BRP channel semantics")
assert "touch_history_load_trace(days[i]" not in history, \
       "trace must not be read eagerly for every history day"
require(display, r"queue_history_trace_load\(selected_day,\s*s_history_channel\)",
        "trace loads only after a night is selected")

history_trace_task_start = display.index("static void history_trace_task(void *arg)\n{")
history_trace_task_source = display[
    history_trace_task_start:
    display.index("static void queue_history_trace_load", history_trace_task_start)
]
history_queue_start = display.index("static void queue_history_trace_load(const char *day,")
history_queue_source = display[
    history_queue_start:display.index("static void history_task", history_queue_start)
]
history_task_start = display.index("static void history_task(void *arg)\n{")
history_task_source = display[
    history_task_start:display.index("#endif", history_task_start)
]
history_load_start = display.index("static void start_history_load(void)\n{")
history_load_source = display[
    history_load_start:display.index("static void qemu_upload_progress", history_load_start)
]
history_row_start = display.index("static void history_row_cb(lv_event_t *event)\n{")
history_row_source = display[
    history_row_start:display.index("static void refresh_cb", history_row_start)
]
history_refresh_start = display.index(
    "static void refresh_history_widgets(const ui_service_state_t *services)\n{"
)
history_refresh_source = display[
    history_refresh_start:
    display.index("static void refresh_device_dropdown", history_refresh_start)
]
therapy_state_start = display.index(
    "void bsp_display_set_therapy_active(bool active)\n{"
)
therapy_state_source = display[
    therapy_state_start:display.index("void bsp_display_push_flow", therapy_state_start)
]
for worker_source, description in (
    (history_task_source, "History metadata worker"),
    (history_trace_task_source, "History trace worker"),
):
    require(worker_source, r"for\s*\(;;\).*?ulTaskNotifyTake\(pdTRUE,\s*portMAX_DELAY\)",
            f"persistent notification-driven {description}")
require(history_load_source, r"xTaskNotifyGive\(s_history_worker_task\)",
        "History refresh notification")
require(history_queue_source, r"xTaskNotifyGive\(s_history_trace_worker_task\)",
        "History trace notification")
require(history_task_source,
        r"if\s*\(result\s*==\s*ESP_OK\).*?"
        r"s_services\.history_count\s*=\s*count;.*?"
        r"else\s*\{.*?s_services\.history_count\s*=\s*0;.*?\}.*?"
        r"s_services\.history_result\s*=\s*result;",
        "failed History metadata refresh clears stale rows")
require(history_task_source,
        r"s_services\.history_busy\s*=\s*rerun;\s*"
        r"s_services\.history_version\+\+;\s*"
        r"s_services\.history_metadata_version\+\+;",
        "metadata completion publishes its own version")
require(display,
        r"static\s+unsigned\s+s_history_refresh_generation\s*=\s*1;.*?"
        r"static\s+unsigned\s+s_history_refresh_started_generation;.*?"
        r"static\s+unsigned\s+s_history_refresh_completed_generation;",
        "boot starts with a stale History metadata generation")
require(history_load_source,
        r"refresh_required\s*=\s*s_history_refresh_generation\s*!=\s*"
        r"s_history_refresh_completed_generation;.*?"
        r"refresh_required\s*&&\s*s_history_worker_task.*?"
        r"s_history_refresh_started_generation\s*=\s*"
        r"s_history_refresh_generation;",
        "History page entry only starts a stale metadata generation")
require(history_load_source,
        r"static void request_history_refresh\(void\).*?"
        r"s_history_refresh_generation\+\+;.*?start_history_load\(\);",
        "manual History refresh forces a new metadata generation")
require(history_task_source,
        r"s_history_refresh_completed_generation\s*=\s*load_generation;.*?"
        r"bool\s+rerun\s*=\s*s_history_refresh_generation\s*!=\s*"
        r"load_generation.*?xTaskNotifyGive\(s_history_worker_task\);",
        "metadata worker preserves a stale generation requested in flight")
require(history_refresh_source,
        r"metadata_changed\s*=\s*services->history_metadata_version\s*!=\s*"
        r"s_seen_history_metadata_version;.*?"
        r"if\s*\(version_changed\)\s*\{.*?"
        r"s_seen_history_metadata_version\s*=\s*"
        r"services->history_metadata_version;.*?"
        r"if\s*\(!metadata_changed\s*&&\s*s_history_selected_day\[0\]\)\s*\{.*?"
        r"strcmp\(s_history_selected_day,\s*services->history\[i\]\.day\).*?"
        r"s_history_selection\s*=\s*\(int\)i;",
        "trace-only History updates preserve the selected day")
require(history_refresh_source,
        r"if\s*\(services->history_count\s*==\s*0\).*?"
        r"else\s+if\s*\(metadata_changed\s*\|\|\s*"
        r"s_history_selection\s*<\s*0\)\s*\{.*?"
        r"s_history_selection\s*=\s*0;.*?"
        r"services->history\[0\]\.day.*?"
        r"if\s*\(metadata_changed\s*&&\s*s_history_selection\s*>=\s*0\)\s*"
        r"queue_history_trace_load\(s_history_selected_day,\s*s_history_channel\)",
        "completed non-empty metadata refresh selects and queues row zero")
require(therapy_state_source,
        r"portENTER_CRITICAL\(&s_state_lock\);\s*"
        r"bool\s+changed\s*=\s*s_state\.therapy\s*!=\s*active;.*?"
        r"s_state\.therapy\s*=\s*active;.*?"
        r"therapy_finished\s*=\s*changed\s*&&\s*!active;.*?"
        r"if\s*\(therapy_finished\)\s*s_history_refresh_generation\+\+;.*?"
        r"bool\s+refresh_history\s*=\s*therapy_finished\s*&&\s*"
        r"s_active_page\s*==\s*1;\s*"
        r"portEXIT_CRITICAL\(&s_state_lock\);.*?"
        r"if\s*\(refresh_history\)\s*start_history_load\(\);",
        "therapy stop snapshots History refresh eligibility under lock")
require(history_row_source,
        r"selection\s*<\s*\(int\)s_render_services->history_count.*?"
        r"s_render_services->history\[selection\]\.day.*?"
        r"queue_history_trace_load\(selected_day,\s*s_history_channel\)",
        "History row taps resolve against the painted service snapshot")
assert not re.search(r"\bs_services(?:\.|->)", history_row_source), \
       "History row taps must not resolve against mutable live services"
require(history_trace_task_source,
        r"touch_history_load_trace\(.*?requested_day,\s*requested_channel,\s*&loaded\);.*?"
        r"if\s*\(request_generation\s*==\s*s_history_trace_request_generation\)\s*\{.*?"
        r"s_services\.history_trace\s*=\s*loaded;.*?"
        r"s_services\.history_trace_result\s*=\s*result;.*?"
        r"s_services\.history_trace_busy\s*=\s*false;.*?\}\s*"
        r"s_services\.history_version\+\+;",
        "only the latest day/channel trace result publishes")
require(history_queue_source,
        r"s_history_trace_requested_day\[0\]\s*=\s*'\\0';\s*"
        r"s_services\.history_trace_busy\s*=\s*false;\s*"
        r"s_services\.history_trace_result\s*=\s*ESP_ERR_NO_MEM;\s*"
        r"s_services\.history_version\+\+;",
        "unavailable trace worker publishes a terminal result")
require(history_refresh_source,
        r"trace_failed\s*=\s*trace_request_matches\s*&&\s*"
        r"!services->history_trace_busy\s*&&\s*"
        r"!trace->loaded\s*&&.*?"
        r'"No O₂ Ring data for this night".*?"O₂ Ring data unavailable - tap SpO₂ to retry"',
        "terminal trace errors expose tap-to-retry guidance")
for task, label in (
    ("history_task", "metadata"),
    ("history_trace_task", "trace"),
):
    assert len(re.findall(rf"psram_task_create\(\s*{task}\b", display)) == 1, \
           f"{label} worker must be created once with a PSRAM stack"
    assert not re.search(rf"xTaskCreate(?:PinnedToCore)?\(\s*{task}\b", display), \
           f"{label} worker must not be recreated per request"
require(display, r"heap_caps_calloc\(.*?HISTORY_MAX_DAYS.*?MALLOC_CAP_SPIRAM",
        "expanded history model stays off the worker stack")
require(display,
        r"lv_chart_set_ext_y_array\(s_history_trace_chart,\s*"
        r"s_history_trace_series,\s*s_history_trace_values\).*?"
        r"s_history_trace_values\[i\]\s*=\s*LV_CHART_POINT_NONE;",
        "external History chart array preserves missing-sample gaps")
require(display,
        r"lv_chart_set_point_count\(s_history_trace_chart,\s*"
        r"TOUCH_HISTORY_TRACE_POINTS\)",
        "History chart point count follows its fixed buffers")
assert "lv_chart_set_next_value" not in history_refresh_source, \
       "History renderer must not invalidate the chart once per point"
require(history_refresh_source,
        r"trace_count\s*=\s*trace->count\s*<\s*TOUCH_HISTORY_TRACE_POINTS.*?"
        r"s_history_trace_values\[i\]\s*=.*?"
        r"lv_chart_refresh\(s_history_trace_chart\);",
        "bounded History chart arrays publish with one final refresh")
assert history_refresh_source.count("lv_chart_refresh(s_history_trace_chart);") == 1, \
       "History renderer must refresh its direct arrays exactly once"
require(history_refresh_source,
        r"bool\s+list_changed\s*=.*?bool\s+summary_changed\s*=.*?"
        r"bool\s+trace_changed\s*=.*?if\s*\(list_changed\).*?"
        r"if\s*\(summary_changed\).*?if\s*\(!trace_changed\)\s*return;",
        "History metadata, summary, and trace render independently")
require(history_refresh_source,
        r"if\s*\(channel_changed\).*?"
        r"set_destination_surface\(s_history_channel_buttons\[i\]",
        "History channel selection has no release-phase animation")
require(history_refresh_source,
        r"if\s*\(busy_changed\s*\|\|\s*metadata_changed\s*\|\|\s*"
        r"revealed_changed\).*?Latest %u · showing %u",
        "History pagination immediately updates its showing count")
require(history_refresh_source,
        r"bool\s+selected\s*=\s*s_history_selection\s*>=\s*0\s*&&\s*"
        r"s_history_selection\s*<\s*\(int\)services->history_count;",
        "valid cached History detail stays visible during metadata refresh")
for direct_invalidation in (
    "lv_obj_clear_flag(s_history_detail_content",
    "lv_obj_clear_flag(s_history_rows[i]",
    "lv_obj_clear_flag(s_history_trace_message",
):
    assert direct_invalidation not in history_refresh_source, \
           f"History renderer bypasses changed-only visibility: {direct_invalidation}"
require(display, r"trace->loaded.*?trace->has_data.*?trace_count",
        "physical History renders only available recorded trace data")
require(history, r"has_ahi\s*=\s*json_number", "missing AHI remains unavailable")
require(history_header, r"int\s+mask_off_count\s*;",
        "nightly mask-off count value")
require(history_header, r"bool\s+has_mask_off_count\s*;",
        "nightly mask-off availability")
require(history, r'has_mask_off_count\s*=\s*\n?\s*json_number\(root,\s*"mask_off_count"',
        "summary-backed mask-off parsing")
require(edf, r'cJSON_AddNumberToObject\(root,\s*"mask_off_count",\s*ctx->n_session_entries\)',
        "one mask-off endpoint per Summary session entry")
require(display, r'"Mask on/off · %d".*?day->mask_off_count',
        "History mask on/off badge")
require(display, r'"Mask on/off · —"',
        "truthful unavailable mask on/off state")
require(display, r'\.mask_off_count\s*=\s*12.*?\.has_mask_off_count\s*=\s*true',
        "two-digit QEMU mask-off layout fixture")
require(history_header, r"#define\s+TOUCH_HISTORY_MAX_DAYS\s+30\b",
        "bounded 30-night native history model")
for metric in ("oai", "cai", "hi", "rera"):
    require(history_header, rf"float\s+{metric}\s*;", f"{metric} history value")
    require(history_header, rf"bool\s+has_{metric}\s*;", f"{metric} availability flag")
    require(history, rf"has_{metric}\s*=\s*json_number\(root,\s*\"{metric}\"",
            f"truthful optional {metric} parsing")
require(history, r"capacity\s*<\s*TOUCH_HISTORY_MAX_DAYS.*?capacity\s*:\s*TOUCH_HISTORY_MAX_DAYS",
        "history loader enforces its 30-night bound")
require(display, r"TOUCH_HISTORY_MAX_DAYS", "touch UI consumes the 30-night history bound")
for event_label in ("Obstructive", "Central", "Hypopnea", "RERA"):
    require(display, rf'"{event_label}"', f"{event_label} History event label")
require(display, r'"AHI"', "handoff AHI metric label")
require(display, r"s_keyboard_sheet.*?keyboard_sheet_action_cb",
        "explicit touch keyboard sheet with completion actions")
require(display, r"s_text_keyboard_lower_map.*?\"q\".*?\"p\".*?LV_SYMBOL_BACKSPACE.*?LV_SYMBOL_UP.*?\"123\".*?\"@\".*?\"space\".*?\"-\".*?\"_\"",
        "five-row handoff text keyboard")
require(display, r"lv_obj_set_align\(s_keyboard,\s*LV_ALIGN_TOP_LEFT\)",
        "visible top-aligned keyboard geometry")
require(display, r"lv_textarea_set_text\(s_wifi_password,\s*\"\"\)",
        "stored Wi-Fi password excluded from LVGL")
assert "CONFIG_COMPILER_STACK_CHECK_MODE_STRONG=y" in defaults
assert "CONFIG_ESP_MAIN_TASK_STACK_SIZE=14336" in defaults

print("Waveshare 7B hardware contract passed")
