/*
 * SomnoTrace board support for Waveshare ESP32-S3-Touch-LCD-7B.
 * Hardware timings and pin assignments are derived from Waveshare's
 * Apache-2.0 ESP-IDF examples for this board.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

#define WAVESHARE_7B_H_RES 1024
#define WAVESHARE_7B_V_RES 600

esp_err_t waveshare_7b_init(esp_lcd_panel_handle_t *panel,
                            esp_lcd_touch_handle_t *touch);
esp_err_t waveshare_7b_set_backlight(bool on);
esp_err_t waveshare_7b_set_brightness(uint8_t percent);
esp_err_t waveshare_7b_set_panel_pclk(uint32_t hz);
esp_err_t waveshare_7b_prepare_sd(void);
