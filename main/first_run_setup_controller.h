/*
 * SomnoTrace - first-run setup service controller
 *
 * Slow setup operations run on one PSRAM-backed worker.  The LVGL surface
 * receives only bounded value snapshots and callbacks which enqueue work.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "first_run_setup_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the singleton controller after networking, BLE and the initial card
 * probe have run.  initial_card_result is the real sd_storage_init() result;
 * it is retained so "missing" is never presented as "unreadable". */
esp_err_t first_run_setup_controller_start(esp_err_t initial_card_result);

/* Stop and reclaim the worker stack after the setup surface is destroyed.
 * The small bounded snapshot remains valid until reboot for diagnostics. */
void first_run_setup_controller_stop(void);

/* Controller callbacks passed directly to first_run_setup_ui_create(). */
const first_run_setup_ui_controller_t *
first_run_setup_controller_callbacks(void);

/* Coherent value snapshot. generation changes only when visible state does,
 * allowing the display task to avoid rebuilding an unchanged LVGL pane. */
bool first_run_setup_controller_snapshot(first_run_setup_ui_live_t *out,
                                         uint32_t *generation);

/* The finished callback only sets a flag.  The display task consumes it on a
 * later timer pass, after LVGL has unwound the touch event safely. */
bool first_run_setup_controller_take_finished(void);

#ifdef __cplusplus
}
#endif
