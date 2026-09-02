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
qemu_patch = source("scripts/qemu-rgb-1024.patch")

require(kconfig, r"config SOMNOTRACE_BOARD_QEMU.*?bool \"ESP32-S3 QEMU UI emulator \(1024x600\)\"",
        "selectable QEMU board profile")
for unit in ("main_qemu.c", "board_qemu.c", "bsp_display_7b.c"):
    assert f'"{unit}"' in cmake, f"QEMU build omits {unit}"
assert "espressif/esp_lcd_qemu_rgb" in component
require(board, r"\.width\s*=\s*WAVESHARE_7B_H_RES", "native panel width")
require(board, r"\.height\s*=\s*WAVESHARE_7B_V_RES", "native panel height")
require(display, r"display_driver\.hor_res\s*=\s*WAVESHARE_7B_H_RES", "LVGL width")
require(display, r"display_driver\.ver_res\s*=\s*WAVESHARE_7B_V_RES", "LVGL height")
require(display, r"s_qemu_requested_tab", "display-task-owned tab switching")
require(demo, r"QEMU preview.*simulated data", "honest simulated-data labeling")
require(display, r"simulated preview", "honest simulated device status")
require(demo, r"bsp_display_qemu_seed_demo\(\)", "deterministic histories and devices")
require(demo, r"\(iteration / 80\) % 5", "all five tabs rotate")

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
assert "--disable-dbus-display" in setup
for launcher in (run, smoke):
    assert "-m 8M" in launcher
    assert "nvram.esp32s3.efuse" in launcher
    assert "timer.esp32s3.timg" in launcher
assert "-display sdl,show-cursor=on" in run
for failure in ("Invalid drawing area", "assert failed", "Guru Meditation Error"):
    assert failure in smoke, f"smoke test does not reject {failure}"

print("QEMU UI contract passed")
