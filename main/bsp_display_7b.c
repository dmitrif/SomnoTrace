/*
 * SomnoTrace native 1024x600 touch UI for Waveshare ESP32-S3-Touch-LCD-7B.
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "bsp_display.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "as11_ble.h"
#include "board_waveshare_7b.h"
#include "device_settings.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#if CONFIG_SOMNOTRACE_BOARD_QEMU
#include "board_qemu.h"
#include "esp_lcd_qemu_rgb.h"
#else
#include "esp_lcd_panel_rgb.h"
#endif
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "net_provision.h"
#include "oximeter.h"
#include "sd_storage.h"
#include "therapy_alert.h"
#include "touch_history.h"

#define FLOW_POINTS 300
#define UI_UPDATE_MS 100
#define HISTORY_MAX_DAYS 12
#define DEVICE_RESULT_MAX 8

typedef struct {
    bool wifi;
    bool paired;
    bool sd_ready;
    bool therapy;
    bool charging;
    int battery;
    float leak;
    float pressure;
    float respiratory_rate;
    float flow_limitation;
    int64_t therapy_start_us;
    int16_t flow[FLOW_POINTS];
    unsigned flow_head;
    unsigned flow_version;
    char title[48];
    char status[192];
    char attention[256];
    char notice[64];
    int64_t notice_expires_us;
    bool notice_critical;
} ui_state_t;

typedef struct {
    char addr[18];
    char name[40];
    int rssi;
    ox_driver_t driver;
} ui_device_result_t;

typedef struct {
    touch_history_day_t history[HISTORY_MAX_DAYS];
    size_t history_count;
    unsigned history_version;
    bool history_busy;
    ui_device_result_t as11[DEVICE_RESULT_MAX];
    size_t as11_count;
    unsigned as11_version;
    bool as11_busy;
    ui_device_result_t ox[DEVICE_RESULT_MAX];
    size_t ox_count;
    unsigned ox_version;
    bool ox_busy;
    esp_err_t history_result;
} ui_service_state_t;

typedef enum {
    BLE_UI_IDLE,
    BLE_UI_SCAN_AS11,
    BLE_UI_SCAN_OX,
    BLE_UI_PAIR_AS11,
    BLE_UI_PAIR_OX,
    BLE_UI_FORGET,
} ble_ui_operation_t;

static const char *TAG = "display_7b";

#if CONFIG_SOMNOTRACE_BOARD_QEMU
#define UI_BOARD_NAME "ESP32-S3 QEMU UI preview"
#define UI_TOUCH_STATUS "not emulated"
#else
#define UI_BOARD_NAME "Waveshare ESP32-S3 Touch LCD 7B"
#define UI_TOUCH_STATUS (s_touch ? "GT911 ready" : "not detected")
#endif
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static ui_state_t s_state;
static ui_service_state_t s_services;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static SemaphoreHandle_t s_lvgl_lock;
static TaskHandle_t s_lvgl_task;
static uint8_t s_brightness = 100;
static bool s_backlight = true;
static void (*s_setup_callback)(void);
static uint32_t s_flush_count;
static uint32_t s_flush_timeouts;
static uint32_t s_touch_read_errors;
static lv_coord_t s_last_touch_x;
static lv_coord_t s_last_touch_y;
static bool s_touch_services_ready;
static bool s_as11_service_ready;
static bool s_ox_service_ready;
static bool s_therapy_command_busy;
static bool s_therapy_command_target;
static ble_ui_operation_t s_ble_operation;
static int64_t s_ble_operation_started_us;

static lv_obj_t *s_title_label;
static lv_obj_t *s_clock_label;
static lv_obj_t *s_status_label;
static lv_obj_t *s_wifi_label;
static lv_obj_t *s_ble_label;
static lv_obj_t *s_sd_label;
static lv_obj_t *s_therapy_label;
static lv_obj_t *s_leak_label;
static lv_obj_t *s_pressure_label;
static lv_obj_t *s_resp_label;
static lv_obj_t *s_flow_lim_label;
static lv_obj_t *s_runtime_label;
static lv_obj_t *s_notice_label;
static lv_obj_t *s_therapy_button_label;
static lv_obj_t *s_therapy_button;
static lv_obj_t *s_attention_card;
static lv_obj_t *s_attention_title;
static lv_obj_t *s_attention_body;
static lv_chart_series_t *s_flow_series;
static lv_obj_t *s_chart;
static lv_obj_t *s_tabview;
static lv_obj_t *s_pages[5];
static lv_obj_t *s_history_status;
static lv_obj_t *s_history_rows[HISTORY_MAX_DAYS];
static lv_obj_t *s_history_row_labels[HISTORY_MAX_DAYS];
static lv_obj_t *s_history_detail;
static lv_obj_t *s_as11_status;
static lv_obj_t *s_as11_dropdown;
static lv_obj_t *s_as11_pair_button;
static lv_obj_t *s_passkey_confirm_button;
static lv_obj_t *s_passkey;
static lv_obj_t *s_ox_status;
static lv_obj_t *s_ox_dropdown;
static lv_obj_t *s_settings_brightness;
static lv_obj_t *s_settings_brightness_value;
static lv_obj_t *s_settings_therapy_mode;
static lv_obj_t *s_network_status;
static lv_obj_t *s_wifi_ssid;
static lv_obj_t *s_wifi_password;
static lv_obj_t *s_system_details;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_wake_overlay;
static unsigned s_seen_history_version;
static unsigned s_seen_as11_version;
static unsigned s_seen_ox_version;
static int s_history_selection;
static bool s_settings_synced;
static bool s_settings_save_busy;
static unsigned s_settings_save_generation;
static char s_saved_wifi_ssid[NETPROV_SSID_MAXLEN + 1];
#if CONFIG_SOMNOTRACE_BOARD_QEMU
static uint8_t s_qemu_requested_tab = UINT8_MAX;
#endif

static bool begin_ble_operation(ble_ui_operation_t operation)
{
    bool started = false;
    portENTER_CRITICAL(&s_state_lock);
    if (s_ble_operation == BLE_UI_IDLE) {
        s_ble_operation = operation;
        s_ble_operation_started_us = esp_timer_get_time();
        started = true;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return started;
}

static void end_ble_operation(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_ble_operation = BLE_UI_IDLE;
    s_ble_operation_started_us = 0;
    portEXIT_CRITICAL(&s_state_lock);
}

static bool lock_lvgl(TickType_t timeout)
{
    return s_lvgl_lock && xSemaphoreTakeRecursive(s_lvgl_lock, timeout) == pdTRUE;
}

static void unlock_lvgl(void)
{
    xSemaphoreGiveRecursive(s_lvgl_lock);
}

#if !CONFIG_SOMNOTRACE_BOARD_QEMU
static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t panel,
                              const esp_lcd_rgb_panel_event_data_t *event,
                              void *ctx)
{
    (void)panel;
    (void)event;
    (void)ctx;
    BaseType_t wake = pdFALSE;
    if (s_lvgl_task) vTaskNotifyGiveFromISR(s_lvgl_task, &wake);
    return wake == pdTRUE;
}
#endif

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                     lv_color_t *pixels)
{
#if !CONFIG_SOMNOTRACE_BOARD_QEMU
    ulTaskNotifyTake(pdTRUE, 0);
#endif
    esp_lcd_panel_draw_bitmap((esp_lcd_panel_handle_t)drv->user_data,
                              area->x1, area->y1, area->x2 + 1, area->y2 + 1,
                              pixels);
    /* Frame swapping occurs on VSYNC. A timeout keeps UI recovery possible if
     * the panel cable is disconnected during a test. */
    s_flush_count++;
#if !CONFIG_SOMNOTRACE_BOARD_QEMU
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)) == 0) {
        s_flush_timeouts++;
        if (s_flush_timeouts == 1 || (s_flush_timeouts % 100) == 0) {
            ESP_LOGW(TAG, "RGB frame handoff timeout (%lu total)",
                     (unsigned long)s_flush_timeouts);
        }
    }
#endif
    lv_disp_flush_ready(drv);
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    uint16_t x = 0;
    uint16_t y = 0;
    bool pressed = false;
    static bool was_pressed;
    board_qemu_touch_read(&x, &y, &pressed);
    s_last_touch_x = x < WAVESHARE_7B_H_RES ? x : WAVESHARE_7B_H_RES - 1;
    s_last_touch_y = y < WAVESHARE_7B_V_RES ? y : WAVESHARE_7B_V_RES - 1;
    data->point.x = s_last_touch_x;
    data->point.y = s_last_touch_y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (pressed && !was_pressed) {
        ESP_LOGI(TAG, "emulated touch at %u,%u", (unsigned)x, (unsigned)y);
    }
    was_pressed = pressed;
    (void)drv;
#else
    esp_lcd_touch_handle_t touch = (esp_lcd_touch_handle_t)drv->user_data;
    esp_lcd_touch_point_data_t point = {0};
    uint8_t count = 0;
    esp_err_t read_result = touch ? esp_lcd_touch_read_data(touch) : ESP_FAIL;
    esp_err_t point_result = ESP_FAIL;
    if (read_result == ESP_OK) {
        point_result = esp_lcd_touch_get_data(touch, &point, &count, 1);
    }
    if (read_result == ESP_OK && point_result == ESP_OK && count) {
        s_last_touch_x = point.x < WAVESHARE_7B_H_RES ? point.x
                                                       : WAVESHARE_7B_H_RES - 1;
        s_last_touch_y = point.y < WAVESHARE_7B_V_RES ? point.y
                                                       : WAVESHARE_7B_V_RES - 1;
        data->point.x = s_last_touch_x;
        data->point.y = s_last_touch_y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->point.x = s_last_touch_x;
        data->point.y = s_last_touch_y;
        if (touch && (read_result != ESP_OK || point_result != ESP_OK))
            s_touch_read_errors++;
        data->state = LV_INDEV_STATE_RELEASED;
    }
#endif
}

static void tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(5);
}

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x121d32), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x24344f), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_text_color(card, lv_color_hex(0xe7edf7), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *make_button(lv_obj_t *parent, int x, int y, int w,
                             const char *text, uint32_t color,
                             lv_event_cb_t callback, intptr_t action)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, w, 104);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(color + 0x101010), LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, (void *)action);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
    return button;
}

static lv_obj_t *make_touch_button(lv_obj_t *parent, int x, int y, int w, int h,
                                   const char *text, uint32_t color,
                                   lv_event_cb_t callback, intptr_t action)
{
    lv_obj_t *button = make_button(parent, x, y, w, text, color, callback, action);
    lv_obj_set_height(button, h);
    return button;
}

static lv_obj_t *make_section_title(lv_obj_t *parent, const char *text, int x, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xf4f7fb), 0);
    return label;
}

static void save_settings_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        portENTER_CRITICAL(&s_state_lock);
        unsigned generation = s_settings_save_generation;
        portEXIT_CRITICAL(&s_state_lock);

        device_settings_t settings = *device_settings_get();
        esp_err_t result = device_settings_save(&settings);

        portENTER_CRITICAL(&s_state_lock);
        bool latest = generation == s_settings_save_generation;
        if (latest) s_settings_save_busy = false;
        portEXIT_CRITICAL(&s_state_lock);
        if (latest) {
            bsp_display_set_notice(result == ESP_OK ? "Settings saved"
                                                    : "Could not save settings");
            vTaskDelete(NULL);
            return;
        }
    }
}

