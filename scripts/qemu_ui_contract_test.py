#!/usr/bin/env python3
"""Static contracts for the ESP32-S3 QEMU UI preview."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def source(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(text: str, pattern: str, description: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(f"missing QEMU UI contract: {description}")


cmake = source("main/CMakeLists.txt")
root_cmake = source("CMakeLists.txt")
lvgl_allocator = source("main/somnotrace_lvgl_psram.h")
fonts = source("main/somnotrace_fonts.h")
qemu_defaults = source("sdkconfig.qemu.defaults")
kconfig = source("main/Kconfig.projbuild")
component = source("main/idf_component.yml")
board = source("main/board_qemu.c")
display = source("main/bsp_display_7b.c")
demo = source("main/main_qemu.c")
defaults = source("sdkconfig.qemu.defaults")
setup = source("scripts/setup-qemu-macos.sh")
run = source("scripts/run-qemu-ui.sh")
smoke = source("scripts/test-qemu-ui.sh")
touch_smoke = source("scripts/test-qemu-touch.py")
capture_smoke = source("scripts/capture-qemu-ui.py")
qemu_patch = source("scripts/qemu-rgb-1024.patch")
touch_patch = source("scripts/qemu-touch.patch")
stability_patch = source("scripts/qemu-emulator-stability.patch")

require(kconfig, r"config SOMNOTRACE_BOARD_QEMU.*?bool \"ESP32-S3 QEMU UI emulator \(1024x600\)\"",
        "selectable QEMU board profile")
for unit in ("main_qemu.c", "board_qemu.c", "bsp_display_7b.c"):
    assert f'"{unit}"' in cmake, f"QEMU build omits {unit}"
assert "espressif/esp_lcd_qemu_rgb" in component
require(board, r"\.width\s*=\s*WAVESHARE_7B_H_RES", "native panel width")
require(board, r"\.height\s*=\s*WAVESHARE_7B_V_RES", "native panel height")
require(board, r"QEMU_RGB_TOUCH_POSITION", "QEMU touch MMIO bridge")
require(display, r"display_driver\.hor_res\s*=\s*WAVESHARE_7B_H_RES", "LVGL width")
require(display, r"display_driver\.ver_res\s*=\s*WAVESHARE_7B_V_RES", "LVGL height")
require(display, r"board_qemu_touch_read", "LVGL QEMU touch reader")
require(display, r"s_qemu_requested_tab", "display-task-owned page switching")
require(display, r"#define\s+UI_UPDATE_MS\s+50\b", "responsive 20 Hz UI update cadence")
require(display,
        r"state\.flow_version\s*-\s*seen_flow_version.*?"
        r"flow_presentation_phase.*?%\s*4U.*?"
        r"flow_presentation_phase\s*==\s*0\s*\?\s*2\s*:\s*1.*?"
        r"FLOW_CATCHUP_THRESHOLD.*?append_flow_visual.*?"
        r"seen_flow_version\s*\+=\s*consume",
        "20 Hz paced presentation of burst-delivered 25 Hz flow samples")
require(display,
        r"if\s*\(!backlight\).*?home_was_active\s*=\s*false.*?return",
        "wake-time flow resynchronisation")
require(display,
        r"glow\.round_start\s*=\s*0;.*?glow\.round_end\s*=\s*0;.*?"
        r"trace\.round_start\s*=\s*0;.*?trace\.round_end\s*=\s*0;",
        "bead-free live graph strokes")
require(display, r"esp_lcd_rgb_qemu_get_frame_buffer\(s_panel,\s*&qemu_vram\).*?fb1\s*=\s*\(lv_color_t\s*\*\)qemu_vram\s*\+.*?WAVESHARE_7B_H_RES\s*\*\s*WAVESHARE_7B_V_RES",
        "second-half QEMU VRAM draw buffer")
assert "display_driver.full_refresh" not in display
require(display, r"display_driver\.direct_mode\s*=\s*1;",
        "persistent dirty-region composition")
require(display, r"CONFIG_SOMNOTRACE_BOARD_QEMU.*?if\s*\(!lv_disp_flush_is_last\(drv\)\).*?lv_disp_flush_ready\(drv\).*?return;.*?0,\s*0,\s*WAVESHARE_7B_H_RES,\s*WAVESHARE_7B_V_RES,\s*pixels",
        "single completed QEMU composition handoff")
require(display, r"QEMU UI first frame published",
        "initial-frame input synchronization")
require(root_cmake, r"LV_MEM_CUSTOM_ALLOC=somnotrace_lvgl_alloc",
        "QEMU exercises the 7-inch PSRAM-first LVGL allocator")
require(lvgl_allocator, r"heap_caps_malloc_prefer.*?MALLOC_CAP_SPIRAM.*?MALLOC_CAP_INTERNAL",
        "PSRAM-first allocation with internal fallback")
require(qemu_defaults, r"CONFIG_LV_USE_FONT_COMPRESSED=y",
        "compressed custom-font renderer")
require(display, r'#include\s+"somnotrace_fonts\.h"', "custom bedside fonts")
for family in ("space_grotesk", "ibm_plex_mono"):
    assert family in fonts, f"missing {family} font declarations"
assert "somnotrace_space_grotesk_semibold_32" in fonts
for pattern, description in (
    (r"#define\s+UI_HEADER_H\s+70\b", "70px shared header"),
    (r"#define\s+UI_CONTENT_Y\s+70\b", "content y origin"),
    (r"#define\s+UI_CONTENT_H\s+448\b", "448px shared content region"),
    (r"#define\s+UI_NAV_H\s+82\b", "82px shared navigation region"),
    (r"s_pages\s*\[\s*3\s*\]", "three primary QEMU pages"),
    (r"s_nav_buttons\s*\[\s*3\s*\]", "three custom QEMU navigation buttons"),
    (r"set_active_page\s*\(", "custom page selection"),
):
    require(display, pattern, description)
assert "lv_tabview_create" not in display
assert "lv_tabview_add_tab" not in display
for token, value in (
    ("COLOR_BASE", "0x05070e"),
    ("COLOR_PANEL", "0x181c29"),
    ("COLOR_CARD", "0x101421"),
    ("COLOR_CONTROL", "0x2d333f"),
):
    require(display, rf"#define\s+{token}\s+{value}\b",
            f"handoff {token.lower()} slate token")
for obsolete_gradient_token in (
    "COLOR_BASE_TOP", "COLOR_BASE_END", "COLOR_PANEL_TOP", "COLOR_PANEL_END"
):
    assert obsolete_gradient_token not in display
require(display,
        r"make_card.*?bg_color\(card,\s*lv_color_hex\(COLOR_PANEL\).*?"
        r"bg_grad_dir\(card,\s*LV_GRAD_DIR_NONE.*?"
        r"border_side\(card,\s*LV_BORDER_SIDE_TOP",
        "RGB565-safe panel surface with top highlight")
for page in ("Home", "History", "Manage"):
    require(display, rf'"{page}"', f"{page} QEMU navigation label")
for section_label in ("Devices", "Connectivity", "Display", "Alerts", "Storage", "System"):
    require(display, rf'"{section_label}"', f"{section_label} Manage rail label")

# The QEMU build exercises the same content-sized header status capsule as the
# panel. Protect its one-line geometry, centre alignment, and right-side inset.
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

# Protect the concrete bedside state vocabulary, not just the shell. These
# literals correspond to visible controls or explicit loading/degraded states
# in the handoff and must remain present in the firmware exercised by QEMU.
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
    ("Diagnostics", "system header action"),
    ("Up to date", "firmware status value"),
):
    assert literal in display, f"missing bedside state: {description}"
require(display, r'"Waiting for (?:therapy|breathing) data(?:\.\.\.|…)?"',
        "first-sample loading state")
require(display, r"flow_count\s*>=\s*FLOW_READY_POINTS",
        "valid sample threshold before chart becomes live")
require(display, r'"(?:No recent flow sample|Live data delayed)"', "stale flow state")
for metric in ("AHI", "PRESSURE 95%", "LEAK 95%", "EVENTS PER HOUR"):
    assert metric in display, f"missing History component: {metric}"
require(display, r"history_trace_draw_cb.*?LV_DRAW_MASK_LINE_SIDE_BOTTOM.*?lv_draw_mask_fade_init",
        "faded History overnight area")
require(display, r"trace_baseline.*?lv_obj_set_style_line_dash_width.*?3.*?lv_obj_set_style_line_dash_gap.*?9",
        "3/9 dashed History baseline")
require(demo, r"QEMU preview.*simulated data", "honest simulated-data labeling")
require(display, r"simulated preview", "honest simulated device status")
require(demo, r"bsp_display_qemu_seed_demo\(\)", "deterministic histories and devices")
require(demo, r"click to emulate touch", "interactive touch preview")
assert "bsp_display_qemu_set_tab(tab)" not in demo

for setting in (
    "CONFIG_SOMNOTRACE_BOARD_QEMU=y",
    "CONFIG_SPIRAM_MODE_QUAD=y",
    "# CONFIG_SPIRAM_MODE_OCT is not set",
    "# CONFIG_SPI_FLASH_AUTO_SUSPEND is not set",
    "CONFIG_ESP_CONSOLE_UART_DEFAULT=y",
    "CONFIG_LV_COLOR_DEPTH_16=y",
    "CONFIG_LV_SPRINTF_USE_FLOAT=y",
):
    assert setting in defaults, f"missing QEMU sdkconfig default: {setting}"

assert "40edccac415693c5130f91c01d84176ae6008566" in setup
assert "ESP_RGB_MAX_WIDTH   (1024)" in qemu_patch
assert "esp-rgb-touch" in touch_patch
assert "A_RGB_TOUCH_STATUS" in touch_patch
assert "s->width * s->height * s->bpp" in touch_patch
assert "qemu-touch.patch" in setup
assert "qemu-emulator-stability.patch" in setup
assert 'PATCH_REVISION="4"' in setup
assert "dpy_gfx_update(s->con, 0, 0, s->width, s->height)" in stability_patch
assert "touch_press_latched" in stability_patch
assert "SomnoTrace: retain a complete short click" in stability_patch
require(stability_patch, r"s->touch_pressed\s*=\s*false;.*?s->touch_press_latched\s*=\s*false;",
        "touch latch reset state")
assert "--disable-dbus-display" in setup
assert "SomnoTrace QEMU is already running" in run
assert "input-send-event" in touch_smoke
assert "emulated touch at" in touch_smoke
assert "emulated touch selected page 1" in touch_smoke
assert "QEMU UI first frame published" in touch_smoke
require(touch_smoke, r"click_latency\s*>\s*1\.5",
        "bounded post-render navigation latency")
require(touch_smoke, r"x\s*=\s*round\(512\s*\*\s*32767\s*/\s*1023\)",
        "synthetic click uses History pill horizontal centre")
require(touch_smoke, r"y\s*=\s*round\(559\s*\*\s*32767\s*/\s*599\)",
        "synthetic click uses History pill vertical centre")
for contract, description in (
    ('"-display", "sdl,show-cursor=off"', "cursor-free capture input backend"),
    ('"screendump"', "QMP framebuffer capture"),
    ('dimensions != (1024, 600)', "native capture dimension validation"),
    ('len(sampled_colours) < 8', "blank-frame rejection"),
    ('"Guru Meditation Error"', "runtime panic rejection"),
    ('"home": (0, (330, 559))', "deterministic Home selection"),
    ('"history": (1, (512, 559))', "deterministic History selection"),
    ('"manage": (2, (694, 559))', "deterministic Manage selection"),
    ('"--screen"', "selective screen capture option"),
    ('"--representative"', "literal handoff-state capture option"),
    ('"--interaction-states"', "additional interaction-state capture option"),
    ('"devices"', "therapy-stopped Devices capture"),
    ('"connectivity-password-keyboard"', "open password-keyboard capture"),
    ('"connectivity-password-revealed"', "revealed password capture"),
    ('"connectivity-password-remasked"', "remasked password capture"),
    ('((130, 186), 0.5, None)', "Connectivity rail interaction coordinate"),
    ('((620, 396), 0.5, None)', "password-field interaction coordinate"),
    ('start_offset=log_offset', "new touch/page log synchronization"),
    ('"QEMU UI first frame published"', "initial-frame capture synchronization"),
    ('validate_persistent_shell(', "persistent shell capture validation"),
    ('not name.startswith("connectivity-password-")',
     "modal keyboard shell validation exception"),
    ('validate_interaction_frame(name, payload)', "keyboard clipping rejection"),
):
    assert contract in capture_smoke, f"capture smoke omits {description}"
require(display, r"s_keyboard_sheet.*?keyboard_sheet_action_cb",
        "explicit touch keyboard sheet with completion actions")
require(display, r"s_text_keyboard_lower_map.*?\"q\".*?\"p\".*?LV_SYMBOL_BACKSPACE.*?LV_SYMBOL_UP.*?\"123\".*?\"@\".*?\"space\".*?\"-\".*?\"_\"",
        "five-row handoff text keyboard")
require(display, r"lv_obj_remove_event_cb\(s_keyboard,\s*lv_keyboard_def_event_cb\).*?keyboard_cb",
        "custom functional keyboard legends")
require(display, r"s_keyboard_target\s*==\s*s_wifi_password.*?Editing network password",
        "password-editing section state")
require(display, r"lv_obj_set_align\(s_keyboard,\s*LV_ALIGN_TOP_LEFT\)",
        "visible top-aligned keyboard geometry")
require(display, r"lv_obj_set_y\(s_keyboard,\s*top\s*==\s*356\s*\?\s*58\s*:\s*67\).*?top\s*==\s*356\s*\?\s*168\s*:\s*203",
        "handoff keyboard and keypad bounds")
require(display, r"set_connectivity_editing.*?lv_obj_set_size\(active_row,\s*718,\s*124\).*?lv_obj_set_pos\(target,\s*0,\s*42\).*?lv_obj_set_size\(target,\s*686,\s*60\)",
        "dedicated network-field editing composition")
require(display,
        r"style_manage_field.*?COLOR_LIVE.*?LV_STATE_FOCUSED.*?"
        r"border_width\(field,\s*2,\s*LV_STATE_FOCUSED\).*?"
        r"shadow_width\(field,\s*8,\s*LV_STATE_FOCUSED\)",
        "focused network-field ring")
require(display,
        r"s_wifi_password_reveal.*?112,\s*52,\s*\"Reveal\".*?"
        r"wifi_password_reveal_cb",
        "touch-sized password reveal control")
require(display,
        r"wifi_password_reveal_cb.*?lv_textarea_set_password_mode.*?\"Mask\".*?"
        r"\"Reveal\"",
        "password reveal and remask behavior")
require(display,
        r"close_keyboard_sheet.*?target\s*==\s*s_wifi_password.*?"
        r"lv_textarea_set_password_mode\(s_wifi_password,\s*true\)",
        "password remasked whenever editing closes")
require(display, r"if\s*\(x2\s*>\s*x1\)\s*x2--;",
        "half-open History fill spans")
require(display,
        r"has_scroll_gutter.*?s_manage_scrolls\[1\].*?"
        r"s_manage_scrolls\[4\].*?704\s*:\s*718",
        "scrollbar gutter only on overflowing Manage sections")
require(display, r"bool\s+show_device_change\s*=\s*as_paired\s*\|\|\s*ox_paired;",
        "paired-state therapy-change explanation")
require(display, r"state\.paired\s*\?\s*0x636975",
        "dim stopped-therapy orb")
for launcher in (run, smoke):
    assert "-m 8M" in launcher
    assert "nvram.esp32s3.efuse" in launcher
    assert "timer.esp32s3.timg" in launcher
assert "-display sdl,show-cursor=on" in run
for failure in ("Invalid drawing area", "assert failed", "Guru Meditation Error"):
    assert failure in smoke, f"smoke test does not reject {failure}"
assert "CONFIG_ESP_MAIN_TASK_STACK_SIZE=14336" in qemu_defaults

print("QEMU UI contract passed")
