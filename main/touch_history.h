/* Lightweight SD-backed history model for the native touch UI. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define TOUCH_HISTORY_MAX_DAYS 30

typedef struct {
    char day[9];
    int sessions;
    int usage_min;
    float ahi;
    float oai;
    float cai;
    float hi;
    float rera;
    float pressure_p95;
    float leak_p95;
    bool has_summary;
    bool has_usage;
    bool has_ahi;
    bool has_oai;
    bool has_cai;
    bool has_hi;
    bool has_rera;
    bool has_pressure_p95;
    bool has_leak_p95;
} touch_history_day_t;

/* Returns newest days first, capped at TOUCH_HISTORY_MAX_DAYS. Safe to call
 * from a worker task. Each has_* flag distinguishes missing data from a real
 * zero value. */
esp_err_t touch_history_load(touch_history_day_t *days, size_t capacity,
                             size_t *count);
