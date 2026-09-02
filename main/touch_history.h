/* Lightweight SD-backed history model for the native touch UI. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    char day[9];
    int sessions;
    int usage_min;
    float ahi;
    float pressure_p95;
    float leak_p95;
    bool has_summary;
} touch_history_day_t;

/* Returns newest days first. Safe to call from a worker task. */
esp_err_t touch_history_load(touch_history_day_t *days, size_t capacity,
                             size_t *count);