static void queue_settings_save(void)
{
    if (!s_touch_services_ready) {
        bsp_display_set_notice("Settings service is still starting");
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_settings_save_generation++;
    bool start_worker = !s_settings_save_busy;
    if (start_worker) s_settings_save_busy = true;
    portEXIT_CRITICAL(&s_state_lock);
    if (start_worker &&
        xTaskCreate(save_settings_task, "ui_settings", 3072, NULL, 3, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_state_lock);
        s_settings_save_busy = false;
        portEXIT_CRITICAL(&s_state_lock);
        bsp_display_set_notice("Unable to save settings");
    }
}

static void brightness_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    int value = lv_slider_get_value(lv_event_get_target(event));
    if (code == LV_EVENT_VALUE_CHANGED) {
        device_settings_set_brightness((uint8_t)value);
        lv_label_set_text_fmt(s_settings_brightness_value, "%d%%", (value + 1) / 2);
    } else if (code == LV_EVENT_RELEASED) {
        queue_settings_save();
    }
}

static void therapy_mode_cb(lv_event_t *event)
{
    uint16_t selected = lv_dropdown_get_selected(lv_event_get_target(event));
    static const lcd_therapy_mode_t modes[] = {
        LCD_THERAPY_GRAPH, LCD_THERAPY_INFO,
        LCD_THERAPY_OFF, LCD_THERAPY_ALWAYS_OFF
    };
    if (selected < sizeof(modes) / sizeof(modes[0])) {
        device_settings_set_lcd_therapy_mode(modes[selected]);
        bsp_display_apply_backlight_policy(false);
        queue_settings_save();
    }
}

#if !CONFIG_SOMNOTRACE_BOARD_QEMU
static void history_task(void *arg)
{
    (void)arg;
    touch_history_day_t local[HISTORY_MAX_DAYS] = {0};
    size_t count = 0;
    esp_err_t result = touch_history_load(local, HISTORY_MAX_DAYS, &count);
    portENTER_CRITICAL(&s_state_lock);
    if (result == ESP_OK) {
        memcpy(s_services.history, local, sizeof(local));
        s_services.history_count = count;
    } else {
        s_services.history_count = 0;
    }
    s_services.history_result = result;
    s_services.history_busy = false;
    s_services.history_version++;
    portEXIT_CRITICAL(&s_state_lock);
    vTaskDelete(NULL);
}
#endif

static void start_history_load(void)
{
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    /* Keep the seeded preview history intact. QEMU does not emulate SDMMC. */
    return;
#else
    portENTER_CRITICAL(&s_state_lock);
    bool busy = s_services.history_busy;
    if (!busy) s_services.history_busy = true;
    portEXIT_CRITICAL(&s_state_lock);
    if (!busy && xTaskCreate(history_task, "ui_history", 8192, NULL, 2, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_state_lock);
        s_services.history_busy = false;
        portEXIT_CRITICAL(&s_state_lock);
    }
#endif
}

static void device_scan_task(void *arg)
{
    bool oxygen = (intptr_t)arg == 1;
    cJSON *results = NULL;
    esp_err_t result = bsp_display_is_therapy_active()
                       ? ESP_ERR_INVALID_STATE
                       : (oxygen ? oximeter_scan(7) : as11_ble_scan(7));
    if (result == ESP_OK)
        results = oxygen ? oximeter_get_scan_results() : as11_ble_get_scan_results();

    ui_device_result_t local[DEVICE_RESULT_MAX] = {0};
    size_t count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        if (count >= DEVICE_RESULT_MAX) break;
        cJSON *addr = cJSON_GetObjectItemCaseSensitive(item, "addr");
        cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        cJSON *rssi = cJSON_GetObjectItemCaseSensitive(item, "rssi");
        cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        if (!cJSON_IsString(addr)) continue;
        strlcpy(local[count].addr, addr->valuestring, sizeof(local[count].addr));
        strlcpy(local[count].name,
                cJSON_IsString(name) && name->valuestring[0] ? name->valuestring : "Unnamed device",
                sizeof(local[count].name));
        local[count].rssi = cJSON_IsNumber(rssi) ? rssi->valueint : 0;
        local[count].driver = cJSON_IsString(type) && !strcmp(type->valuestring, "legacy")
                                ? OX_DRIVER_LEGACY : OX_DRIVER_OXYII;
        count++;
    }
    cJSON_Delete(results);

    portENTER_CRITICAL(&s_state_lock);
    ui_device_result_t *target = oxygen ? s_services.ox : s_services.as11;
    memcpy(target, local, sizeof(local));
    if (oxygen) {
        s_services.ox_count = count;
        s_services.ox_busy = false;
        s_services.ox_version++;
    } else {
        s_services.as11_count = count;
        s_services.as11_busy = false;
        s_services.as11_version++;
    }
    portEXIT_CRITICAL(&s_state_lock);
    end_ble_operation();
    vTaskDelete(NULL);
}

static void scan_cb(lv_event_t *event)
{
    bool oxygen = (intptr_t)lv_event_get_user_data(event) == 1;
    if (!s_touch_services_ready || (oxygen ? !s_ox_service_ready
                                          : !s_as11_service_ready)) {
        bsp_display_set_notice("Pairing services are still starting");
        return;
    }
    if (bsp_display_is_therapy_active()) {
        bsp_display_set_notice("Stop therapy before scanning for devices");
        return;
    }
    if (!begin_ble_operation(oxygen ? BLE_UI_SCAN_OX : BLE_UI_SCAN_AS11)) {
        bsp_display_set_notice("Another Bluetooth action is already running");
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    if (oxygen) s_services.ox_busy = true;
    else s_services.as11_busy = true;
    portEXIT_CRITICAL(&s_state_lock);
    bsp_display_set_notice(oxygen ? "Scanning for O2 rings..." : "Scanning for AirSense 11...");
    if (xTaskCreate(device_scan_task, oxygen ? "ui_scan_ox" : "ui_scan_as11",
                    8192, (void *)(intptr_t)(oxygen ? 1 : 0), 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_state_lock);
        if (oxygen) s_services.ox_busy = false;
        else s_services.as11_busy = false;
        portEXIT_CRITICAL(&s_state_lock);
        end_ble_operation();
    }
}

typedef enum { DEVICE_PAIR_AS11, DEVICE_PAIR_OX, DEVICE_CONFIRM_AS11,
               DEVICE_FORGET_AS11, DEVICE_FORGET_OX } device_action_t;

typedef struct {
    device_action_t action;
    char addr[18];
    char passkey[5];
    ox_driver_t driver;
} device_job_t;

static void device_action_task(void *arg)
{
    device_job_t *job = arg;
    esp_err_t result = ESP_FAIL;
    if (bsp_display_is_therapy_active()) {
        result = ESP_ERR_INVALID_STATE;
    } else if (job->action == DEVICE_PAIR_AS11) {
        result = as11_ble_start_pair(job->addr);
    } else if (job->action == DEVICE_PAIR_OX) {
        result = oximeter_pair(job->addr, OX_DRIVER_AUTO);
    } else if (job->action == DEVICE_CONFIRM_AS11) {
        result = as11_ble_confirm_pair(job->passkey);
    } else if (job->action == DEVICE_FORGET_AS11) {
        result = as11_ble_forget();
    } else if (job->action == DEVICE_FORGET_OX) {
        result = oximeter_forget();
    }
    bsp_display_set_notice(result == ESP_OK ? "Device action started" : "Device action failed");
    if (result != ESP_OK || job->action == DEVICE_FORGET_AS11 ||
        job->action == DEVICE_FORGET_OX) end_ble_operation();
    free(job);
    vTaskDelete(NULL);
}

static void device_action_cb(lv_event_t *event)
{
    device_action_t action = (device_action_t)(intptr_t)lv_event_get_user_data(event);
    bool oxygen = action == DEVICE_PAIR_OX || action == DEVICE_FORGET_OX;
    if (!s_touch_services_ready || (oxygen ? !s_ox_service_ready
                                          : !s_as11_service_ready)) {
        bsp_display_set_notice("Pairing services are still starting");
        return;
    }
    if (bsp_display_is_therapy_active()) {
        bsp_display_set_notice("Stop therapy before changing paired devices");
        return;
    }
    if (action == DEVICE_CONFIRM_AS11 &&
        strcmp(as11_ble_get_status(), AS11_STATUS_WAIT_PASSKEY) != 0) {
        bsp_display_set_notice("AirSense is not waiting for a passkey");
        return;
    }
    ble_ui_operation_t operation =
        action == DEVICE_PAIR_AS11 || action == DEVICE_CONFIRM_AS11 ? BLE_UI_PAIR_AS11 :
        action == DEVICE_PAIR_OX ? BLE_UI_PAIR_OX : BLE_UI_FORGET;
    portENTER_CRITICAL(&s_state_lock);
    bool continuing_passkey = action == DEVICE_CONFIRM_AS11 &&
                              s_ble_operation == BLE_UI_PAIR_AS11;
    portEXIT_CRITICAL(&s_state_lock);
    if (!continuing_passkey && !begin_ble_operation(operation)) {
        bsp_display_set_notice("Another Bluetooth action is already running");
        return;
    }
    device_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        if (!continuing_passkey) end_ble_operation();
        bsp_display_set_notice("Unable to allocate pairing job");
        return;
    }
    job->action = action;
    if (action == DEVICE_PAIR_AS11) {
        uint16_t selected = lv_dropdown_get_selected(s_as11_dropdown);
        portENTER_CRITICAL(&s_state_lock);
        if (selected < s_services.as11_count)
            *job = (device_job_t){ .action = action, .driver = OX_DRIVER_AUTO };
        if (selected < s_services.as11_count)
            strlcpy(job->addr, s_services.as11[selected].addr, sizeof(job->addr));
        portEXIT_CRITICAL(&s_state_lock);
    } else if (action == DEVICE_PAIR_OX) {
        uint16_t selected = lv_dropdown_get_selected(s_ox_dropdown);
        portENTER_CRITICAL(&s_state_lock);
        if (selected < s_services.ox_count) {
            strlcpy(job->addr, s_services.ox[selected].addr, sizeof(job->addr));
            job->driver = OX_DRIVER_AUTO;
        }
        portEXIT_CRITICAL(&s_state_lock);
    } else if (action == DEVICE_CONFIRM_AS11) {
        strlcpy(job->passkey, lv_textarea_get_text(s_passkey), sizeof(job->passkey));
    }
    if ((action == DEVICE_PAIR_AS11 || action == DEVICE_PAIR_OX) && !job->addr[0]) {
        free(job);
        if (!continuing_passkey) end_ble_operation();
        bsp_display_set_notice("Scan and select a device first");
        return;
    }
    if (action == DEVICE_CONFIRM_AS11 && strlen(job->passkey) != 4) {
        free(job);
        if (!continuing_passkey) end_ble_operation();
        bsp_display_set_notice("Enter the 4-digit AirSense code");
        return;
    }
    if (xTaskCreate(device_action_task, "ui_pair", 6144,
                    job, 4, NULL) != pdPASS) {
        free(job);
        if (!continuing_passkey) end_ble_operation();
        bsp_display_set_notice("Unable to start pairing action");
    }
}

