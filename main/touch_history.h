/* Lightweight SD-backed history model for the native touch UI. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define TOUCH_HISTORY_MAX_DAYS 30
#define TOUCH_HISTORY_TRACE_POINTS 48
#define TOUCH_HISTORY_TRACE_MISSING INT16_MIN

typedef enum {
    TOUCH_HISTORY_CHANNEL_FLOW = 0,
    TOUCH_HISTORY_CHANNEL_SPO2,
    TOUCH_HISTORY_CHANNEL_LEAK,
    TOUCH_HISTORY_CHANNEL_COUNT,
} touch_history_channel_t;

/* Only the currently selected night/channel trace is retained by the UI.
 * Keeping this separate from touch_history_day_t avoids multiplying the
 * trace storage by 30 nights and three channels in scarce internal RAM. */
typedef struct {
    int16_t points[TOUCH_HISTORY_TRACE_POINTS];
    /* Flow is rendered as two parallel time series.  upper_points contains
     * the per-bin maximum while points contains the per-bin minimum; other
     * channels leave upper_points missing. */
    int16_t upper_points[TOUCH_HISTORY_TRACE_POINTS];
    int64_t start_ms;
    int64_t end_ms;
    uint8_t count;
    touch_history_channel_t channel;
    bool has_data;
    bool loaded;
} touch_history_trace_t;

typedef struct {
    char day[9];
    int sessions;
    int mask_off_count;
    int usage_min;
    float ahi;
    float oai;
    float cai;
    float hi;
    float rera;
    float pressure_p95;
    float leak_p95;
    bool has_summary;
    bool has_mask_off_count;
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
 * zero value. Trace data is intentionally deferred until a night is selected. */
esp_err_t touch_history_load(touch_history_day_t *days, size_t capacity,
                             size_t *count);

/* Loads one bounded overview for the given noon-day. Flow uses the longest
 * terminal session's 1 Hz min/max sidecar, Leak its 0.5 Hz PLD track, and
 * SpO2 the ready canonical O2 Ring vitals track with the greatest valid-sample
 * coverage. The function owns an upload/read lease and is intended for a
 * worker task; gaps remain explicit. */
esp_err_t touch_history_load_trace(const char *day,
                                   touch_history_channel_t channel,
                                   touch_history_trace_t *trace);
