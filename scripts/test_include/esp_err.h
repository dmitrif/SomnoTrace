/* Minimal ESP error shim for pure host unit tests. */

#pragma once

#include <stdint.h>

typedef int32_t esp_err_t;

#define ESP_OK                    0
#define ESP_FAIL                 -1
#define ESP_ERR_INVALID_ARG       0x102
#define ESP_ERR_INVALID_STATE     0x103
#define ESP_ERR_INVALID_SIZE      0x104
#define ESP_ERR_INVALID_VERSION   0x10A
#define ESP_ERR_NVS_BASE          0x1100
#define ESP_ERR_NVS_NOT_FOUND    (ESP_ERR_NVS_BASE + 0x02)
