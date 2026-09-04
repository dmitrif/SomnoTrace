/*
 * SomnoTrace - Waveshare 7B first-run setup UI
 *
 * This surface deliberately owns no Wi-Fi, BLE, clock, or storage driver.
 * The display controller supplies truthful snapshots and operation callbacks;
 * this module owns only LVGL presentation and durable setup transitions.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "first_run_setup.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FIRST_RUN_SETUP_UI_WIFI_RESULT_MAX      6U
#define FIRST_RUN_SETUP_UI_TIMEZONE_RESULT_MAX  6U
#define FIRST_RUN_SETUP_UI_AIRSENSE_RESULT_MAX  6U

typedef enum {
    FIRST_RUN_SETUP_UI_WIFI_IDLE = 0,
    FIRST_RUN_SETUP_UI_WIFI_SCANNING,
    FIRST_RUN_SETUP_UI_WIFI_SELECT,
    FIRST_RUN_SETUP_UI_WIFI_PASSWORD,
    FIRST_RUN_SETUP_UI_WIFI_CONNECTING,
    FIRST_RUN_SETUP_UI_WIFI_CONNECTED,
    FIRST_RUN_SETUP_UI_WIFI_AUTH_FAILED,
    FIRST_RUN_SETUP_UI_WIFI_ERROR,
} first_run_setup_ui_wifi_state_t;

typedef enum {
    FIRST_RUN_SETUP_UI_TIME_IDLE = 0,
    FIRST_RUN_SETUP_UI_TIME_SEARCHING,
    FIRST_RUN_SETUP_UI_TIME_APPLYING,
    FIRST_RUN_SETUP_UI_TIME_SET,
    FIRST_RUN_SETUP_UI_TIME_ERROR,
} first_run_setup_ui_time_state_t;

typedef enum {
    FIRST_RUN_SETUP_UI_AIRSENSE_INSTRUCTIONS = 0,
    FIRST_RUN_SETUP_UI_AIRSENSE_SCANNING,
    FIRST_RUN_SETUP_UI_AIRSENSE_SELECT,
    FIRST_RUN_SETUP_UI_AIRSENSE_NOT_FOUND,
    FIRST_RUN_SETUP_UI_AIRSENSE_STARTING,
    FIRST_RUN_SETUP_UI_AIRSENSE_WAIT_CODE,
    FIRST_RUN_SETUP_UI_AIRSENSE_CONFIRMING,
    FIRST_RUN_SETUP_UI_AIRSENSE_CODE_REJECTED,
    FIRST_RUN_SETUP_UI_AIRSENSE_PAIRED,
    FIRST_RUN_SETUP_UI_AIRSENSE_ERROR,
} first_run_setup_ui_airsense_state_t;

typedef enum {
    FIRST_RUN_SETUP_UI_CARD_CHECKING = 0,
    FIRST_RUN_SETUP_UI_CARD_READY,
    FIRST_RUN_SETUP_UI_CARD_MISSING,
    FIRST_RUN_SETUP_UI_CARD_UNREADABLE,
    FIRST_RUN_SETUP_UI_CARD_FULL,
} first_run_setup_ui_card_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi_dbm;
    bool secure;
} first_run_setup_ui_wifi_result_t;

typedef struct {
    /* IANA identifier, for example America/Toronto. */
    char id[48];
    /* POSIX rule required by time_sync_set_timezone(). */
    char posix_tz[64];
    char utc_offset[12];
    char abbreviation[12];
} first_run_setup_ui_timezone_result_t;

typedef struct {
    char name[32];
    char address[24];
    int8_t rssi_dbm;
} first_run_setup_ui_airsense_result_t;

/* A full, pointer-free snapshot. The controller may build it off-thread, then
 * call update while holding the application's LVGL lock. Results are bounded
 * so the setup surface never retains caller-owned memory. */
typedef struct {
    first_run_setup_ui_wifi_state_t wifi_state;
    first_run_setup_ui_time_state_t time_state;
    first_run_setup_ui_airsense_state_t airsense_state;
    first_run_setup_ui_card_state_t card_state;

    bool wifi_configured;
    bool time_configured;
    bool airsense_paired;
    bool alerts_configured;
    bool uploads_configured;

    char connected_ssid[33];
    char selected_ssid[33];
    char local_time[20];
    char timezone_id[48];
    char paired_name[32];
    char paired_address[24];
    char selected_airsense_address[24];
    char card_summary[48];
    char error_message[96];

    first_run_setup_ui_wifi_result_t
        wifi_results[FIRST_RUN_SETUP_UI_WIFI_RESULT_MAX];
    uint8_t wifi_result_count;
    first_run_setup_ui_timezone_result_t
        timezone_results[FIRST_RUN_SETUP_UI_TIMEZONE_RESULT_MAX];
    uint8_t timezone_result_count;
    first_run_setup_ui_airsense_result_t
        airsense_results[FIRST_RUN_SETUP_UI_AIRSENSE_RESULT_MAX];
    uint8_t airsense_result_count;
} first_run_setup_ui_live_t;

/* Callbacks start real operations; completion is reported later by update().
 * Every callback executes on the LVGL caller/task and must return promptly.
 * A NULL callback leaves its action disabled rather than pretending success. */
typedef struct {
    void *context;
    esp_err_t (*wifi_scan)(void *context);
    esp_err_t (*wifi_connect)(void *context, const char *ssid,
                              const char *password);
    esp_err_t (*timezone_search)(void *context, const char *query);
    esp_err_t (*timezone_select)(void *context, const char *iana_id,
                                 const char *posix_tz);
    esp_err_t (*airsense_scan)(void *context);
    esp_err_t (*airsense_begin_pairing)(void *context,
                                        const char *device_address);
    /* String form preserves a valid leading zero in the AirSense passkey. */
    esp_err_t (*airsense_confirm_code)(void *context,
                                       const char four_digit_code[5]);
    esp_err_t (*card_retry)(void *context);
    esp_err_t (*configure_alerts)(void *context);
    esp_err_t (*configure_uploads)(void *context);
    esp_err_t (*finished)(void *context);
} first_run_setup_ui_controller_t;

typedef struct {
    first_run_setup_snapshot_t durable;
    first_run_setup_ui_live_t live;
    first_run_setup_step_t displayed_step;
    bool created;
    bool visible;
} first_run_setup_ui_snapshot_t;

/* All functions that mutate LVGL must be called from the LVGL task, or while
 * holding the application's LVGL lock. create() is deliberately lazy: until
 * it is called, this module allocates no LVGL objects or snapshot storage. */
esp_err_t first_run_setup_ui_create(
    lv_obj_t *parent, const first_run_setup_ui_controller_t *controller);
void first_run_setup_ui_destroy(void);

/* show() rebuilds the current step before clearing HIDDEN, guaranteeing the
 * first visible frame is complete. hide() retains the small active tree for a
 * resumable setup; destroy() releases it after setup is finished. */
esp_err_t first_run_setup_ui_show(void);
void first_run_setup_ui_hide(void);
bool first_run_setup_ui_is_visible(void);

esp_err_t first_run_setup_ui_update(
    const first_run_setup_ui_live_t *snapshot);
void first_run_setup_ui_snapshot(first_run_setup_ui_snapshot_t *out);
lv_obj_t *first_run_setup_ui_root(void);

#ifdef __cplusplus
}
#endif
