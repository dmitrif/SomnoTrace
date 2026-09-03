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
qemu_patch = source("scripts/qemu-rgb-1024.patch")
touch_patch = source("scripts/qemu-touch.patch")

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
for page in ("Home", "History", "Manage"):
    require(display, rf'"{page}"', f"{page} QEMU navigation label")
for section_label in ("Devices", "Connectivity", "Display", "Alerts", "Storage", "System"):
    require(display, rf'"{section_label}"', f"{section_label} Manage rail label")
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
assert "--disable-dbus-display" in setup
assert "SomnoTrace QEMU is already running" in run
assert "input-send-event" in touch_smoke
assert "emulated touch at" in touch_smoke
assert "emulated touch selected page 1" in touch_smoke
require(touch_smoke, r"x\s*=\s*round\(512\s*\*\s*32767\s*/\s*1023\)",
        "synthetic click uses History pill horizontal centre")
require(touch_smoke, r"y\s*=\s*round\(559\s*\*\s*32767\s*/\s*599\)",
        "synthetic click uses History pill vertical centre")
for launcher in (run, smoke):
    assert "-m 8M" in launcher
    assert "nvram.esp32s3.efuse" in launcher
    assert "timer.esp32s3.timg" in launcher
assert "-display sdl,show-cursor=on" in run
for failure in ("Invalid drawing area", "assert failed", "Guru Meditation Error"):
    assert failure in smoke, f"smoke test does not reject {failure}"

print("QEMU UI contract passed")
