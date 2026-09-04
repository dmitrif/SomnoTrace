/* Lightweight SD-backed history model for the native touch UI. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define TOUCH_HISTORY_MAX_DAYS 30
#define TOUCH_HISTORY_TRACE_POINTS 48
#define TOUCH_HISTORY_TRACE_MISSING INT16_MIN

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
    int16_t flow_trace[TOUCH_HISTORY_TRACE_POINTS];
    int64_t flow_trace_start_ms;
    int64_t flow_trace_end_ms;
    uint8_t flow_trace_count;
    bool has_summary;
    bool has_usage;
    bool has_ahi;
    bool has_oai;
    bool has_cai;
    bool has_hi;
    bool has_rera;
    bool has_pressure_p95;
    bool has_leak_p95;
    bool has_flow_trace;
    bool flow_trace_loaded;
} touch_history_day_t;

/* Returns newest days first, capped at TOUCH_HISTORY_MAX_DAYS. Safe to call
 * from a worker task. Each has_* flag distinguishes missing data from a real
 * zero value. Trace data is intentionally deferred until a night is selected. */
esp_err_t touch_history_load(touch_history_day_t *days, size_t capacity,
                             size_t *count);

/* Loads only the longest terminal session's 1 Hz flow overview for day->day.
 * The function owns an upload/read lease and is intended for a worker task;
 * missing samples remain explicit gaps. */
esp_err_t touch_history_load_flow_trace(touch_history_day_t *day);