static void forget_dialog_cb(lv_event_t *event)
{
    lv_obj_t *dialog = lv_event_get_current_target(event);
    const char *button = lv_msgbox_get_active_btn_text(dialog);
    if (!button) return;
    if (!strcmp(button, "Forget")) {
        if (!begin_ble_operation(BLE_UI_FORGET)) {
            bsp_display_set_notice("Another Bluetooth action is already running");
            lv_msgbox_close(dialog);
            return;
        }
        device_job_t *job = calloc(1, sizeof(*job));
        if (job) {
            job->action = (device_action_t)(intptr_t)lv_event_get_user_data(event);
            if (xTaskCreate(device_action_task, "ui_forget", 4096,
                            job, 4, NULL) != pdPASS) {
                free(job);
                end_ble_operation();
            }
        } else {
            end_ble_operation();
        }
    }
    lv_msgbox_close(dialog);
}

static void forget_prompt_cb(lv_event_t *event)
{
    device_action_t action = (device_action_t)(intptr_t)lv_event_get_user_data(event);
    bool oxygen = action == DEVICE_FORGET_OX;
    if (!s_touch_services_ready || (oxygen ? !s_ox_service_ready
                                          : !s_as11_service_ready)) {
        bsp_display_set_notice("Pairing service is unavailable");
        return;
    }
    if (bsp_display_is_therapy_active()) {
        bsp_display_set_notice("Stop therapy before forgetting a device");
        return;
    }
    static const char *buttons[] = { "Cancel", "Forget", "" };
    const char *device = action == DEVICE_FORGET_AS11 ? "AirSense 11" : "O2 ring";
    char question[128];
    snprintf(question, sizeof(question),
             "Remove the saved %s pairing? You will need to pair it again.", device);
    lv_obj_t *dialog = lv_msgbox_create(NULL, "Forget device?", question, buttons, true);
    lv_obj_set_width(dialog, 620);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(0x172640), 0);
    lv_obj_set_style_text_color(dialog, lv_color_hex(0xe7edf7), 0);
    lv_obj_add_event_cb(dialog, forget_dialog_cb, LV_EVENT_VALUE_CHANGED,
                        (void *)(intptr_t)action);
    lv_obj_center(dialog);
}

static void passkey_focus_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_FOCUSED) {
        lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_NUMBER);
        lv_keyboard_set_textarea(s_keyboard, lv_event_get_target(event));
        lv_obj_set_pos(s_keyboard, 512, 176);
        lv_obj_set_size(s_keyboard, 480, 306);
        lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_keyboard);
    }
}

static void text_focus_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_FOCUSED) {
        lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_keyboard_set_textarea(s_keyboard, lv_event_get_target(event));
        /* Use the otherwise-idle left settings panel as a full-height editor;
         * the focused network field remains visible on the right. */
        lv_obj_set_pos(s_keyboard, 16, 84);
        lv_obj_set_size(s_keyboard, 484, 430);
        lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_keyboard);
    }
}

static void keyboard_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(s_keyboard, NULL);
    }
}

static void history_row_cb(lv_event_t *event)
{
    s_history_selection = (int)(intptr_t)lv_event_get_user_data(event);
}

static void refresh_cb(lv_event_t *event)
{
    (void)event;
    start_history_load();
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    if (bsp_display_is_therapy_active() || sd_storage_recording_active()) {
        bsp_display_set_notice("Restart cancelled: therapy recording is active");
        vTaskDelete(NULL);
        return;
    }
    if (!sd_storage_lease_acquire(SD_LEASE_DESTRUCTIVE, 1000)) {
        bsp_display_set_notice("Restart cancelled: microSD is busy");
        vTaskDelete(NULL);
        return;
    }
    if (bsp_display_is_therapy_active() || sd_storage_recording_active()) {
        sd_storage_lease_release(SD_LEASE_DESTRUCTIVE);
        bsp_display_set_notice("Restart cancelled: therapy recording started");
        vTaskDelete(NULL);
        return;
    }
    sd_storage_deinit();
    esp_restart();
}

static void reboot_cb(lv_event_t *event)
{
    (void)event;
    if (bsp_display_is_therapy_active() || sd_storage_recording_active()) {
        bsp_display_set_notice("Stop therapy before restarting");
        return;
    }
    bsp_display_set_notice("Restarting SomnoTrace...");
    xTaskCreate(reboot_task, "ui_reboot", 2048, NULL, 5, NULL);
}

typedef struct {
    char ssid[NETPROV_SSID_MAXLEN + 1];
    char password[NETPROV_PASS_MAXLEN + 1];
    bool keep_password;
} wifi_job_t;

static void wifi_save_task(void *arg)
{
    wifi_job_t *job = arg;
    struct netprov_config cfg = {0};
    netprov_load_config(&cfg);
    strlcpy(cfg.wifi[0].ssid, job->ssid, sizeof(cfg.wifi[0].ssid));
    if (!job->keep_password)
        strlcpy(cfg.wifi[0].pass, job->password, sizeof(cfg.wifi[0].pass));
    esp_err_t result = netprov_save_config(&cfg);
    free(job);
    if (result == ESP_OK) {
        bsp_display_set_notice("Wi-Fi saved; restarting...");
        reboot_task(NULL);
        return;
    }
    bsp_display_set_notice("Could not save Wi-Fi settings");
    vTaskDelete(NULL);
}

static void wifi_save_cb(lv_event_t *event)
{
    (void)event;
    if (!s_touch_services_ready) {
        bsp_display_set_notice("Network service is still starting");
        return;
    }
    if (bsp_display_is_therapy_active()) {
        bsp_display_set_notice("Stop therapy before changing Wi-Fi");
        return;
    }
    const char *ssid = lv_textarea_get_text(s_wifi_ssid);
    const char *password = lv_textarea_get_text(s_wifi_password);
    if (!ssid || !ssid[0]) {
        bsp_display_set_notice("Enter a Wi-Fi network name");
        return;
    }
    wifi_job_t *job = calloc(1, sizeof(*job));
    if (!job) return;
    strlcpy(job->ssid, ssid, sizeof(job->ssid));
    strlcpy(job->password, password ? password : "", sizeof(job->password));
    job->keep_password = !job->password[0] && !strcmp(job->ssid, s_saved_wifi_ssid);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    if (xTaskCreate(wifi_save_task, "ui_wifi_save", 4096, job, 4, NULL) != pdPASS) {
        free(job);
        bsp_display_set_notice("Unable to save Wi-Fi settings");
    }
}

static void tabview_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    if (lv_tabview_get_tab_act(s_tabview) == 1) start_history_load();
    if (s_keyboard) {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(s_keyboard, NULL);
    }
}

static void action_task(void *arg)
{
    intptr_t action = (intptr_t)arg;
    esp_err_t result = ESP_OK;
    if (action == 5 || action == 6) {
        bool start = action == 5;
        bool active = bsp_display_is_therapy_active();
        if (active != start) {
            result = start ? as11_ble_start_therapy() : as11_ble_stop_therapy();
        }
    } else if (action == 2) {
        therapy_alert_acknowledge();
    }
    if (action == 5 || action == 6) {
        portENTER_CRITICAL(&s_state_lock);
        s_therapy_command_busy = false;
        portEXIT_CRITICAL(&s_state_lock);
        bsp_display_set_notice(result == ESP_OK ? "Therapy command sent" :
                                                  "Therapy command failed");
    } else if (action == 2) {
        bsp_display_set_notice("Alert acknowledged");
    }
    vTaskDelete(NULL);
}

static void action_cb(lv_event_t *event)
{
    intptr_t action = (intptr_t)lv_event_get_user_data(event);
    if (action == 1) {
        portENTER_CRITICAL(&s_state_lock);
        bool busy = s_therapy_command_busy;
        bool start = !s_state.therapy;
        if (!busy) {
            s_therapy_command_busy = true;
            s_therapy_command_target = start;
        }
        portEXIT_CRITICAL(&s_state_lock);
        if (busy) return;
        bsp_display_set_notice(start ? "Starting therapy..." : "Stopping therapy...");
        if (xTaskCreate(action_task, "ui_therapy", 4096,
                        (void *)(intptr_t)(start ? 5 : 6), 4, NULL) != pdPASS) {
            portENTER_CRITICAL(&s_state_lock);
            s_therapy_command_busy = false;
            portEXIT_CRITICAL(&s_state_lock);
            bsp_display_set_notice("Unable to start therapy action");
        }
    } else if (action == 2) {
        if (xTaskCreate(action_task, "ui_action", 4096,
                        (void *)action, 4, NULL) != pdPASS) {
            bsp_display_set_notice("Unable to start touch action");
        }
    } else if (action == 3) {
        if (bsp_display_is_therapy_active()) {
            bsp_display_set_notice("Stop therapy before starting Wi-Fi setup");
            return;
        }
        bsp_display_set_notice("Starting Wi-Fi setup hotspot...");
        if (s_setup_callback) s_setup_callback();
    } else if (action == 4) {
        bsp_display_set_backlight(false);
    }
}

static void wake_cb(lv_event_t *event)
{
    (void)event;
    portENTER_CRITICAL(&s_state_lock);
    bool backlight = s_backlight;
    portEXIT_CRITICAL(&s_state_lock);
    if (!backlight) bsp_display_set_backlight(true);
}

static void wake_overlay_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        /* This object is above every control while dark, so the wake gesture
         * cannot also activate the button that happens to be underneath it. */
        bsp_display_set_backlight(true);
    }
}

static void diagnostics_cb(lv_event_t *event)
{
    (void)event;
    ui_state_t state;
    portENTER_CRITICAL(&s_state_lock);
    state = s_state;
    portEXIT_CRITICAL(&s_state_lock);

    const esp_app_desc_t *app = esp_app_get_description();
    UBaseType_t stack_free = s_lvgl_task ? uxTaskGetStackHighWaterMark(s_lvgl_task) : 0;
    char details[640];
    snprintf(details, sizeof(details),
             "Board        " UI_BOARD_NAME "\n"
             "Display      1024 x 600 RGB565\n"
             "Touch        %s (this tap was received)\n"
             "SD storage   %s\n"
             "Wi-Fi        %s\n"
             "AirSense 11  %s\n"
             "PSRAM free   %u KiB\n"
             "Internal RAM %u KiB\n"
             "UI stack min %u bytes free\n"
             "RGB frames   %lu (%lu sync timeouts)\n"
             "Touch errors %lu\n"
             "Firmware     %s",
             UI_TOUCH_STATUS,
             state.sd_ready ? "ready" : "not ready",
             state.wifi ? "connected" : "offline",
             state.paired ? "paired" : "not paired",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)stack_free,
             (unsigned long)s_flush_count,
             (unsigned long)s_flush_timeouts,
             (unsigned long)s_touch_read_errors,
             app ? app->version : "unknown");

    lv_obj_t *message = lv_msgbox_create(NULL, "Hardware diagnostics",
                                         details, NULL, true);
    lv_obj_set_width(message, 720);
    lv_obj_set_style_bg_color(message, lv_color_hex(0x121d32), 0);
    lv_obj_set_style_text_color(message, lv_color_hex(0xe7edf7), 0);
    lv_obj_center(message);
}

