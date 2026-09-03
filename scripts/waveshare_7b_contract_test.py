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
history = source("main/touch_history.c")
history_header = source("main/touch_history.h")
main = source("main/main.c")
as11 = source("main/as11_ble.c")

expected_scalars = {
    r"#define\s+I2C_SDA\s+GPIO_NUM_8\b": "I2C SDA GPIO8",
    r"#define\s+I2C_SCL\s+GPIO_NUM_9\b": "I2C SCL GPIO9",
    r"#define\s+IOX_ADDR\s+0x24\b": "CH422G address 0x24",
    r"\.pclk_hz\s*=\s*30850000\b": "30.85 MHz pixel clock",
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
        "ten-line bounce buffer",
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

require(display, r"\.on_bounce_frame_finish\s*=\s*on_vsync", "bounce-frame handoff")
require(display, r"display_driver\.hor_res\s*=\s*WAVESHARE_7B_H_RES", "LVGL width")
require(display, r"display_driver\.ver_res\s*=\s*WAVESHARE_7B_V_RES", "LVGL height")
require(display, r"display_driver\.full_refresh\s*=\s*1", "tear-free full refresh")

for setting in (
    "CONFIG_SOMNOTRACE_BOARD_WAVESHARE_7B=y",
    "CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y",
    "CONFIG_LV_COLOR_DEPTH_16=y",
    "CONFIG_LV_SPRINTF_USE_FLOAT=y",
    "CONFIG_LV_USE_CHART=y",
    "CONFIG_LV_USE_MSGBOX=y",
):
    assert setting in defaults, f"missing 7B sdkconfig default: {setting}"

for unit in ("board_waveshare_7b.c", "bsp_display_7b.c", "bsp_power_7b.c", "bsp_audio_7b.c"):
    assert f'"{unit}"' in cmake, f"7B build omits {unit}"

# The 7-inch build is a native touch application, not a scaled-up status view.
for tab in ("Live", "History", "Devices", "Settings", "System"):
    require(display, rf'lv_tabview_add_tab\(s_tabview,\s*"{tab}"\)', f"{tab} touch tab")
require(display, r"xTaskCreate\(device_scan_task", "non-blocking BLE scan worker")
require(display, r"as11_ble_start_pair\(job->addr\)", "native AirSense pairing action")
require(display, r"as11_ble_confirm_pair\(job->passkey\)", "native AirSense passkey action")
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
require(display, r"s_wake_overlay.*?LV_EVENT_PRESSED", "wake-only touch interception")
require(display, r"sd_storage_deinit\(\);\s*esp_restart\(\)",
        "clean SD unmount before UI restart")
require(history, r"sd_storage_lease_acquire\(SD_LEASE_UPLOAD", "leased history reads")
require(history, r"has_ahi\s*=\s*json_number", "missing AHI remains unavailable")
require(history_header, r"#define\s+TOUCH_HISTORY_MAX_DAYS\s+30\b",
        "bounded 30-night native history model")
for metric in ("oai", "cai", "hi", "rera"):
    require(history_header, rf"float\s+{metric}\s*;", f"{metric} history value")
    require(history_header, rf"bool\s+has_{metric}\s*;", f"{metric} availability flag")
    require(history, rf"has_{metric}\s*=\s*json_number\(root,\s*\"{metric}\"",
            f"truthful optional {metric} parsing")
require(history, r"capacity\s*<\s*TOUCH_HISTORY_MAX_DAYS.*?capacity\s*:\s*TOUCH_HISTORY_MAX_DAYS",
        "history loader enforces its 30-night bound")
require(display, r"Device-reported AHI", "clinical provenance label")
require(display, r"lv_textarea_set_text\(s_wifi_password,\s*\"\"\)",
        "stored Wi-Fi password excluded from LVGL")
assert "CONFIG_COMPILER_STACK_CHECK_MODE_STRONG=y" in defaults

print("Waveshare 7B hardware contract passed")