static void build_ui(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x08111f), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xe7edf7), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, wake_cb, LV_EVENT_PRESSED, NULL);

    s_title_label = lv_label_create(screen);
    lv_label_set_text(s_title_label, "SomnoTrace");
    lv_obj_set_pos(s_title_label, 26, 18);
    lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_title_label, lv_color_hex(0xf4f7fb), 0);
    lv_obj_add_flag(s_title_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_title_label, diagnostics_cb, LV_EVENT_CLICKED, NULL);

    s_status_label = lv_label_create(screen);
    lv_label_set_text(s_status_label, "Starting hardware...");
    lv_obj_set_pos(s_status_label, 220, 27);
    lv_obj_set_width(s_status_label, 390);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x91a3bd), 0);

    s_sd_label = lv_label_create(screen);
    lv_label_set_text(s_sd_label, "SD --");
    lv_obj_set_pos(s_sd_label, 620, 27);
    lv_obj_set_style_text_font(s_sd_label, &lv_font_montserrat_14, 0);

    s_wifi_label = lv_label_create(screen);
    lv_label_set_text(s_wifi_label, "Wi-Fi --");
    lv_obj_set_pos(s_wifi_label, 690, 27);
    lv_obj_set_style_text_font(s_wifi_label, &lv_font_montserrat_14, 0);

    s_ble_label = lv_label_create(screen);
    lv_label_set_text(s_ble_label, "AirSense --");
    lv_obj_set_pos(s_ble_label, 780, 27);
    lv_obj_set_style_text_font(s_ble_label, &lv_font_montserrat_14, 0);

    s_clock_label = lv_label_create(screen);
    lv_label_set_text(s_clock_label, "--:--");
    lv_obj_align(s_clock_label, LV_ALIGN_TOP_RIGHT, -24, 24);
    lv_obj_set_style_text_font(s_clock_label, &lv_font_montserrat_18, 0);

    s_tabview = lv_tabview_create(screen, LV_DIR_BOTTOM, 64);
    lv_obj_set_pos(s_tabview, 0, 60);
    lv_obj_set_size(s_tabview, 1024, 540);
    lv_obj_set_style_bg_color(s_tabview, lv_color_hex(0x08111f), 0);
    lv_obj_set_style_border_width(s_tabview, 0, 0);
    lv_obj_set_style_pad_all(s_tabview, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_tabview, tabview_cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* Navigation is button-driven. Disabling swipe prevents a graph gesture
     * from accidentally changing pages. */
    lv_obj_clear_flag(lv_tabview_get_content(s_tabview), LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *tab_buttons = lv_tabview_get_tab_btns(s_tabview);
    lv_obj_set_style_bg_color(tab_buttons, lv_color_hex(0x0d1829), 0);
    lv_obj_set_style_text_font(tab_buttons, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(tab_buttons, lv_color_hex(0x91a3bd), 0);
    lv_obj_set_style_text_color(tab_buttons, lv_color_hex(0x43d7e8), LV_STATE_CHECKED);
    lv_obj_set_style_border_color(tab_buttons, lv_color_hex(0x43d7e8),
                                  LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(tab_buttons, 3,
                                  LV_PART_ITEMS | LV_STATE_CHECKED);

    s_pages[0] = lv_tabview_add_tab(s_tabview, "Live");
    s_pages[1] = lv_tabview_add_tab(s_tabview, "History");
    s_pages[2] = lv_tabview_add_tab(s_tabview, "Devices");
    s_pages[3] = lv_tabview_add_tab(s_tabview, "Settings");
    s_pages[4] = lv_tabview_add_tab(s_tabview, "System");
    for (int i = 0; i < 5; ++i) {
        lv_obj_set_style_bg_color(s_pages[i], lv_color_hex(0x08111f), 0);
        lv_obj_set_style_bg_opa(s_pages[i], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(s_pages[i], 0, 0);
        lv_obj_clear_flag(s_pages[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_t *live = s_pages[0];
    lv_obj_t *graph_card = make_card(live, 16, 8, 680, 318);
    lv_obj_t *graph_title = lv_label_create(graph_card);
    lv_label_set_text(graph_title, "LIVE BREATHING FLOW");
    lv_obj_set_style_text_font(graph_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(graph_title, lv_color_hex(0x8ea0ba), 0);

    s_chart = lv_chart_create(graph_card);
    lv_obj_set_pos(s_chart, 0, 34);
    lv_obj_set_size(s_chart, 648, 250);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, FLOW_POINTS);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, -1000, 1000);
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(0x0b1424), 0);
    lv_obj_set_style_line_color(s_chart, lv_color_hex(0x24344f), LV_PART_MAIN);
    lv_obj_set_style_line_width(s_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_size(s_chart, 0, LV_PART_INDICATOR);
    s_flow_series = lv_chart_add_series(s_chart, lv_color_hex(0x43d7e8),
                                        LV_CHART_AXIS_PRIMARY_Y);

    lv_obj_t *metrics = make_card(live, 712, 8, 296, 318);
    lv_obj_t *metrics_title = lv_label_create(metrics);
    lv_label_set_text(metrics_title, "THERAPY NOW");
    lv_obj_set_style_text_font(metrics_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(metrics_title, lv_color_hex(0x8ea0ba), 0);

    s_therapy_label = lv_label_create(metrics);
    lv_label_set_text(s_therapy_label, "Standby");
    lv_obj_set_pos(s_therapy_label, 0, 38);
    lv_obj_set_style_text_font(s_therapy_label, &lv_font_montserrat_36, 0);

    lv_obj_t *pressure_caption = lv_label_create(metrics);
    lv_label_set_text(pressure_caption, "PRESSURE");
    lv_obj_set_pos(pressure_caption, 0, 110);
    lv_obj_set_style_text_font(pressure_caption, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(pressure_caption, lv_color_hex(0x8ea0ba), 0);
    s_pressure_label = lv_label_create(metrics);
    lv_label_set_text(s_pressure_label, "-- cmH2O");
    lv_obj_set_pos(s_pressure_label, 0, 136);
    lv_obj_set_style_text_font(s_pressure_label, &lv_font_montserrat_18, 0);

    lv_obj_t *leak_caption = lv_label_create(metrics);
    lv_label_set_text(leak_caption, "LEAK");
    lv_obj_set_pos(leak_caption, 122, 110);
    lv_obj_set_style_text_font(leak_caption, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(leak_caption, lv_color_hex(0x8ea0ba), 0);
    s_leak_label = lv_label_create(metrics);
    lv_label_set_text(s_leak_label, "-- L/min");
    lv_obj_set_pos(s_leak_label, 122, 136);
    lv_obj_set_style_text_font(s_leak_label, &lv_font_montserrat_18, 0);

    lv_obj_t *resp_caption = lv_label_create(metrics);
    lv_label_set_text(resp_caption, "RESP RATE");
    lv_obj_set_pos(resp_caption, 0, 205);
    lv_obj_set_style_text_font(resp_caption, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(resp_caption, lv_color_hex(0x8ea0ba), 0);
    s_resp_label = lv_label_create(metrics);
    lv_label_set_text(s_resp_label, "-- /min");
    lv_obj_set_pos(s_resp_label, 0, 231);
    lv_obj_set_style_text_font(s_resp_label, &lv_font_montserrat_18, 0);

    lv_obj_t *flow_lim_caption = lv_label_create(metrics);
    lv_label_set_text(flow_lim_caption, "FLOW LIMITATION");
    lv_obj_set_pos(flow_lim_caption, 0, 266);
    lv_obj_set_style_text_font(flow_lim_caption, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(flow_lim_caption, lv_color_hex(0x8ea0ba), 0);
    s_flow_lim_label = lv_label_create(metrics);
    lv_label_set_text(s_flow_lim_label, "--");
    lv_obj_set_pos(s_flow_lim_label, 152, 264);
    lv_obj_set_style_text_font(s_flow_lim_label, &lv_font_montserrat_18, 0);

    lv_obj_t *runtime_caption = lv_label_create(metrics);
    lv_label_set_text(runtime_caption, "RUNTIME");
    lv_obj_set_pos(runtime_caption, 122, 205);
    lv_obj_set_style_text_font(runtime_caption, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(runtime_caption, lv_color_hex(0x8ea0ba), 0);
    s_runtime_label = lv_label_create(metrics);
    lv_label_set_text(s_runtime_label, "00:00:00");
    lv_obj_set_pos(s_runtime_label, 122, 231);
    lv_obj_set_style_text_font(s_runtime_label, &lv_font_montserrat_18, 0);

    s_therapy_button = make_touch_button(live, 16, 340, 238, 74,
                                         "Start therapy", 0x087f8c,
                                         action_cb, 1);
    s_therapy_button_label = lv_obj_get_child(s_therapy_button, 0);
    make_touch_button(live, 266, 340, 238, 74, "Acknowledge", 0x365083, action_cb, 2);
    make_touch_button(live, 516, 340, 238, 74, "Wi-Fi setup", 0x6652a3, action_cb, 3);
    make_touch_button(live, 766, 340, 242, 74, "Screen off", 0x29364b, action_cb, 4);

    s_notice_label = lv_label_create(live);
    lv_obj_set_pos(s_notice_label, 20, 424);
    lv_obj_set_width(s_notice_label, 976);
    lv_obj_set_style_text_font(s_notice_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_notice_label, lv_color_hex(0xffbd59), 0);
    lv_obj_add_flag(s_notice_label, LV_OBJ_FLAG_HIDDEN);

    /* History: a scrollable night list plus a large at-a-glance summary. */
    lv_obj_t *history_list = make_card(s_pages[1], 16, 12, 368, 440);
    lv_obj_add_flag(history_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(history_list, LV_DIR_VER);
    make_section_title(history_list, "Recorded nights", 0, 0);
    s_history_status = lv_label_create(history_list);
    lv_label_set_text(s_history_status, "Tap Refresh to read the microSD card");
    lv_obj_set_pos(s_history_status, 0, 38);
    lv_obj_set_width(s_history_status, 330);
    lv_label_set_long_mode(s_history_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_history_status, lv_color_hex(0x91a3bd), 0);
    for (int i = 0; i < HISTORY_MAX_DAYS; ++i) {
        s_history_rows[i] = make_touch_button(history_list, 0, 72 + i * 58,
                                              330, 50, "", 0x1b2a42,
                                              history_row_cb, i);
        s_history_row_labels[i] = lv_obj_get_child(s_history_rows[i], 0);
        lv_obj_add_flag(s_history_rows[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t *history_card = make_card(s_pages[1], 400, 12, 608, 440);
    make_section_title(history_card, "Night summary", 0, 0);
    s_history_detail = lv_label_create(history_card);
    lv_label_set_text(s_history_detail,
                      "Select a recorded night to see usage, AHI, pressure and leak.\n\n"
                      "History stays on the microSD card; no cloud connection is required.");
    lv_obj_set_pos(s_history_detail, 0, 52);
    lv_obj_set_width(s_history_detail, 568);
    lv_label_set_long_mode(s_history_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_history_detail, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_line_space(s_history_detail, 14, 0);
    make_touch_button(history_card, 350, 342, 218, 64, "Refresh history",
                      0x365083, refresh_cb, 0);

    /* Devices: both supported BLE families can be paired without a phone. */
    lv_obj_t *as_card = make_card(s_pages[2], 16, 12, 484, 440);
    make_section_title(as_card, "AirSense 11", 0, 0);
    s_as11_status = lv_label_create(as_card);
    lv_label_set_text(s_as11_status, "Status: idle");
    lv_obj_set_pos(s_as11_status, 0, 42);
    lv_obj_set_width(s_as11_status, 445);
    lv_label_set_long_mode(s_as11_status, LV_LABEL_LONG_DOT);
    s_as11_dropdown = lv_dropdown_create(as_card);
    lv_dropdown_set_options(s_as11_dropdown, "No scan results");
    lv_obj_set_pos(s_as11_dropdown, 0, 84);
    lv_obj_set_size(s_as11_dropdown, 445, 56);
    lv_obj_set_style_text_font(s_as11_dropdown, &lv_font_montserrat_18, 0);
    make_touch_button(as_card, 0, 156, 138, 62, "Scan", 0x365083, scan_cb, 0);
    s_as11_pair_button = make_touch_button(as_card, 152, 156, 138, 62,
                                           "Pair", 0x087f8c,
                                           device_action_cb, DEVICE_PAIR_AS11);
    make_touch_button(as_card, 304, 156, 141, 62, "Forget", 0x6a3544,
                      forget_prompt_cb, DEVICE_FORGET_AS11);
    lv_obj_t *passkey_label = lv_label_create(as_card);
    lv_label_set_text(passkey_label, "4-digit code shown by AirSense");
    lv_obj_set_pos(passkey_label, 0, 244);
    lv_obj_set_style_text_color(passkey_label, lv_color_hex(0x91a3bd), 0);
    s_passkey = lv_textarea_create(as_card);
    lv_textarea_set_one_line(s_passkey, true);
    lv_textarea_set_max_length(s_passkey, 4);
    lv_textarea_set_accepted_chars(s_passkey, "0123456789");
    lv_textarea_set_placeholder_text(s_passkey, "Passkey");
    lv_obj_set_pos(s_passkey, 0, 278);
    lv_obj_set_size(s_passkey, 245, 62);
    lv_obj_set_style_text_font(s_passkey, &lv_font_montserrat_22, 0);
    lv_obj_add_event_cb(s_passkey, passkey_focus_cb, LV_EVENT_FOCUSED, NULL);
    s_passkey_confirm_button = make_touch_button(as_card, 261, 278, 184, 62,
                                                  "Confirm code", 0x6652a3,
                                                  device_action_cb,
                                                  DEVICE_CONFIRM_AS11);
    lv_obj_add_state(s_passkey, LV_STATE_DISABLED);
    lv_obj_add_state(s_passkey_confirm_button, LV_STATE_DISABLED);

    lv_obj_t *ox_card = make_card(s_pages[2], 516, 12, 492, 440);
    make_section_title(ox_card, "O2 ring", 0, 0);
    s_ox_status = lv_label_create(ox_card);
    lv_label_set_text(s_ox_status, "Status: idle");
    lv_obj_set_pos(s_ox_status, 0, 42);
    lv_obj_set_width(s_ox_status, 450);
    s_ox_dropdown = lv_dropdown_create(ox_card);
    lv_dropdown_set_options(s_ox_dropdown, "No scan results");
    lv_obj_set_pos(s_ox_dropdown, 0, 84);
    lv_obj_set_size(s_ox_dropdown, 452, 56);
    lv_obj_set_style_text_font(s_ox_dropdown, &lv_font_montserrat_18, 0);
    make_touch_button(ox_card, 0, 156, 140, 62, "Scan", 0x365083, scan_cb, 1);
    make_touch_button(ox_card, 156, 156, 140, 62, "Pair", 0x087f8c,
                      device_action_cb, DEVICE_PAIR_OX);
    make_touch_button(ox_card, 312, 156, 140, 62, "Forget", 0x6a3544,
                      forget_prompt_cb, DEVICE_FORGET_OX);
    lv_obj_t *ox_help = lv_label_create(ox_card);
    lv_label_set_text(ox_help,
                      "Supports Wellue O2 Ring S, SleepHQ O2 Ring Pro and legacy ViaTom rings.\n\n"
                      "Pairing identifies the protocol automatically from the scan result.");
    lv_obj_set_pos(ox_help, 0, 250);
    lv_obj_set_width(ox_help, 452);
    lv_label_set_long_mode(ox_help, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(ox_help, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_line_space(ox_help, 10, 0);
    lv_obj_set_style_text_color(ox_help, lv_color_hex(0x91a3bd), 0);

    /* Settings: only relevant controls are shown for this mains-powered board. */
    lv_obj_t *display_card = make_card(s_pages[3], 16, 12, 484, 440);
    make_section_title(display_card, "Display", 0, 0);
    lv_obj_t *brightness_caption = lv_label_create(display_card);
    lv_label_set_text(brightness_caption, "Brightness");
    lv_obj_set_pos(brightness_caption, 0, 64);
    lv_obj_set_style_text_font(brightness_caption, &lv_font_montserrat_18, 0);
    s_settings_brightness_value = lv_label_create(display_card);
    lv_obj_set_pos(s_settings_brightness_value, 354, 64);
    lv_obj_set_style_text_font(s_settings_brightness_value, &lv_font_montserrat_18, 0);
    s_settings_brightness = lv_slider_create(display_card);
    lv_obj_set_pos(s_settings_brightness, 0, 108);
    lv_obj_set_size(s_settings_brightness, 445, 24);
    lv_slider_set_range(s_settings_brightness, 1, 200);
    lv_slider_set_value(s_settings_brightness, device_settings_get()->brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_settings_brightness, brightness_cb, LV_EVENT_ALL, NULL);
    lv_label_set_text_fmt(s_settings_brightness_value, "%d%%",
                          (device_settings_get()->brightness + 1) / 2);
    lv_obj_t *therapy_caption = lv_label_create(display_card);
    lv_label_set_text(therapy_caption, "During therapy");
    lv_obj_set_pos(therapy_caption, 0, 174);
    lv_obj_set_style_text_font(therapy_caption, &lv_font_montserrat_18, 0);
    s_settings_therapy_mode = lv_dropdown_create(display_card);
    lv_dropdown_set_options(s_settings_therapy_mode,
                            "Live dashboard\nInfo dashboard\nTurn off during therapy\nAlways off");
    lcd_therapy_mode_t mode = device_settings_get()->lcd_therapy_mode;
    uint16_t mode_index = mode == LCD_THERAPY_INFO ? 1 :
                          mode == LCD_THERAPY_OFF ? 2 :
                          mode == LCD_THERAPY_ALWAYS_OFF ? 3 : 0;
    lv_dropdown_set_selected(s_settings_therapy_mode, mode_index);
    lv_obj_set_pos(s_settings_therapy_mode, 0, 212);
    lv_obj_set_size(s_settings_therapy_mode, 445, 58);
    lv_obj_set_style_text_font(s_settings_therapy_mode, &lv_font_montserrat_18, 0);
    lv_obj_add_event_cb(s_settings_therapy_mode, therapy_mode_cb, LV_EVENT_VALUE_CHANGED, NULL);
    make_touch_button(display_card, 0, 326, 214, 64, "Screen off", 0x29364b,
                      action_cb, 4);
    make_touch_button(display_card, 231, 326, 214, 64, "Diagnostics", 0x365083,
                      diagnostics_cb, 0);

    lv_obj_t *network_card = make_card(s_pages[3], 516, 12, 492, 440);
    make_section_title(network_card, "Network", 0, 0);
    s_network_status = lv_label_create(network_card);
    lv_label_set_text(s_network_status, "Checking network...");
    lv_obj_set_pos(s_network_status, 0, 58);
    lv_obj_set_width(s_network_status, 452);
    lv_label_set_long_mode(s_network_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_network_status, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_line_space(s_network_status, 10, 0);
    lv_obj_t *ssid_caption = lv_label_create(network_card);
    lv_label_set_text(ssid_caption, "Network name (SSID)");
    lv_obj_set_pos(ssid_caption, 0, 142);
    lv_obj_set_style_text_color(ssid_caption, lv_color_hex(0x91a3bd), 0);
    s_wifi_ssid = lv_textarea_create(network_card);
    lv_textarea_set_one_line(s_wifi_ssid, true);
    lv_textarea_set_max_length(s_wifi_ssid, NETPROV_SSID_MAXLEN);
    lv_textarea_set_placeholder_text(s_wifi_ssid, "Wi-Fi name");
    lv_obj_set_pos(s_wifi_ssid, 0, 168);
    lv_obj_set_size(s_wifi_ssid, 452, 54);
    lv_obj_set_style_text_font(s_wifi_ssid, &lv_font_montserrat_18, 0);
    lv_obj_add_event_cb(s_wifi_ssid, text_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_t *password_caption = lv_label_create(network_card);
    lv_label_set_text(password_caption, "Password (blank keeps it for the same network)");
    lv_obj_set_pos(password_caption, 0, 236);
    lv_obj_set_style_text_color(password_caption, lv_color_hex(0x91a3bd), 0);
    s_wifi_password = lv_textarea_create(network_card);
    lv_textarea_set_one_line(s_wifi_password, true);
    lv_textarea_set_password_mode(s_wifi_password, true);
    lv_textarea_set_max_length(s_wifi_password, NETPROV_PASS_MAXLEN);
    lv_textarea_set_placeholder_text(s_wifi_password, "Wi-Fi password");
    lv_obj_set_pos(s_wifi_password, 0, 262);
    lv_obj_set_size(s_wifi_password, 452, 54);
    lv_obj_set_style_text_font(s_wifi_password, &lv_font_montserrat_18, 0);
    lv_obj_add_event_cb(s_wifi_password, text_focus_cb, LV_EVENT_FOCUSED, NULL);
    make_touch_button(network_card, 0, 332, 218, 62, "Save & restart", 0x087f8c,
                      wifi_save_cb, 0);
    make_touch_button(network_card, 234, 332, 218, 62, "Setup hotspot", 0x6652a3,
                      action_cb, 3);

    lv_obj_t *system_card = make_card(s_pages[4], 16, 12, 680, 440);
    make_section_title(system_card, "System health", 0, 0);
    s_system_details = lv_label_create(system_card);
    lv_label_set_text(s_system_details, "Collecting diagnostics...");
    lv_obj_set_pos(s_system_details, 0, 50);
    lv_obj_set_width(s_system_details, 640);
    lv_label_set_long_mode(s_system_details, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_system_details, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_line_space(s_system_details, 8, 0);
    lv_obj_t *system_actions = make_card(s_pages[4], 712, 12, 296, 440);
    make_section_title(system_actions, "Controls", 0, 0);
    make_touch_button(system_actions, 0, 62, 256, 72, "Restart", 0x6a3544,
                      reboot_cb, 0);
    make_touch_button(system_actions, 0, 152, 256, 72, "Wi-Fi setup", 0x6652a3,
                      action_cb, 3);
    make_touch_button(system_actions, 0, 242, 256, 72, "Screen off", 0x29364b,
                      action_cb, 4);
    make_touch_button(system_actions, 0, 332, 256, 72, "Details", 0x365083,
                      diagnostics_cb, 0);

    s_attention_card = make_card(screen, 132, 142, 760, 250);
    lv_obj_set_style_bg_color(s_attention_card, lv_color_hex(0x172640), 0);
    lv_obj_set_style_bg_opa(s_attention_card, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_attention_card, lv_color_hex(0x43d7e8), 0);
    lv_obj_set_style_border_width(s_attention_card, 2, 0);
    s_attention_title = lv_label_create(s_attention_card);
    lv_obj_set_pos(s_attention_title, 18, 18);
    lv_obj_set_style_text_font(s_attention_title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_attention_title, lv_color_hex(0x43d7e8), 0);
    s_attention_body = lv_label_create(s_attention_card);
    lv_obj_set_pos(s_attention_body, 18, 70);
    lv_obj_set_width(s_attention_body, 690);
    lv_label_set_long_mode(s_attention_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_attention_body, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_line_space(s_attention_body, 9, 0);
    lv_obj_add_flag(s_attention_card, LV_OBJ_FLAG_HIDDEN);

    s_keyboard = lv_keyboard_create(screen);
    lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_set_pos(s_keyboard, 512, 176);
    lv_obj_set_size(s_keyboard, 480, 306);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(0x172640), 0);
    lv_obj_add_event_cb(s_keyboard, keyboard_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

    s_wake_overlay = lv_obj_create(screen);
    lv_obj_set_pos(s_wake_overlay, 0, 0);
    lv_obj_set_size(s_wake_overlay, WAVESHARE_7B_H_RES, WAVESHARE_7B_V_RES);
    lv_obj_set_style_bg_color(s_wake_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_wake_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_wake_overlay, 0, 0);
    lv_obj_clear_flag(s_wake_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wake_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wake_overlay, wake_overlay_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_flag(s_wake_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_history_widgets(void)
{
    ui_service_state_t services;
    portENTER_CRITICAL(&s_state_lock);
    services = s_services;
    portEXIT_CRITICAL(&s_state_lock);

    if (services.history_version != s_seen_history_version) {
        s_seen_history_version = services.history_version;
        if (services.history_count == 0) {
            if (services.history_result == ESP_ERR_INVALID_STATE) {
                lv_label_set_text(s_history_status, "History is unavailable while recording");
            } else if (services.history_result == ESP_ERR_TIMEOUT) {
                lv_label_set_text(s_history_status, "microSD is busy; try Refresh again");
            } else if (services.history_result == ESP_ERR_NOT_FOUND) {
                lv_label_set_text(s_history_status, "microSD history is unavailable");
            } else {
                lv_label_set_text(s_history_status, "No completed sessions found");
            }
            s_history_selection = -1;
            lv_label_set_text(s_history_detail,
                              services.history_result == ESP_OK
                              ? "No completed sessions are stored on this card."
                              : "History could not be read. Recording and live therapy remain available.");
        } else {
            lv_label_set_text_fmt(s_history_status, "%u recent night%s",
                                  (unsigned)services.history_count,
                                  services.history_count == 1 ? "" : "s");
            if (s_history_selection < 0 ||
                s_history_selection >= (int)services.history_count)
                s_history_selection = 0;
        }
        for (int i = 0; i < HISTORY_MAX_DAYS; ++i) {
            if (i < (int)services.history_count) {
                const char *d = services.history[i].day;
                lv_label_set_text_fmt(s_history_row_labels[i],
                                      "%.4s-%.2s-%.2s     %d session%s",
                                      d, d + 4, d + 6, services.history[i].sessions,
                                      services.history[i].sessions == 1 ? "" : "s");
                lv_obj_clear_flag(s_history_rows[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_history_rows[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (services.history_busy) {
        lv_label_set_text(s_history_status, "Reading microSD history...");
    } else if (s_history_selection >= 0 &&
               s_history_selection < (int)services.history_count) {
        touch_history_day_t *day = &services.history[s_history_selection];
        if (day->has_summary) {
            char usage[32];
            char ahi[32];
            char pressure[32];
            char leak[32];
            if (day->has_usage) snprintf(usage, sizeof(usage), "%d h %02d min",
                                         day->usage_min / 60, day->usage_min % 60);
            else strlcpy(usage, "Unavailable", sizeof(usage));
            if (day->has_ahi) snprintf(ahi, sizeof(ahi), "%.1f / hour", day->ahi);
            else strlcpy(ahi, "Unavailable", sizeof(ahi));
            if (day->has_pressure_p95)
                snprintf(pressure, sizeof(pressure), "%.1f cmH2O", day->pressure_p95);
            else strlcpy(pressure, "Unavailable", sizeof(pressure));
            if (day->has_leak_p95)
                snprintf(leak, sizeof(leak), "%.1f L/min", day->leak_p95);
            else strlcpy(leak, "Unavailable", sizeof(leak));
            lv_label_set_text_fmt(s_history_detail,
                                  "Night of %.4s-%.2s-%.2s\n\n"
                                  "Usage                 %s\n"
                                  "Sessions              %d\n"
                                  "Device-reported AHI   %s\n"
                                  "95%% pressure          %s\n"
                                  "95%% leak              %s\n\n"
                                  "For trend review; not a diagnosis or prescription.",
                                  day->day, day->day + 4, day->day + 6,
                                  usage, day->sessions, ahi, pressure, leak);
        } else {
            lv_label_set_text_fmt(s_history_detail,
                                  "Night of %.4s-%.2s-%.2s\n\n%d session%s recorded\n\n"
                                  "Summary metrics are not available for this night yet.",
                                  day->day, day->day + 4, day->day + 6,
                                  day->sessions, day->sessions == 1 ? "" : "s");
        }
    }

    for (int i = 0; i < HISTORY_MAX_DAYS; ++i) {
        lv_obj_set_style_bg_color(s_history_rows[i],
                                  lv_color_hex(i == s_history_selection
                                               ? 0x365083 : 0x1b2a42), 0);
    }
}

static void refresh_device_dropdown(bool oxygen, const ui_service_state_t *services)
{
    unsigned version = oxygen ? services->ox_version : services->as11_version;
    unsigned *seen = oxygen ? &s_seen_ox_version : &s_seen_as11_version;
    if (*seen == version) return;
    *seen = version;
    const ui_device_result_t *items = oxygen ? services->ox : services->as11;
    size_t count = oxygen ? services->ox_count : services->as11_count;
    lv_obj_t *dropdown = oxygen ? s_ox_dropdown : s_as11_dropdown;
    char options[700] = {0};
    if (count == 0) {
        strlcpy(options, "No devices found", sizeof(options));
    } else {
        for (size_t i = 0; i < count; ++i) {
            size_t used = strlen(options);
            snprintf(options + used, sizeof(options) - used, "%s%s (%d dBm)",
                     i ? "\n" : "", items[i].name, items[i].rssi);
        }
    }
    lv_dropdown_set_options(dropdown, options);
}

static void refresh_secondary_pages(const ui_state_t *state, int active_tab)
{
    ui_service_state_t services;
    ble_ui_operation_t ble_operation;
    int64_t ble_started;
    portENTER_CRITICAL(&s_state_lock);
    services = s_services;
    ble_operation = s_ble_operation;
    ble_started = s_ble_operation_started_us;
    portEXIT_CRITICAL(&s_state_lock);
    if (active_tab == 2) {
        refresh_device_dropdown(false, &services);
        refresh_device_dropdown(true, &services);
    }

    if (!s_touch_services_ready) {
        lv_label_set_text(s_as11_status, "Status: starting BLE service...");
        lv_label_set_text(s_ox_status, "Status: starting BLE service...");
        lv_label_set_text(s_network_status, "Starting network service...");
        const esp_app_desc_t *boot_app = esp_app_get_description();
        lv_label_set_text_fmt(s_system_details,
                              "Board                 " UI_BOARD_NAME "\n"
                              "Display               1024 x 600 RGB565\n"
                              "Touch                 %s\n"
                              "microSD               %s\n"
                              "Services              starting\n"
                              "Firmware              %s",
                              UI_TOUCH_STATUS,
                              state->sd_ready ? "ready" : "not ready",
                              boot_app ? boot_app->version : "unknown");
        return;
    }

    if (!s_settings_synced && active_tab == 3) {
        const device_settings_t *settings = device_settings_get();
        lv_slider_set_value(s_settings_brightness, settings->brightness, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_settings_brightness_value, "%d%%",
                              (settings->brightness + 1) / 2);
        uint16_t mode_index = settings->lcd_therapy_mode == LCD_THERAPY_INFO ? 1 :
                              settings->lcd_therapy_mode == LCD_THERAPY_OFF ? 2 :
                              settings->lcd_therapy_mode == LCD_THERAPY_ALWAYS_OFF ? 3 : 0;
        lv_dropdown_set_selected(s_settings_therapy_mode, mode_index);
        struct netprov_config cfg = {0};
        if (netprov_load_config(&cfg)) {
            lv_textarea_set_text(s_wifi_ssid, cfg.wifi[0].ssid);
            strlcpy(s_saved_wifi_ssid, cfg.wifi[0].ssid,
                    sizeof(s_saved_wifi_ssid));
            /* Never copy a stored credential into an LVGL object. Blank means
             * unchanged when the SSID is unchanged. */
            lv_textarea_set_text(s_wifi_password, "");
        }
        s_settings_synced = true;
    }

#if CONFIG_SOMNOTRACE_BOARD_QEMU
    const char *as_status = "simulated preview";
    const char *ox_status = "simulated preview";
#else
    const char *as_status = s_as11_service_ready ? as11_ble_get_status() : "unavailable";
    const char *ox_status = s_ox_service_ready ? oximeter_get_status() : "unavailable";
#endif
    if (ble_started && esp_timer_get_time() - ble_started > 1000000) {
        bool as_done = ble_operation == BLE_UI_PAIR_AS11 &&
                       (!strcmp(as_status, AS11_STATUS_PAIRED) ||
                        !strcmp(as_status, AS11_STATUS_ERROR));
        bool ox_done = ble_operation == BLE_UI_PAIR_OX &&
                       (!strcmp(ox_status, OX_STATUS_PAIRED) ||
                        !strcmp(ox_status, OX_STATUS_MONITORING) ||
                        !strcmp(ox_status, OX_STATUS_ERROR));
        if (as_done || ox_done) end_ble_operation();
    }
    lv_label_set_text_fmt(s_as11_status, "Status: %s%s",
                          services.as11_busy ? "scanning" : as_status,
                          s_as11_service_ready && as11_ble_is_paired()
                          ? "  -  paired" : "");
    lv_label_set_text_fmt(s_ox_status, "Status: %s%s",
                          services.ox_busy ? "scanning" : ox_status,
                          s_ox_service_ready && oximeter_is_paired()
                          ? "  -  paired" : "");
    bool waiting_passkey = !strcmp(as_status, AS11_STATUS_WAIT_PASSKEY);
    if (waiting_passkey) {
        lv_obj_set_style_border_color(s_passkey, lv_color_hex(0x43d7e8), 0);
        lv_obj_set_style_border_width(s_passkey, 2, 0);
    } else {
        lv_obj_set_style_border_width(s_passkey, 1, 0);
    }
    if (waiting_passkey) {
        lv_obj_clear_state(s_passkey, LV_STATE_DISABLED);
        lv_obj_clear_state(s_passkey_confirm_button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_passkey, LV_STATE_DISABLED);
        lv_obj_add_state(s_passkey_confirm_button, LV_STATE_DISABLED);
    }
    if (ble_operation == BLE_UI_IDLE)
        lv_obj_clear_state(s_as11_pair_button, LV_STATE_DISABLED);
    else
        lv_obj_add_state(s_as11_pair_button, LV_STATE_DISABLED);

#if CONFIG_SOMNOTRACE_BOARD_QEMU
    if (active_tab == 3) {
        lv_label_set_text(s_network_status,
                          "Simulated network connected\n"
                          "IP address: 192.0.2.10\n"
                          "Dashboard: http://somnotrace-qemu.local");
    }
#else
    netprov_link_t link;
    netprov_get_link(&link);
    const char *hostname = netprov_mdns_name_cached();
    if (!hostname || !hostname[0]) hostname = "somnotrace";
    if (link.up) {
        lv_label_set_text_fmt(s_network_status,
                              "Connected to %s\nIP address: %s\nDashboard: http://%s.local",
                              link.ssid, link.ip, hostname);
    } else {
        lv_label_set_text_fmt(s_network_status,
                              "Not connected\nSetup name: %s-setup\nDashboard: http://%s.local",
                              hostname, hostname);
    }
#endif

    const esp_app_desc_t *app = esp_app_get_description();
    UBaseType_t stack_free = s_lvgl_task ? uxTaskGetStackHighWaterMark(s_lvgl_task) : 0;
    lv_label_set_text_fmt(s_system_details,
                          "Board                 " UI_BOARD_NAME "\n"
                          "Display               1024 x 600 RGB565\n"
                          "Touch                 %s\n"
                          "microSD               %s\n"
                          "Wi-Fi                 %s\n"
                          "AirSense 11           %s\n"
                          "PSRAM free            %u KiB\n"
                          "Internal RAM free     %u KiB\n"
                          "UI stack minimum      %u bytes\n"
                          "Frames / timeouts     %lu / %lu\n"
                          "Touch read errors     %lu\n"
                          "Firmware              %s",
                          UI_TOUCH_STATUS,
                          state->sd_ready ? "ready" : "not ready",
                          state->wifi ? "connected" : "offline",
                          state->paired ? "paired" : "not paired",
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                          (unsigned)stack_free,
                          (unsigned long)s_flush_count,
                          (unsigned long)s_flush_timeouts,
                          (unsigned long)s_touch_read_errors,
                          app ? app->version : "unknown");
}

static void update_ui(void)
{
    static TickType_t last_text_update;
    static unsigned seen_flow_version;
    ui_state_t state;
    bool therapy_command_busy;
    bool therapy_command_target;
    bool backlight;
    portENTER_CRITICAL(&s_state_lock);
    if (s_state.notice_expires_us > 0 &&
        esp_timer_get_time() >= s_state.notice_expires_us) {
        s_state.notice[0] = '\0';
        s_state.notice_expires_us = 0;
        s_state.notice_critical = false;
    }
    state = s_state;
    therapy_command_busy = s_therapy_command_busy;
    therapy_command_target = s_therapy_command_target;
    backlight = s_backlight;
    portEXIT_CRITICAL(&s_state_lock);

#if CONFIG_SOMNOTRACE_BOARD_QEMU
    uint8_t requested_tab;
    portENTER_CRITICAL(&s_state_lock);
    requested_tab = s_qemu_requested_tab;
    s_qemu_requested_tab = UINT8_MAX;
    portEXIT_CRITICAL(&s_state_lock);
    if (requested_tab < 5 && s_tabview)
        lv_tabview_set_act(s_tabview, requested_tab, LV_ANIM_OFF);
#endif

    /* Keep LVGL responsive for the wake overlay but avoid chart/label churn
     * while the panel is intentionally dark. */
    if (!backlight) return;

    int active_tab = s_tabview ? lv_tabview_get_tab_act(s_tabview) : 0;
    if (active_tab == 0 && state.flow_version != seen_flow_version) {
        seen_flow_version = state.flow_version;
        for (unsigned i = 0; i < FLOW_POINTS; ++i) {
            unsigned source = (state.flow_head + i) % FLOW_POINTS;
            s_flow_series->y_points[i] = state.flow[source];
        }
        lv_chart_refresh(s_chart);
    }
    if (active_tab == 1) refresh_history_widgets();

    TickType_t now_ticks = xTaskGetTickCount();
    if (now_ticks - last_text_update < pdMS_TO_TICKS(500)) return;
    last_text_update = now_ticks;

    /* Keep the product title stable and use the centered attention card for
     * setup/warning/error headings. The title remains a predictable target
     * for opening diagnostics in every operating state. */
    lv_label_set_text(s_title_label, "SomnoTrace");
    lv_label_set_text(s_status_label, state.notice[0] ? state.notice : state.status);
    lv_obj_set_style_text_color(s_status_label,
                                lv_color_hex(state.notice[0] ? 0xffbd59 : 0x91a3bd), 0);
    lv_label_set_text(s_sd_label, state.sd_ready ? "SD OK" : "SD --");
    lv_obj_set_style_text_color(s_sd_label,
                                lv_color_hex(state.sd_ready ? 0x58d68d : 0xffbd59), 0);
    lv_label_set_text(s_wifi_label, state.wifi ? "Wi-Fi OK" : "Wi-Fi --");
    lv_obj_set_style_text_color(s_wifi_label,
                                lv_color_hex(state.wifi ? 0x58d68d : 0x8ea0ba), 0);
    lv_label_set_text(s_ble_label, state.paired ? "AirSense OK" : "AirSense --");
    lv_obj_set_style_text_color(s_ble_label,
                                lv_color_hex(state.paired ? 0x58d68d : 0x8ea0ba), 0);
    lv_label_set_text(s_therapy_label, state.therapy ? "Therapy" : "Standby");
    lv_obj_set_style_text_color(s_therapy_label,
                                lv_color_hex(state.therapy ? 0x43d7e8 : 0xe7edf7), 0);
    lv_label_set_text(s_therapy_button_label,
                      therapy_command_busy
                      ? (therapy_command_target ? "Starting..." : "Stopping...")
                      : (state.therapy ? "Stop therapy" : "Start therapy"));
    if (therapy_command_busy || !state.paired)
        lv_obj_add_state(s_therapy_button, LV_STATE_DISABLED);
    else
        lv_obj_clear_state(s_therapy_button, LV_STATE_DISABLED);
    if (isfinite(state.leak)) lv_label_set_text_fmt(s_leak_label, "%.1f L/min", state.leak);
    else lv_label_set_text(s_leak_label, "-- L/min");
    if (isfinite(state.pressure)) lv_label_set_text_fmt(s_pressure_label, "%.1f cmH2O", state.pressure);
    else lv_label_set_text(s_pressure_label, "-- cmH2O");
    if (isfinite(state.respiratory_rate)) lv_label_set_text_fmt(s_resp_label, "%.1f /min", state.respiratory_rate);
    else lv_label_set_text(s_resp_label, "-- /min");
    if (isfinite(state.flow_limitation)) lv_label_set_text_fmt(s_flow_lim_label, "%.2f", state.flow_limitation);
    else lv_label_set_text(s_flow_lim_label, "--");

    int64_t elapsed = 0;
    if (state.therapy && state.therapy_start_us != 0) {
        elapsed = (esp_timer_get_time() - state.therapy_start_us) / 1000000;
        if (elapsed < 0) elapsed = 0;
    }
    lv_label_set_text_fmt(s_runtime_label, "%02lld:%02lld:%02lld",
                          (long long)(elapsed / 3600),
                          (long long)((elapsed / 60) % 60),
                          (long long)(elapsed % 60));

    if (active_tab >= 2) refresh_secondary_pages(&state, active_tab);

    if (state.notice[0]) {
        lv_label_set_text(s_notice_label, state.notice);
        lv_obj_clear_flag(s_notice_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_notice_label, LV_OBJ_FLAG_HIDDEN);
    }

    bool attention = !state.therapy && state.attention[0] &&
                     strcmp(state.title, "SomnoTrace") != 0;
    if (attention) {
        lv_label_set_text(s_attention_title, state.title);
        lv_label_set_text(s_attention_body, state.attention);
        lv_obj_clear_flag(s_attention_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_attention_card);
    } else {
        lv_obj_add_flag(s_attention_card, LV_OBJ_FLAG_HIDDEN);
    }

    time_t now = time(NULL);
    struct tm local;
    if (now > 100000 && localtime_r(&now, &local)) {
        char clock[16];
        strftime(clock, sizeof(clock), "%H:%M", &local);
        lv_label_set_text(s_clock_label, clock);
    }
}

static void lvgl_task(void *arg)
{
    (void)arg;
    s_lvgl_task = xTaskGetCurrentTaskHandle();
    TickType_t last_update = 0;
    while (true) {
        if (lock_lvgl(portMAX_DELAY)) {
            TickType_t now = xTaskGetTickCount();
            if (now - last_update >= pdMS_TO_TICKS(UI_UPDATE_MS)) {
                update_ui();
                last_update = now;
            }
            uint32_t delay = lv_timer_handler();
            unlock_lvgl();
            if (delay < 5) delay = 5;
            if (delay > 50) delay = 50;
            vTaskDelay(pdMS_TO_TICKS(delay));
        }
    }
}

esp_err_t bsp_display_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    strcpy(s_state.title, "SomnoTrace");
    strcpy(s_state.status, "Initializing 7-inch dashboard...");
    s_state.leak = NAN;
    s_state.pressure = NAN;
    s_state.respiratory_rate = NAN;
    s_state.flow_limitation = NAN;

    ESP_RETURN_ON_ERROR(waveshare_7b_init(&s_panel, &s_touch), TAG,
                        "initialize Waveshare 7B");

    /* With an RGB bounce buffer, frame-buffer handoff completion is reported
     * by on_bounce_frame_finish. This matches Waveshare's 7B reference port
     * and prevents LVGL from drawing into a buffer still being scanned out. */
#if !CONFIG_SOMNOTRACE_BOARD_QEMU
    esp_lcd_rgb_panel_event_callbacks_t callbacks = {
        .on_bounce_frame_finish = on_vsync,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_register_event_callbacks(s_panel,
                                                                  &callbacks, NULL),
                        TAG, "register display VSYNC");
#endif

    lv_init();
    void *fb1 = NULL, *fb2 = NULL;
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_qemu_get_frame_buffer(s_panel, &fb1), TAG,
                        "get QEMU framebuffer");
#else
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &fb1, &fb2),
                        TAG, "get RGB framebuffers");
#endif
    static lv_disp_draw_buf_t draw_buffer;
    lv_disp_draw_buf_init(&draw_buffer, fb1, fb2,
                          WAVESHARE_7B_H_RES * WAVESHARE_7B_V_RES);
    static lv_disp_drv_t display_driver;
    lv_disp_drv_init(&display_driver);
    display_driver.hor_res = WAVESHARE_7B_H_RES;
    display_driver.ver_res = WAVESHARE_7B_V_RES;
    display_driver.flush_cb = flush_cb;
    display_driver.draw_buf = &draw_buffer;
    display_driver.user_data = s_panel;
    display_driver.full_refresh = 1;
    lv_disp_drv_register(&display_driver);

#if CONFIG_SOMNOTRACE_BOARD_QEMU
    const bool input_available = true;
#else
    const bool input_available = s_touch != NULL;
#endif
    if (input_available) {
        static lv_indev_drv_t touch_driver;
        lv_indev_drv_init(&touch_driver);
        touch_driver.type = LV_INDEV_TYPE_POINTER;
        touch_driver.read_cb = touch_read_cb;
        touch_driver.user_data = s_touch;
        lv_indev_drv_register(&touch_driver);
#if CONFIG_SOMNOTRACE_BOARD_QEMU
        ESP_LOGI(TAG, "QEMU pointer registered as LVGL touch input");
#endif
    }

    s_lvgl_lock = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_lvgl_lock, ESP_ERR_NO_MEM, TAG, "create LVGL mutex");
    if (lock_lvgl(portMAX_DELAY)) {
        build_ui();
        unlock_lvgl();
    }

    esp_timer_handle_t tick_timer;
    const esp_timer_create_args_t tick_args = {
        .callback = tick_cb,
        .name = "lvgl_tick",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick_timer), TAG,
                        "create LVGL tick timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, 5000), TAG,
                        "start LVGL tick timer");
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(lvgl_task, "display_7b", 8192,
                                               NULL, 3, &s_lvgl_task, 1) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "create display task");

    waveshare_7b_set_brightness(10);
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    ESP_LOGI(TAG, "native 1024x600 QEMU dashboard initialized");
#else
    ESP_LOGI(TAG, "native 1024x600 touch dashboard initialized");
#endif
    return ESP_OK;
}

void bsp_display_set_setup_callback(void (*callback)(void))
{
    s_setup_callback = callback;
}

void bsp_display_enable_touch_services(bool as11_ready, bool oximeter_ready)
{
    portENTER_CRITICAL(&s_state_lock);
    s_touch_services_ready = true;
    s_as11_service_ready = as11_ready;
    s_ox_service_ready = oximeter_ready;
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_show_number(uint32_t value)
{
    char line[16];
    snprintf(line, sizeof(line), "%lu", (unsigned long)value);
    const char *lines[] = { line };
    bsp_display_show_lines(NULL, lines, 1);
}

void bsp_display_show_lines(const char *title, const char *const *lines, int n_lines)
{
    portENTER_CRITICAL(&s_state_lock);
    if (title && title[0]) snprintf(s_state.title, sizeof(s_state.title), "%s", title);
    s_state.status[0] = '\0';
    s_state.attention[0] = '\0';
    for (int i = 0; lines && i < n_lines; ++i) {
        size_t used = strlen(s_state.status);
        if (used && used + 3 < sizeof(s_state.status)) strcat(s_state.status, "  |  ");
        used = strlen(s_state.status);
        if (used < sizeof(s_state.status) - 1) {
            snprintf(s_state.status + used, sizeof(s_state.status) - used, "%s", lines[i]);
        }
        used = strlen(s_state.attention);
        if (used && used + 1 < sizeof(s_state.attention)) strcat(s_state.attention, "\n");
        used = strlen(s_state.attention);
        if (used < sizeof(s_state.attention) - 1) {
            snprintf(s_state.attention + used,
                     sizeof(s_state.attention) - used, "%s", lines[i]);
        }
    }
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_set_notice(const char *text)
{
    portENTER_CRITICAL(&s_state_lock);
    if (!text || !text[0]) {
        s_state.notice[0] = '\0';
        s_state.notice_expires_us = 0;
        s_state.notice_critical = false;
    } else if (!s_state.notice_critical) {
        snprintf(s_state.notice, sizeof(s_state.notice), "%s", text);
        s_state.notice_expires_us = esp_timer_get_time() + 8000000;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_set_critical_notice(const char *text)
{
    portENTER_CRITICAL(&s_state_lock);
    snprintf(s_state.notice, sizeof(s_state.notice), "%s", text ? text : "");
    s_state.notice_expires_us = 0;
    s_state.notice_critical = text && text[0];
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_set_wifi_connected(bool connected)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.wifi = connected;
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_set_as11_paired(bool paired)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.paired = paired;
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_set_sd_ready(bool ready)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.sd_ready = ready;
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_set_battery(int percent, bool charging)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.battery = percent;
    s_state.charging = charging;
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_set_therapy_active(bool active)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.therapy = active;
    if (!active) s_state.leak = NAN;
    portEXIT_CRITICAL(&s_state_lock);
    bsp_display_apply_backlight_policy(false);
}

void bsp_display_push_flow(float flow_lpm)
{
    int value = (int)lrintf(flow_lpm * 10.0f);
    if (value > 1000) value = 1000;
    if (value < -1000) value = -1000;
    portENTER_CRITICAL(&s_state_lock);
    s_state.flow[s_state.flow_head] = (int16_t)value;
    s_state.flow_head = (s_state.flow_head + 1) % FLOW_POINTS;
    s_state.flow_version++;
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_push_leak(float leak_lpm)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.leak = leak_lpm;
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_push_metrics(float pressure_cmh2o, float respiratory_rate,
                              float flow_limitation)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.pressure = pressure_cmh2o;
    s_state.respiratory_rate = respiratory_rate;
    s_state.flow_limitation = flow_limitation;
    portEXIT_CRITICAL(&s_state_lock);
}

void bsp_display_set_therapy_start_time(int64_t start_us)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.therapy_start_us = start_us;
    portEXIT_CRITICAL(&s_state_lock);
}

bool bsp_display_is_therapy_active(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool active = s_state.therapy;
    portEXIT_CRITICAL(&s_state_lock);
    return active;
}

static uint8_t physical_brightness(uint8_t tenth_percent)
{
    uint8_t physical_percent =
        (uint8_t)(((uint16_t)tenth_percent * 97U + 199U) / 200U);
    return physical_percent < 1 ? 1 : physical_percent;
}

void bsp_display_set_brightness(uint8_t tenth_percent)
{
    if (tenth_percent < 1) tenth_percent = 1;
    if (tenth_percent > 200) tenth_percent = 200;
    portENTER_CRITICAL(&s_state_lock);
    s_brightness = tenth_percent;
    bool backlight = s_backlight;
    portEXIT_CRITICAL(&s_state_lock);
    if (backlight) {
        /* The original 1.54-inch target deliberately limits its backlight to
         * 20%. On the much larger 7B, spread that same persisted 1..200 UI
         * range across the usable CH422G PWM range so the default is visible
         * in daylight while the lowest setting remains bedside-dim. */
        waveshare_7b_set_brightness(physical_brightness(tenth_percent));
    }
}

void bsp_display_set_backlight(bool on)
{
    if (lock_lvgl(pdMS_TO_TICKS(100))) {
        if (s_wake_overlay) {
            if (on) {
                lv_obj_add_flag(s_wake_overlay, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(s_wake_overlay, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(s_wake_overlay);
            }
        }
        unlock_lvgl();
    }
    portENTER_CRITICAL(&s_state_lock);
    s_backlight = on;
    uint8_t brightness = s_brightness;
    portEXIT_CRITICAL(&s_state_lock);
    if (on) waveshare_7b_set_brightness(physical_brightness(brightness));
    waveshare_7b_set_backlight(on);
}

uint8_t bsp_display_get_brightness(void)
{
    portENTER_CRITICAL(&s_state_lock);
    uint8_t brightness = s_brightness;
    portEXIT_CRITICAL(&s_state_lock);
    return brightness;
}

void bsp_display_apply_backlight_policy(bool force_on)
{
    if (force_on) {
        bsp_display_set_backlight(true);
        return;
    }
    const device_settings_t *settings = device_settings_get();
    bool therapy = bsp_display_is_therapy_active();
    bool off = settings->lcd_therapy_mode == LCD_THERAPY_ALWAYS_OFF ||
               (therapy && settings->lcd_therapy_mode == LCD_THERAPY_OFF);
    bsp_display_set_backlight(!off);
}

void bsp_display_qemu_seed_demo(void)
{
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    static const touch_history_day_t demo_history[] = {
        {
            .day = "20260901", .sessions = 1, .usage_min = 438,
            .ahi = 1.7f, .pressure_p95 = 10.4f, .leak_p95 = 7.8f,
            .has_summary = true, .has_usage = true, .has_ahi = true,
            .has_pressure_p95 = true, .has_leak_p95 = true,
        },
        {
            .day = "20260831", .sessions = 2, .usage_min = 401,
            .ahi = 2.2f, .pressure_p95 = 10.8f,
            .has_summary = true, .has_usage = true, .has_ahi = true,
            .has_pressure_p95 = true, .has_leak_p95 = false,
        },
        {
            .day = "20260830", .sessions = 1, .usage_min = 462,
            .ahi = 1.3f, .pressure_p95 = 9.9f, .leak_p95 = 5.1f,
            .has_summary = true, .has_usage = true, .has_ahi = true,
            .has_pressure_p95 = true, .has_leak_p95 = true,
        },
    };
    portENTER_CRITICAL(&s_state_lock);
    memcpy(s_services.history, demo_history, sizeof(demo_history));
    s_services.history_count = sizeof(demo_history) / sizeof(demo_history[0]);
    s_services.history_result = ESP_OK;
    s_services.history_version++;
    s_services.as11_count = 1;
    strlcpy(s_services.as11[0].addr, "AA:11:00:00:00:01",
            sizeof(s_services.as11[0].addr));
    strlcpy(s_services.as11[0].name, "AirSense 11 (simulated)",
            sizeof(s_services.as11[0].name));
    s_services.as11[0].rssi = -47;
    s_services.as11_version++;
    s_services.ox_count = 1;
    strlcpy(s_services.ox[0].addr, "02:00:00:00:00:02",
            sizeof(s_services.ox[0].addr));
    strlcpy(s_services.ox[0].name, "O2 Ring (simulated)",
            sizeof(s_services.ox[0].name));
    s_services.ox[0].rssi = -55;
    s_services.ox[0].driver = OX_DRIVER_AUTO;
    s_services.ox_version++;
    portEXIT_CRITICAL(&s_state_lock);
#endif
}

void bsp_display_qemu_set_tab(uint8_t tab)
{
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    if (tab >= 5) return;
    /* The display task owns LVGL. Queue navigation into that task so slow
     * emulated full-frame flushes cannot starve or drop preview changes. */
    portENTER_CRITICAL(&s_state_lock);
    s_qemu_requested_tab = tab;
    portEXIT_CRITICAL(&s_state_lock);
#else
    (void)tab;
#endif
}

void bsp_display_set_rotation(uint16_t degrees)
{
    if (degrees != 0) {
        ESP_LOGW(TAG, "rotation %u ignored: the 7B dashboard is landscape-native",
                 (unsigned)degrees);
    }
}
