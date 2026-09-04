/*
 * SomnoTrace - 1024x600 first-run setup surface
 *
 * The UI is intentionally a lazy singleton. Only a compact persistent shell
 * and one detail tree exist at a time; destroy() releases both when setup is
 * finished. Hardware work is delegated through the controller callbacks.
 */

#include "first_run_setup_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "somnotrace_fonts.h"

#define UI_WIDTH             1024
#define UI_HEIGHT             600
#define UI_HEADER_HEIGHT       64
#define UI_RAIL_X              18
#define UI_RAIL_Y              78
#define UI_RAIL_WIDTH         250
#define UI_RAIL_ROW_HEIGHT     62
#define UI_RAIL_ROW_GAP         8
#define UI_DETAIL_X           284
#define UI_DETAIL_Y            78
#define UI_DETAIL_WIDTH       722
#define UI_DETAIL_HEIGHT      504
#define UI_TOUCH_TARGET_MIN    44
#define UI_ACTION_HEIGHT       52
#define UI_RESULT_HEIGHT       50

#define COLOR_BASE        0x05070e
#define COLOR_PANEL       0x181c29
#define COLOR_CARD        0x101421
#define COLOR_CONTROL     0x2d333f
#define COLOR_INVERSE     0xe0ebe8
#define COLOR_TEXT        0xf0f2f6
#define COLOR_SECONDARY   0xa0a5af
#define COLOR_TERTIARY    0x818691
#define COLOR_DISABLED    0x5e636e
#define COLOR_LIVE        0x00e1e2
#define COLOR_AMBER       0xf8bd40
#define COLOR_FAULT       0xf45249
#define COLOR_SUCCESS     0x58d6a6

#define FONT_TITLE        (&somnotrace_space_grotesk_semibold_29)
#define FONT_SECTION      (&somnotrace_space_grotesk_semibold_23)
#define FONT_ROW          (&somnotrace_space_grotesk_semibold_17)
#define FONT_BUTTON       (&somnotrace_space_grotesk_semibold_15)
#define FONT_BODY         (&somnotrace_space_grotesk_medium_15)
#define FONT_SMALL        (&somnotrace_space_grotesk_medium_13)
#define FONT_MONO         (&somnotrace_ibm_plex_mono_medium_15)
#define FONT_MONO_SMALL   (&somnotrace_ibm_plex_mono_medium_13)

#define STEP_BIT(step) ((uint8_t)(1U << (unsigned)(step)))

static const char *TAG = "first_run_ui";

typedef enum {
    ACTION_RAIL = 1,
    ACTION_BACK,
    ACTION_PRIMARY,
    ACTION_SKIP,
    ACTION_WIFI_RESULT,
    ACTION_TIMEZONE_RESULT,
    ACTION_AIRSENSE_RESULT,
    ACTION_SEARCH,
    ACTION_RETRY,
} action_t;

typedef struct {
    first_run_setup_ui_controller_t controller;
    first_run_setup_ui_live_t live;
    first_run_setup_snapshot_t durable;
    first_run_setup_step_t displayed_step;

    lv_obj_t *root;
    lv_obj_t *rail;
    lv_obj_t *detail;
    lv_obj_t *header_wifi;
    lv_obj_t *header_time;
    lv_obj_t *header_card;
    lv_obj_t *rail_buttons[FIRST_RUN_SETUP_STEP_COUNT];
    lv_obj_t *rail_dots[FIRST_RUN_SETUP_STEP_COUNT];
    lv_obj_t *rail_labels[FIRST_RUN_SETUP_STEP_COUNT];
    lv_obj_t *rail_status[FIRST_RUN_SETUP_STEP_COUNT];

    lv_obj_t *input;
    lv_obj_t *keyboard;
    int selected_wifi;
    int selected_airsense;
    char wifi_password[65];
    char timezone_query[48];
    char pairing_code[5];
    char action_error[96];
    bool visible;
    bool in_event;
    bool render_pending;
} first_run_setup_ui_t;

static first_run_setup_ui_t *s_ui;

static void render_all(void);
static void render_async(void *unused);
static void action_cb(lv_event_t *event);
static void input_changed_cb(lv_event_t *event);

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static unsigned bounded_count(unsigned count, unsigned maximum)
{
    return count < maximum ? count : maximum;
}

static void style_surface(lv_obj_t *obj, uint32_t color, int radius)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            int x, int y, int width,
                            const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_pos(label, x, y);
    if (width > 0) lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_line_space(label, 4, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             int x, int y, int width, int height,
                             bool inverse, action_t action, intptr_t value)
{
    if (height < UI_TOUCH_TARGET_MIN) height = UI_TOUCH_TARGET_MIN;
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    style_surface(button, inverse ? COLOR_INVERSE : COLOR_CONTROL, 16);
    /* All setup destinations and commands begin on touch-down. There is no
     * delayed release transform or size animation. */
    lv_obj_set_style_bg_opa(button, inverse ? LV_OPA_80 : LV_OPA_70,
                            LV_STATE_PRESSED);
    lv_obj_set_style_transform_width(button, 0, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(button, 0, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(button, 0, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_CONTROL),
                              LV_STATE_DISABLED);
    lv_obj_set_style_opa(button, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_add_event_cb(button, action_cb, LV_EVENT_PRESSED,
                        (void *)(((uintptr_t)action << 16) |
                                 ((uintptr_t)value & 0xffffU)));

    lv_obj_t *label = make_label(button, text, 0, 0, width,
                                 FONT_BUTTON,
                                 inverse ? COLOR_BASE : COLOR_TEXT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    return button;
}

static void set_button_enabled(lv_obj_t *button, bool enabled)
{
    if (!button) return;
    if (enabled) lv_obj_clear_state(button, LV_STATE_DISABLED);
    else lv_obj_add_state(button, LV_STATE_DISABLED);
}

static void make_title(const char *title, const char *body)
{
    make_label(s_ui->detail, title, 30, 26, 650, FONT_TITLE, COLOR_TEXT);
    make_label(s_ui->detail, body, 30, 72, 650, FONT_BODY, COLOR_SECONDARY);
}

static lv_obj_t *make_notice(const char *title, const char *body,
                             int y, uint32_t color)
{
    lv_obj_t *box = lv_obj_create(s_ui->detail);
    lv_obj_set_pos(box, 30, y);
    lv_obj_set_size(box, 662, 82);
    style_surface(box, COLOR_CONTROL, 16);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(color), 0);
    make_label(box, title, 18, 13, 620, FONT_ROW, color);
    make_label(box, body, 18, 42, 620, FONT_SMALL, COLOR_SECONDARY);
    return box;
}

static lv_obj_t *make_input(const char *placeholder, int x, int y,
                            int width, bool password, unsigned max_length)
{
    lv_obj_t *input = lv_textarea_create(s_ui->detail);
    lv_obj_set_pos(input, x, y);
    lv_obj_set_size(input, width, UI_ACTION_HEIGHT);
    lv_textarea_set_one_line(input, true);
    lv_textarea_set_password_mode(input, password);
    lv_textarea_set_max_length(input, max_length);
    lv_textarea_set_placeholder_text(input, placeholder);
    lv_obj_set_style_bg_color(input, lv_color_hex(COLOR_CONTROL), 0);
    lv_obj_set_style_bg_opa(input, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(input, 1, 0);
    lv_obj_set_style_border_color(input, lv_color_hex(0x454c5a), 0);
    lv_obj_set_style_border_color(input, lv_color_hex(COLOR_LIVE),
                                  LV_STATE_FOCUSED);
    lv_obj_set_style_radius(input, 15, 0);
    lv_obj_set_style_pad_left(input, 17, 0);
    lv_obj_set_style_pad_right(input, 17, 0);
    lv_obj_set_style_pad_top(input, 14, 0);
    lv_obj_set_style_text_font(input, FONT_BODY, 0);
    lv_obj_set_style_text_color(input, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_color(input, lv_color_hex(COLOR_TERTIARY),
                                LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_event_cb(input, input_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return input;
}

static void input_changed_cb(lv_event_t *event)
{
    if (!s_ui || lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t *input = lv_event_get_target(event);
    const char *text = lv_textarea_get_text(input);
    switch (s_ui->displayed_step) {
        case FIRST_RUN_SETUP_STEP_WIFI:
            copy_text(s_ui->wifi_password, sizeof(s_ui->wifi_password), text);
            break;
        case FIRST_RUN_SETUP_STEP_TIME:
            copy_text(s_ui->timezone_query, sizeof(s_ui->timezone_query), text);
            break;
        case FIRST_RUN_SETUP_STEP_AIRSENSE:
            copy_text(s_ui->pairing_code, sizeof(s_ui->pairing_code), text);
            break;
        default:
            break;
    }
}

static lv_obj_t *make_keyboard(lv_obj_t *textarea,
                               lv_keyboard_mode_t mode,
                               int y, int height)
{
    lv_obj_t *keyboard = lv_keyboard_create(s_ui->detail);
    lv_obj_set_pos(keyboard, 22, y);
    lv_obj_set_size(keyboard, 678, height);
    lv_keyboard_set_mode(keyboard, mode);
    lv_keyboard_set_textarea(keyboard, textarea);
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(keyboard, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(keyboard, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(keyboard, 7, LV_PART_MAIN);
    lv_obj_set_style_pad_column(keyboard, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(COLOR_CONTROL),
                              LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_width(keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(keyboard, 12, LV_PART_ITEMS);
    lv_obj_set_style_text_font(keyboard, FONT_BUTTON, LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard, lv_color_hex(COLOR_TEXT),
                                LV_PART_ITEMS);
    return keyboard;
}

static void make_footer(bool show_back, const char *secondary,
                        const char *primary, bool primary_enabled)
{
    if (show_back) {
        make_button(s_ui->detail, "Back", 30, 428, 112,
                    UI_ACTION_HEIGHT, false, ACTION_BACK, 0);
    }
    if (secondary) {
        make_button(s_ui->detail, secondary, 322, 428, 170,
                    UI_ACTION_HEIGHT, false, ACTION_SKIP, 0);
    }
    if (primary) {
        lv_obj_t *button = make_button(s_ui->detail, primary, 506, 428, 186,
                                       UI_ACTION_HEIGHT, true,
                                       ACTION_PRIMARY, 0);
        set_button_enabled(button, primary_enabled);
    }
}

static void set_action_error(esp_err_t err, const char *action)
{
    if (!s_ui) return;
    if (err == ESP_OK) {
        s_ui->action_error[0] = '\0';
        return;
    }
    snprintf(s_ui->action_error, sizeof(s_ui->action_error),
             "%s could not start (%s).", action, esp_err_to_name(err));
}

static void render_action_error(void)
{
    const char *message = s_ui->live.error_message[0]
                              ? s_ui->live.error_message
                              : s_ui->action_error;
    if (message[0]) {
        make_label(s_ui->detail, message, 30, 400, 650,
                   FONT_SMALL, COLOR_FAULT);
    }
}

static const char *step_label(first_run_setup_step_t step)
{
    static const char *const labels[FIRST_RUN_SETUP_STEP_COUNT] = {
        [FIRST_RUN_SETUP_STEP_WIFI] = "Wi-Fi",
        [FIRST_RUN_SETUP_STEP_TIME] = "Time & clock",
        [FIRST_RUN_SETUP_STEP_AIRSENSE] = "AirSense",
        [FIRST_RUN_SETUP_STEP_CARD] = "microSD card",
        [FIRST_RUN_SETUP_STEP_ALERTS] = "Alerts",
        [FIRST_RUN_SETUP_STEP_UPLOADS] = "Uploads",
    };
    return (unsigned)step < FIRST_RUN_SETUP_STEP_COUNT ? labels[step] : "";
}

static const char *step_status(first_run_setup_step_t step)
{
    uint8_t bit = STEP_BIT(step);
    if ((s_ui->durable.state.completed_mask & bit) != 0) return "Complete";
    if (step == FIRST_RUN_SETUP_STEP_CARD &&
        s_ui->durable.state.continue_without_recording) {
        return "Not recording";
    }
    if ((s_ui->durable.state.skipped_mask & bit) != 0) return "Skipped";
    if (step == FIRST_RUN_SETUP_STEP_ALERTS ||
        step == FIRST_RUN_SETUP_STEP_UPLOADS) return "Optional";
    return "Not set";
}

static void render_header(void)
{
    char text[96];
    if (s_ui->live.wifi_configured && s_ui->live.connected_ssid[0]) {
        snprintf(text, sizeof(text), "Wi-Fi  %s", s_ui->live.connected_ssid);
    } else if (s_ui->live.wifi_state == FIRST_RUN_SETUP_UI_WIFI_SCANNING ||
               s_ui->live.wifi_state == FIRST_RUN_SETUP_UI_WIFI_CONNECTING) {
        copy_text(text, sizeof(text), "Wi-Fi  Working");
    } else {
        copy_text(text, sizeof(text), "Wi-Fi  Not connected");
    }
    lv_label_set_text(s_ui->header_wifi, text);

    if (s_ui->live.time_configured) {
        snprintf(text, sizeof(text), "Time  %s",
                 s_ui->live.local_time[0] ? s_ui->live.local_time : "Set");
    } else {
        copy_text(text, sizeof(text), "Time  Not set");
    }
    lv_label_set_text(s_ui->header_time, text);

    switch (s_ui->live.card_state) {
        case FIRST_RUN_SETUP_UI_CARD_READY:
            snprintf(text, sizeof(text), "Card  %s",
                     s_ui->live.card_summary[0]
                         ? s_ui->live.card_summary : "Ready");
            break;
        case FIRST_RUN_SETUP_UI_CARD_CHECKING:
            copy_text(text, sizeof(text), "Card  Checking");
            break;
        case FIRST_RUN_SETUP_UI_CARD_FULL:
            copy_text(text, sizeof(text), "Card  Full");
            break;
        case FIRST_RUN_SETUP_UI_CARD_UNREADABLE:
            copy_text(text, sizeof(text), "Card  Unreadable");
            break;
        case FIRST_RUN_SETUP_UI_CARD_MISSING:
        default:
            copy_text(text, sizeof(text), "Card  Not found");
            break;
    }
    lv_label_set_text(s_ui->header_card, text);
}

static void render_rail(void)
{
    for (unsigned i = 0; i < FIRST_RUN_SETUP_STEP_COUNT; i++) {
        bool selected = s_ui->displayed_step == (first_run_setup_step_t)i;
        bool resolved = first_run_setup_step_is_resolved(
            &s_ui->durable.state, (first_run_setup_step_t)i);
        lv_obj_set_style_bg_color(s_ui->rail_buttons[i],
                                  lv_color_hex(selected ? COLOR_INVERSE
                                                        : COLOR_CARD), 0);
        lv_obj_set_style_bg_opa(s_ui->rail_buttons[i], LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_ui->rail_labels[i],
                                    lv_color_hex(selected ? COLOR_BASE
                                                          : COLOR_TEXT), 0);
        lv_label_set_text(s_ui->rail_status[i], step_status(i));
        lv_obj_set_style_text_color(s_ui->rail_status[i],
                                    lv_color_hex(selected ? 0x4c5360
                                                          : COLOR_TERTIARY), 0);
        lv_obj_set_style_bg_color(s_ui->rail_dots[i],
                                  lv_color_hex(resolved ? COLOR_LIVE
                                                        : COLOR_DISABLED), 0);
    }
}

static void render_wifi_results(void)
{
    make_title("Choose a Wi-Fi network",
               "Only 2.4 GHz networks are supported. Select a network to continue.");
    unsigned count = bounded_count(s_ui->live.wifi_result_count,
                                   FIRST_RUN_SETUP_UI_WIFI_RESULT_MAX);
    if (count == 0) {
        make_notice("No networks yet",
                    "Move closer to the router, then scan again.",
                    138, COLOR_AMBER);
    } else {
        lv_obj_t *list = lv_obj_create(s_ui->detail);
        lv_obj_set_pos(list, 22, 120);
        lv_obj_set_size(list, 678, 296);
        style_surface(list, COLOR_PANEL, 0);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
        for (unsigned i = 0; i < count; i++) {
            const first_run_setup_ui_wifi_result_t *network =
                &s_ui->live.wifi_results[i];
            lv_obj_t *row = make_button(list, network->ssid,
                                        8, 8 + (int)i * 54, 654,
                                        UI_RESULT_HEIGHT, false,
                                        ACTION_WIFI_RESULT, i);
            lv_obj_t *label = lv_obj_get_child(row, 0);
            lv_obj_set_width(label, 480);
            lv_obj_align(label, LV_ALIGN_LEFT_MID, 18, 0);
            char meta[32];
            snprintf(meta, sizeof(meta), "%s  %d dBm",
                     network->secure ? "Secure" : "Open",
                     (int)network->rssi_dbm);
            lv_obj_t *right = make_label(row, meta, 500, 15, 145,
                                         FONT_MONO_SMALL, COLOR_SECONDARY);
            lv_obj_set_style_text_align(right, LV_TEXT_ALIGN_RIGHT, 0);
        }
    }
    make_button(s_ui->detail, "Scan again", 522, 428, 170,
                UI_ACTION_HEIGHT, false, ACTION_RETRY, 0);
}

static void render_wifi_password(void)
{
    const first_run_setup_ui_wifi_result_t *network =
        s_ui->selected_wifi >= 0 &&
                s_ui->selected_wifi < s_ui->live.wifi_result_count
            ? &s_ui->live.wifi_results[s_ui->selected_wifi]
            : NULL;
    char body[96];
    snprintf(body, sizeof(body), "Connect to %s. Passwords stay on this device.",
             network && network->ssid[0] ? network->ssid : "the selected network");
    make_title("Network password", body);
    s_ui->input = make_input(network && !network->secure
                                 ? "Open network - no password required"
                                 : "Enter password",
                             30, 126, 472, network ? network->secure : true, 64);
    lv_textarea_set_text(s_ui->input, s_ui->wifi_password);
    lv_obj_t *connect = make_button(s_ui->detail, "Connect", 516, 126, 176,
                                    UI_ACTION_HEIGHT, true,
                                    ACTION_PRIMARY, 0);
    set_button_enabled(connect, network != NULL);
    s_ui->keyboard = make_keyboard(s_ui->input,
                                   LV_KEYBOARD_MODE_TEXT_LOWER, 195, 286);
}

static void render_wifi(void)
{
    switch (s_ui->live.wifi_state) {
        case FIRST_RUN_SETUP_UI_WIFI_PASSWORD:
            render_wifi_password();
            break;

        case FIRST_RUN_SETUP_UI_WIFI_SCANNING:
            make_title("Looking for Wi-Fi",
                       "Scanning nearby 2.4 GHz networks. This can take a moment.");
            make_notice("Scanning", "Nearby networks will appear automatically.",
                        150, COLOR_LIVE);
            make_footer(true, "Skip for now", NULL, false);
            break;

        case FIRST_RUN_SETUP_UI_WIFI_CONNECTING:
            make_title("Connecting to Wi-Fi",
                       "SomnoTrace is checking the network credentials.");
            make_notice("Connecting", "Keep this screen open for a moment.",
                        150, COLOR_LIVE);
            make_footer(true, "Skip for now", NULL, false);
            break;

        case FIRST_RUN_SETUP_UI_WIFI_SELECT:
            render_wifi_results();
            break;

        case FIRST_RUN_SETUP_UI_WIFI_CONNECTED:
            make_title("Wi-Fi connected",
                       "Network time and optional uploads can now use this connection.");
            make_notice("Connected", s_ui->live.connected_ssid[0]
                                         ? s_ui->live.connected_ssid
                                         : "The network is ready.",
                        150, COLOR_SUCCESS);
            make_footer(false, NULL, "Continue", true);
            break;

        case FIRST_RUN_SETUP_UI_WIFI_AUTH_FAILED:
            make_title("That password did not work",
                       "Check the password and try again. AirSense and card recording do not depend on Wi-Fi.");
            make_notice("Connection rejected",
                        s_ui->live.error_message[0]
                            ? s_ui->live.error_message
                            : "The router rejected these credentials.",
                        145, COLOR_FAULT);
            make_footer(true, "Skip for now", "Try again", true);
            break;

        case FIRST_RUN_SETUP_UI_WIFI_ERROR:
            make_title("Wi-Fi is unavailable",
                       "AirSense pairing and card recording still work without Wi-Fi.");
            make_notice("Could not scan or connect",
                        s_ui->live.error_message[0]
                            ? s_ui->live.error_message
                            : "Check the router and try again.",
                        145, COLOR_FAULT);
            make_footer(true, "Skip for now", "Scan again", true);
            break;

        case FIRST_RUN_SETUP_UI_WIFI_IDLE:
        default:
            if (s_ui->live.wifi_result_count > 0) {
                render_wifi_results();
            } else {
                make_title("Set this up at the bedside",
                           "Connect Wi-Fi for automatic time and optional uploads. Only 2.4 GHz networks are supported.");
                make_notice("Private by default",
                            "Therapy recordings stay on the microSD card unless uploads are enabled later.",
                            150, COLOR_LIVE);
                make_footer(false, "Skip for now", "Start", true);
            }
            break;
    }
    render_action_error();
}

static void render_time_results(void)
{
    make_title("Choose your time zone",
               "Searchable IANA time zones keep nights and clock changes accurate.");
    s_ui->input = make_input("Search city or time zone", 30, 118, 472,
                             false, 47);
    lv_textarea_set_text(s_ui->input, s_ui->timezone_query);
    make_button(s_ui->detail, "Search", 516, 118, 176,
                UI_ACTION_HEIGHT, true, ACTION_SEARCH, 0);

    unsigned count = bounded_count(s_ui->live.timezone_result_count,
                                   FIRST_RUN_SETUP_UI_TIMEZONE_RESULT_MAX);
    lv_obj_t *list = lv_obj_create(s_ui->detail);
    lv_obj_set_pos(list, 22, 176);
    lv_obj_set_size(list, 678, 262);
    style_surface(list, COLOR_PANEL, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    for (unsigned i = 0; i < count; i++) {
        const first_run_setup_ui_timezone_result_t *zone =
            &s_ui->live.timezone_results[i];
        lv_obj_t *row = make_button(list, zone->id,
                                    8, 6 + (int)i * 48, 654,
                                    UI_TOUCH_TARGET_MIN, false,
                                    ACTION_TIMEZONE_RESULT, i);
        lv_obj_t *label = lv_obj_get_child(row, 0);
        lv_obj_set_width(label, 455);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 16, 0);
        char meta[32];
        snprintf(meta, sizeof(meta), "%s %s", zone->utc_offset,
                 zone->abbreviation);
        lv_obj_t *right = make_label(row, meta, 490, 13, 150,
                                     FONT_MONO_SMALL, COLOR_SECONDARY);
        lv_obj_set_style_text_align(right, LV_TEXT_ALIGN_RIGHT, 0);
    }
    if (count == 0) {
        make_label(list,
                   "Start with a city, country, or IANA zone name.",
                   8, 14, 620, FONT_BODY, COLOR_SECONDARY);
    }
    make_button(s_ui->detail, "Back", 30, 448, 112,
                UI_TOUCH_TARGET_MIN, false, ACTION_BACK, 0);
    make_button(s_ui->detail, "Skip for now", 522, 448, 170,
                UI_TOUCH_TARGET_MIN, false, ACTION_SKIP, 0);
}

static void render_time(void)
{
    switch (s_ui->live.time_state) {
        case FIRST_RUN_SETUP_UI_TIME_SEARCHING:
            make_title("Searching time zones",
                       "Matching locations and IANA time-zone names.");
            make_notice("Searching", "Results will appear automatically.",
                        150, COLOR_LIVE);
            make_footer(true, "Skip for now", NULL, false);
            break;
        case FIRST_RUN_SETUP_UI_TIME_APPLYING:
            make_title("Setting the clock",
                       "Applying the selected time zone and checking local time.");
            make_notice("Applying", s_ui->live.timezone_id,
                        150, COLOR_LIVE);
            make_footer(true, "Skip for now", NULL, false);
            break;
        case FIRST_RUN_SETUP_UI_TIME_SET:
            make_title("Time and clock are ready",
                       "Therapy sessions will be grouped into the correct local night.");
            make_notice(s_ui->live.local_time[0]
                            ? s_ui->live.local_time : "Clock set",
                        s_ui->live.timezone_id[0]
                            ? s_ui->live.timezone_id : "Local time zone",
                        150, COLOR_SUCCESS);
            make_footer(false, NULL, "Continue", true);
            break;
        case FIRST_RUN_SETUP_UI_TIME_ERROR:
            make_title("The clock could not be set",
                       "Choose another time zone or continue with UTC for now.");
            make_notice("Time not set",
                        s_ui->live.error_message[0]
                            ? s_ui->live.error_message
                            : "The selected time zone could not be applied.",
                        145, COLOR_FAULT);
            make_footer(true, "Skip for now", "Try again", true);
            break;
        case FIRST_RUN_SETUP_UI_TIME_IDLE:
        default:
            render_time_results();
            break;
    }
    render_action_error();
}

static void render_airsense_results(void)
{
    make_title("Choose your AirSense",
               "Keep More > myAir App open on the AirSense 11 while scanning.");
    unsigned count = bounded_count(s_ui->live.airsense_result_count,
                                   FIRST_RUN_SETUP_UI_AIRSENSE_RESULT_MAX);
    lv_obj_t *list = lv_obj_create(s_ui->detail);
    lv_obj_set_pos(list, 22, 120);
    lv_obj_set_size(list, 678, 296);
    style_surface(list, COLOR_PANEL, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    for (unsigned i = 0; i < count; i++) {
        const first_run_setup_ui_airsense_result_t *device =
            &s_ui->live.airsense_results[i];
        lv_obj_t *row = make_button(list,
                                    device->name[0] ? device->name
                                                    : "AirSense 11",
                                    8, 8 + (int)i * 54, 654,
                                    UI_RESULT_HEIGHT, false,
                                    ACTION_AIRSENSE_RESULT, i);
        lv_obj_t *label = lv_obj_get_child(row, 0);
        lv_obj_set_width(label, 440);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 18, 0);
        char meta[48];
        snprintf(meta, sizeof(meta), "%s  %d dBm", device->address,
                 (int)device->rssi_dbm);
        lv_obj_t *right = make_label(row, meta, 455, 15, 190,
                                     FONT_MONO_SMALL, COLOR_SECONDARY);
        lv_obj_set_style_text_align(right, LV_TEXT_ALIGN_RIGHT, 0);
    }
    make_button(s_ui->detail, "Scan again", 522, 428, 170,
                UI_ACTION_HEIGHT, false, ACTION_RETRY, 0);
}

static void render_airsense_code(void)
{
    make_title("Enter the AirSense code",
               "A code is now shown on the AirSense. Enter all 4 digits exactly as displayed.");
    s_ui->input = make_input("4-digit code", 30, 126, 310,
                             false, 4);
    lv_textarea_set_accepted_chars(s_ui->input, "0123456789");
    lv_textarea_set_text(s_ui->input, s_ui->pairing_code);
    lv_obj_set_style_text_font(s_ui->input, FONT_MONO, 0);
    lv_obj_t *pair = make_button(s_ui->detail, "Pair", 516, 126, 176,
                                 UI_ACTION_HEIGHT, true,
                                 ACTION_PRIMARY, 0);
    set_button_enabled(pair, true);
    s_ui->keyboard = make_keyboard(s_ui->input,
                                   LV_KEYBOARD_MODE_NUMBER, 198, 282);
}

static void render_airsense(void)
{
    switch (s_ui->live.airsense_state) {
        case FIRST_RUN_SETUP_UI_AIRSENSE_SCANNING:
            make_title("Looking for AirSense 11",
                       "Scanning nearby Bluetooth devices. Keep myAir App open on the machine.");
            make_notice("Scanning", "Pairable AirSense devices will appear here.",
                        150, COLOR_LIVE);
            make_footer(true, "Skip for now", NULL, false);
            break;
        case FIRST_RUN_SETUP_UI_AIRSENSE_SELECT:
            render_airsense_results();
            break;
        case FIRST_RUN_SETUP_UI_AIRSENSE_NOT_FOUND:
            make_title("No AirSense found",
                       "On the machine, open More > myAir App first, then scan again here.");
            make_notice("Nothing pairable nearby",
                        "Keep the AirSense awake and close to SomnoTrace.",
                        145, COLOR_AMBER);
            make_footer(true, "Skip for now", "Scan again", true);
            break;
        case FIRST_RUN_SETUP_UI_AIRSENSE_STARTING:
            make_title("Starting secure pairing",
                       "Waiting for the AirSense to display its pairing code.");
            make_notice("Contacting AirSense", "Do not close myAir App yet.",
                        150, COLOR_LIVE);
            make_footer(true, "Skip for now", NULL, false);
            break;
        case FIRST_RUN_SETUP_UI_AIRSENSE_WAIT_CODE:
            render_airsense_code();
            break;
        case FIRST_RUN_SETUP_UI_AIRSENSE_CONFIRMING:
            make_title("Checking the code",
                       "SomnoTrace and the AirSense are confirming the secure connection.");
            make_notice("Pairing", "This normally takes a few seconds.",
                        150, COLOR_LIVE);
            make_footer(true, "Skip for now", NULL, false);
            break;
        case FIRST_RUN_SETUP_UI_AIRSENSE_CODE_REJECTED:
            make_title("The code was rejected",
                       "The code may have expired. Restart myAir App on the machine and try again.");
            make_notice("Not paired",
                        s_ui->live.error_message[0]
                            ? s_ui->live.error_message
                            : "No connection was saved.",
                        145, COLOR_FAULT);
            make_footer(true, "Skip for now", "Start again", true);
            break;
        case FIRST_RUN_SETUP_UI_AIRSENSE_PAIRED:
            make_title("AirSense paired",
                       "SomnoTrace can now follow therapy status and live respiratory data.");
            make_notice(s_ui->live.paired_name[0]
                            ? s_ui->live.paired_name : "AirSense 11",
                        s_ui->live.paired_address[0]
                            ? s_ui->live.paired_address
                            : "Secure Bluetooth connection saved",
                        150, COLOR_SUCCESS);
            make_footer(false, NULL, "Continue", true);
            break;
        case FIRST_RUN_SETUP_UI_AIRSENSE_ERROR:
            make_title("Pairing could not continue",
                       "Restart the machine-first pairing flow and try again.");
            make_notice("AirSense not paired",
                        s_ui->live.error_message[0]
                            ? s_ui->live.error_message
                            : "The Bluetooth connection ended.",
                        145, COLOR_FAULT);
            make_footer(true, "Skip for now", "Start again", true);
            break;
        case FIRST_RUN_SETUP_UI_AIRSENSE_INSTRUCTIONS:
        default:
            make_title("Put AirSense in pairing mode",
                       "Start on the machine. On the AirSense 11, open More > myAir App before scanning here.");
            make_notice("Machine first",
                        "Leave myAir App open. SomnoTrace will scan, then the machine will show a 4-digit code.",
                        154, COLOR_LIVE);
            make_footer(false, "Skip for now", "I've opened myAir App", true);
            break;
    }
    render_action_error();
}

static void render_card(void)
{
    switch (s_ui->live.card_state) {
        case FIRST_RUN_SETUP_UI_CARD_READY:
            make_title("microSD card is ready",
                       "New therapy sessions can be recorded and reviewed in History.");
            make_notice("Ready",
                        s_ui->live.card_summary[0]
                            ? s_ui->live.card_summary
                            : "Writable FAT32 card detected",
                        150, COLOR_SUCCESS);
            make_footer(false, NULL, "Continue", true);
            break;
        case FIRST_RUN_SETUP_UI_CARD_CHECKING:
            make_title("Checking the microSD card",
                       "SomnoTrace is confirming that the card can be read and written.");
            make_notice("Checking", "Keep the card inserted.",
                        150, COLOR_LIVE);
            make_footer(true, NULL, NULL, false);
            break;
        case FIRST_RUN_SETUP_UI_CARD_FULL:
            make_title("The microSD card is full",
                       "Free space or insert another compatible card before recording.");
            make_notice("No recording space",
                        s_ui->live.error_message[0]
                            ? s_ui->live.error_message
                            : "Existing nights are still available if readable.",
                        145, COLOR_FAULT);
            make_footer(true, "Continue without recording", "Check again", true);
            break;
        case FIRST_RUN_SETUP_UI_CARD_UNREADABLE:
            make_title("The microSD card could not be read",
                       "Use one FAT32 partition with an MBR partition table. exFAT and GPT do not mount.");
            make_notice("Card unreadable",
                        s_ui->live.error_message[0]
                            ? s_ui->live.error_message
                            : "Reinsert or reformat the card, then check again.",
                        145, COLOR_FAULT);
            make_footer(true, "Continue without recording", "Check again", true);
            break;
        case FIRST_RUN_SETUP_UI_CARD_MISSING:
        default:
            make_title("Insert a microSD card",
                       "FAT32 with MBR, 8 GB minimum; a reputable 16-32 GB card is recommended.");
            make_notice("No card detected",
                        "SomnoTrace can run therapy, but it cannot record new nights without a card.",
                        145, COLOR_AMBER);
            make_footer(true, "Continue without recording", "Check again", true);
            break;
    }
    render_action_error();
}

static void render_optional(first_run_setup_step_t step)
{
    bool alerts = step == FIRST_RUN_SETUP_STEP_ALERTS;
    bool configured = alerts ? s_ui->live.alerts_configured
                             : s_ui->live.uploads_configured;
    const char *title = alerts ? "Therapy alerts" : "Optional uploads";
    const char *body = alerts
        ? "Choose which bedside therapy conditions need a visible alert."
        : "Choose whether encrypted session summaries may leave this device.";
    make_title(title, body);
    if (configured) {
        make_notice(alerts ? "Alerts configured" : "Uploads configured",
                    alerts ? "Your alert choices are saved."
                           : "Your upload destination and consent are saved.",
                    150, COLOR_SUCCESS);
        make_footer(false, NULL, "Continue", true);
    } else {
        make_notice("Optional",
                    alerts ? "You can change alert choices later in Manage."
                           : "Nothing is uploaded unless you explicitly enable it.",
                    150, COLOR_LIVE);
        make_footer(true, "Skip for now", "Configure", true);
    }
    render_action_error();
}

static void render_finished(void)
{
    make_title("SomnoTrace is ready",
               "Setup choices are saved. You can change them later in Manage.");
    make_notice("Ready for tonight",
                s_ui->live.card_state == FIRST_RUN_SETUP_UI_CARD_READY
                    ? "AirSense status and card recording are ready."
                    : "Therapy can run; card recording remains unavailable.",
                150, COLOR_SUCCESS);
    make_button(s_ui->detail, "Open SomnoTrace", 472, 428, 220,
                UI_ACTION_HEIGHT, true, ACTION_PRIMARY, 0);
}

static void render_detail(void)
{
    s_ui->input = NULL;
    s_ui->keyboard = NULL;
    lv_obj_clean(s_ui->detail);
    switch (s_ui->displayed_step) {
        case FIRST_RUN_SETUP_STEP_WIFI: render_wifi(); break;
        case FIRST_RUN_SETUP_STEP_TIME: render_time(); break;
        case FIRST_RUN_SETUP_STEP_AIRSENSE: render_airsense(); break;
        case FIRST_RUN_SETUP_STEP_CARD: render_card(); break;
        case FIRST_RUN_SETUP_STEP_ALERTS:
        case FIRST_RUN_SETUP_STEP_UPLOADS:
            render_optional(s_ui->displayed_step);
            break;
        case FIRST_RUN_SETUP_STEP_FINISHED:
        default:
            render_finished();
            break;
    }
}

static void refresh_durable(void)
{
    first_run_setup_snapshot(&s_ui->durable);
}

static void select_step(first_run_setup_step_t step)
{
    if ((unsigned)step >= FIRST_RUN_SETUP_STEP_COUNT) return;
    esp_err_t err = first_run_setup_update(step,
                                           FIRST_RUN_SETUP_UPDATE_SELECT);
    set_action_error(err, "Saving setup position");
    if (err == ESP_OK) {
        refresh_durable();
        s_ui->displayed_step = step;
    }
    render_all();
}

static void advance_from_current(void)
{
    refresh_durable();
    s_ui->displayed_step = s_ui->durable.state.current_step;
    render_all();
}

static void skip_current(void)
{
    first_run_setup_step_t step = s_ui->displayed_step;
    first_run_setup_update_t update =
        step == FIRST_RUN_SETUP_STEP_CARD
            ? FIRST_RUN_SETUP_UPDATE_CONTINUE_WITHOUT_RECORDING
            : FIRST_RUN_SETUP_UPDATE_SKIP;
    esp_err_t err = first_run_setup_update(step, update);
    set_action_error(err, step == FIRST_RUN_SETUP_STEP_CARD
                              ? "Saving the no-recording choice"
                              : "Saving the optional choice");
    if (err == ESP_OK) advance_from_current();
    else render_all();
}

static esp_err_t call_noarg(esp_err_t (*callback)(void *), const char *name)
{
    if (!callback) {
        set_action_error(ESP_ERR_NOT_SUPPORTED, name);
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t err = callback(s_ui->controller.context);
    set_action_error(err, name);
    return err;
}

static void wifi_start_scan(void)
{
    if (call_noarg(s_ui->controller.wifi_scan, "Wi-Fi scan") == ESP_OK) {
        s_ui->live.wifi_state = FIRST_RUN_SETUP_UI_WIFI_SCANNING;
        s_ui->live.wifi_result_count = 0;
        s_ui->wifi_password[0] = '\0';
    }
    render_all();
}

static void airsense_start_scan(void)
{
    if (call_noarg(s_ui->controller.airsense_scan,
                   "AirSense scan") == ESP_OK) {
        s_ui->live.airsense_state = FIRST_RUN_SETUP_UI_AIRSENSE_SCANNING;
        s_ui->live.airsense_result_count = 0;
        s_ui->pairing_code[0] = '\0';
    }
    render_all();
}

static void primary_action(void)
{
    esp_err_t err = ESP_OK;
    switch (s_ui->displayed_step) {
        case FIRST_RUN_SETUP_STEP_WIFI:
            switch (s_ui->live.wifi_state) {
                case FIRST_RUN_SETUP_UI_WIFI_PASSWORD: {
                    if (s_ui->selected_wifi < 0 ||
                        s_ui->selected_wifi >= s_ui->live.wifi_result_count) {
                        set_action_error(ESP_ERR_INVALID_STATE,
                                         "Wi-Fi connection");
                        break;
                    }
                    const first_run_setup_ui_wifi_result_t *network =
                        &s_ui->live.wifi_results[s_ui->selected_wifi];
                    const char *password = s_ui->input
                                               ? lv_textarea_get_text(s_ui->input)
                                               : "";
                    if (!s_ui->controller.wifi_connect) {
                        set_action_error(ESP_ERR_NOT_SUPPORTED,
                                         "Wi-Fi connection");
                        break;
                    }
                    err = s_ui->controller.wifi_connect(
                        s_ui->controller.context, network->ssid, password);
                    set_action_error(err, "Wi-Fi connection");
                    if (err == ESP_OK) {
                        s_ui->live.wifi_state =
                            FIRST_RUN_SETUP_UI_WIFI_CONNECTING;
                    }
                    break;
                }
                case FIRST_RUN_SETUP_UI_WIFI_CONNECTED:
                    advance_from_current();
                    return;
                case FIRST_RUN_SETUP_UI_WIFI_AUTH_FAILED:
                    s_ui->live.wifi_state = FIRST_RUN_SETUP_UI_WIFI_PASSWORD;
                    break;
                case FIRST_RUN_SETUP_UI_WIFI_ERROR:
                case FIRST_RUN_SETUP_UI_WIFI_IDLE:
                default:
                    wifi_start_scan();
                    return;
            }
            break;

        case FIRST_RUN_SETUP_STEP_TIME:
            if (s_ui->live.time_state == FIRST_RUN_SETUP_UI_TIME_SET) {
                advance_from_current();
                return;
            }
            s_ui->live.time_state = FIRST_RUN_SETUP_UI_TIME_IDLE;
            break;

        case FIRST_RUN_SETUP_STEP_AIRSENSE:
            switch (s_ui->live.airsense_state) {
                case FIRST_RUN_SETUP_UI_AIRSENSE_WAIT_CODE: {
                    const char *code = s_ui->input
                                           ? lv_textarea_get_text(s_ui->input)
                                           : "";
                    if (strlen(code) != 4) {
                        set_action_error(ESP_ERR_INVALID_SIZE,
                                         "Four-digit code");
                        break;
                    }
                    if (!s_ui->controller.airsense_confirm_code) {
                        set_action_error(ESP_ERR_NOT_SUPPORTED,
                                         "Pairing code");
                        break;
                    }
                    err = s_ui->controller.airsense_confirm_code(
                        s_ui->controller.context, code);
                    set_action_error(err, "Pairing code");
                    if (err == ESP_OK) {
                        s_ui->live.airsense_state =
                            FIRST_RUN_SETUP_UI_AIRSENSE_CONFIRMING;
                    }
                    break;
                }
                case FIRST_RUN_SETUP_UI_AIRSENSE_PAIRED:
                    advance_from_current();
                    return;
                case FIRST_RUN_SETUP_UI_AIRSENSE_INSTRUCTIONS:
                case FIRST_RUN_SETUP_UI_AIRSENSE_NOT_FOUND:
                case FIRST_RUN_SETUP_UI_AIRSENSE_CODE_REJECTED:
                case FIRST_RUN_SETUP_UI_AIRSENSE_ERROR:
                default:
                    airsense_start_scan();
                    return;
            }
            break;

        case FIRST_RUN_SETUP_STEP_CARD:
            if (s_ui->live.card_state == FIRST_RUN_SETUP_UI_CARD_READY) {
                advance_from_current();
                return;
            }
            if (call_noarg(s_ui->controller.card_retry,
                           "Card check") == ESP_OK) {
                s_ui->live.card_state = FIRST_RUN_SETUP_UI_CARD_CHECKING;
            }
            break;

        case FIRST_RUN_SETUP_STEP_ALERTS:
            if (s_ui->live.alerts_configured) {
                advance_from_current();
                return;
            }
            call_noarg(s_ui->controller.configure_alerts,
                       "Alert configuration");
            break;

        case FIRST_RUN_SETUP_STEP_UPLOADS:
            if (s_ui->live.uploads_configured) {
                advance_from_current();
                return;
            }
            call_noarg(s_ui->controller.configure_uploads,
                       "Upload configuration");
            break;

        case FIRST_RUN_SETUP_STEP_FINISHED:
        default:
            call_noarg(s_ui->controller.finished, "Opening SomnoTrace");
            break;
    }
    render_all();
}

static void search_action(void)
{
    const char *query = s_ui->input ? lv_textarea_get_text(s_ui->input) : "";
    if (!s_ui->controller.timezone_search) {
        set_action_error(ESP_ERR_NOT_SUPPORTED, "Time-zone search");
    } else {
        esp_err_t err = s_ui->controller.timezone_search(
            s_ui->controller.context, query);
        set_action_error(err, "Time-zone search");
        if (err == ESP_OK) {
            s_ui->live.time_state = FIRST_RUN_SETUP_UI_TIME_SEARCHING;
            s_ui->live.timezone_result_count = 0;
        }
    }
    render_all();
}

static void retry_action(void)
{
    switch (s_ui->displayed_step) {
        case FIRST_RUN_SETUP_STEP_WIFI: wifi_start_scan(); return;
        case FIRST_RUN_SETUP_STEP_AIRSENSE: airsense_start_scan(); return;
        case FIRST_RUN_SETUP_STEP_CARD:
            if (call_noarg(s_ui->controller.card_retry,
                           "Card check") == ESP_OK) {
                s_ui->live.card_state = FIRST_RUN_SETUP_UI_CARD_CHECKING;
            }
            break;
        default: break;
    }
    render_all();
}

static void result_action(action_t action, unsigned index)
{
    esp_err_t err = ESP_OK;
    if (action == ACTION_WIFI_RESULT &&
        index < s_ui->live.wifi_result_count) {
        s_ui->selected_wifi = (int)index;
        const first_run_setup_ui_wifi_result_t *network =
            &s_ui->live.wifi_results[index];
        if (strcmp(s_ui->live.selected_ssid, network->ssid) != 0) {
            s_ui->wifi_password[0] = '\0';
        }
        copy_text(s_ui->live.selected_ssid,
                  sizeof(s_ui->live.selected_ssid), network->ssid);
        if (network->secure) {
            s_ui->live.wifi_state = FIRST_RUN_SETUP_UI_WIFI_PASSWORD;
        } else if (s_ui->controller.wifi_connect) {
            err = s_ui->controller.wifi_connect(s_ui->controller.context,
                                                network->ssid, "");
            set_action_error(err, "Wi-Fi connection");
            if (err == ESP_OK) {
                s_ui->live.wifi_state = FIRST_RUN_SETUP_UI_WIFI_CONNECTING;
            }
        } else {
            set_action_error(ESP_ERR_NOT_SUPPORTED, "Wi-Fi connection");
        }
    } else if (action == ACTION_TIMEZONE_RESULT &&
               index < s_ui->live.timezone_result_count) {
        const first_run_setup_ui_timezone_result_t *zone =
            &s_ui->live.timezone_results[index];
        if (!s_ui->controller.timezone_select) {
            set_action_error(ESP_ERR_NOT_SUPPORTED, "Setting time zone");
        } else {
            err = s_ui->controller.timezone_select(s_ui->controller.context,
                                                   zone->id,
                                                   zone->posix_tz);
            set_action_error(err, "Setting time zone");
            if (err == ESP_OK) {
                s_ui->live.time_state = FIRST_RUN_SETUP_UI_TIME_APPLYING;
                copy_text(s_ui->live.timezone_id,
                          sizeof(s_ui->live.timezone_id), zone->id);
            }
        }
    } else if (action == ACTION_AIRSENSE_RESULT &&
               index < s_ui->live.airsense_result_count) {
        s_ui->selected_airsense = (int)index;
        const first_run_setup_ui_airsense_result_t *device =
            &s_ui->live.airsense_results[index];
        if (strcmp(s_ui->live.selected_airsense_address,
                   device->address) != 0) {
            s_ui->pairing_code[0] = '\0';
        }
        copy_text(s_ui->live.selected_airsense_address,
                  sizeof(s_ui->live.selected_airsense_address),
                  device->address);
        if (!s_ui->controller.airsense_begin_pairing) {
            set_action_error(ESP_ERR_NOT_SUPPORTED, "AirSense pairing");
        } else {
            err = s_ui->controller.airsense_begin_pairing(
                s_ui->controller.context, device->address);
            set_action_error(err, "AirSense pairing");
            if (err == ESP_OK) {
                s_ui->live.airsense_state =
                    FIRST_RUN_SETUP_UI_AIRSENSE_STARTING;
            }
        }
    }
    render_all();
}

static void action_cb(lv_event_t *event)
{
    if (!s_ui || lv_event_get_code(event) != LV_EVENT_PRESSED) return;
    s_ui->in_event = true;
    uintptr_t packed = (uintptr_t)lv_event_get_user_data(event);
    action_t action = (action_t)(packed >> 16);
    unsigned value = (unsigned)(packed & 0xffffU);
    switch (action) {
        case ACTION_RAIL:
            select_step((first_run_setup_step_t)value);
            break;
        case ACTION_BACK:
            if (s_ui->displayed_step > FIRST_RUN_SETUP_STEP_WIFI &&
                s_ui->displayed_step <= FIRST_RUN_SETUP_STEP_FINISHED) {
                select_step((first_run_setup_step_t)
                            (s_ui->displayed_step - 1));
            }
            break;
        case ACTION_PRIMARY: primary_action(); break;
        case ACTION_SKIP: skip_current(); break;
        case ACTION_WIFI_RESULT:
        case ACTION_TIMEZONE_RESULT:
        case ACTION_AIRSENSE_RESULT:
            result_action(action, value);
            break;
        case ACTION_SEARCH: search_action(); break;
        case ACTION_RETRY: retry_action(); break;
        default: break;
    }
    if (s_ui) s_ui->in_event = false;
}

static void render_all(void)
{
    if (!s_ui || !s_ui->root) return;
    /* Rebuilding the active detail pane while its PRESSED event is unwinding
     * would delete the event target. Defer only that rebuild to LVGL's next
     * timer pass; update/show calls still render synchronously. */
    if (s_ui->in_event) {
        if (!s_ui->render_pending) {
            s_ui->render_pending = lv_async_call(render_async, NULL) == LV_RES_OK;
        }
        return;
    }
    render_header();
    render_rail();
    render_detail();
    /* This is intentional: create/show always lay out the first complete frame
     * before it can be exposed, preventing the blank setup frame seen in QEMU. */
    lv_obj_update_layout(s_ui->root);
}

static void render_async(void *unused)
{
    (void)unused;
    if (!s_ui) return;
    s_ui->render_pending = false;
    render_all();
}

static void live_defaults(first_run_setup_ui_live_t *live)
{
    memset(live, 0, sizeof(*live));
    live->wifi_state = FIRST_RUN_SETUP_UI_WIFI_IDLE;
    live->time_state = FIRST_RUN_SETUP_UI_TIME_IDLE;
    live->airsense_state = FIRST_RUN_SETUP_UI_AIRSENSE_INSTRUCTIONS;
    live->card_state = FIRST_RUN_SETUP_UI_CARD_CHECKING;
}

static void make_shell(lv_obj_t *parent)
{
    s_ui->root = lv_obj_create(parent ? parent : lv_scr_act());
    lv_obj_set_pos(s_ui->root, 0, 0);
    lv_obj_set_size(s_ui->root, UI_WIDTH, UI_HEIGHT);
    style_surface(s_ui->root, COLOR_BASE, 0);
    lv_obj_clear_flag(s_ui->root, LV_OBJ_FLAG_SCROLLABLE);

    make_label(s_ui->root, "SomnoTrace setup", 22, 18, 260,
               FONT_SECTION, COLOR_TEXT);
    lv_obj_t *status = lv_obj_create(s_ui->root);
    lv_obj_set_pos(status, 413, 9);
    lv_obj_set_size(status, 593, 46);
    style_surface(status, COLOR_PANEL, 23);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_SCROLLABLE);
    s_ui->header_wifi = make_label(status, "", 18, 14, 210,
                                   FONT_SMALL, COLOR_TEXT);
    lv_label_set_long_mode(s_ui->header_wifi, LV_LABEL_LONG_DOT);
    s_ui->header_time = make_label(status, "", 235, 14, 160,
                                   FONT_SMALL, COLOR_TEXT);
    lv_label_set_long_mode(s_ui->header_time, LV_LABEL_LONG_DOT);
    s_ui->header_card = make_label(status, "", 402, 14, 173,
                                   FONT_SMALL, COLOR_TEXT);
    lv_label_set_long_mode(s_ui->header_card, LV_LABEL_LONG_DOT);

    s_ui->rail = lv_obj_create(s_ui->root);
    lv_obj_set_pos(s_ui->rail, UI_RAIL_X, UI_RAIL_Y);
    lv_obj_set_size(s_ui->rail, UI_RAIL_WIDTH, UI_DETAIL_HEIGHT);
    style_surface(s_ui->rail, COLOR_PANEL, 22);
    lv_obj_clear_flag(s_ui->rail, LV_OBJ_FLAG_SCROLLABLE);

    for (unsigned i = 0; i < FIRST_RUN_SETUP_STEP_COUNT; i++) {
        int y = 18 + (int)i * (UI_RAIL_ROW_HEIGHT + UI_RAIL_ROW_GAP);
        lv_obj_t *row = make_button(s_ui->rail, "", 12, y,
                                    UI_RAIL_WIDTH - 24, UI_RAIL_ROW_HEIGHT,
                                    false, ACTION_RAIL, i);
        lv_obj_set_style_radius(row, 17, 0);
        s_ui->rail_buttons[i] = row;
        s_ui->rail_dots[i] = lv_obj_create(row);
        lv_obj_set_pos(s_ui->rail_dots[i], 15, 23);
        lv_obj_set_size(s_ui->rail_dots[i], 12, 12);
        style_surface(s_ui->rail_dots[i], COLOR_DISABLED, 6);
        lv_obj_clear_flag(s_ui->rail_dots[i], LV_OBJ_FLAG_CLICKABLE);
        s_ui->rail_labels[i] = make_label(row, step_label(i), 39, 10,
                                          165, FONT_ROW, COLOR_TEXT);
        s_ui->rail_status[i] = make_label(row, "", 39, 35,
                                          165, FONT_SMALL, COLOR_TERTIARY);
    }

    s_ui->detail = lv_obj_create(s_ui->root);
    lv_obj_set_pos(s_ui->detail, UI_DETAIL_X, UI_DETAIL_Y);
    lv_obj_set_size(s_ui->detail, UI_DETAIL_WIDTH, UI_DETAIL_HEIGHT);
    style_surface(s_ui->detail, COLOR_PANEL, 22);
    lv_obj_clear_flag(s_ui->detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui->root, LV_OBJ_FLAG_HIDDEN);
}

static void complete_observed_steps(void)
{
    const bool observed[FIRST_RUN_SETUP_STEP_COUNT] = {
        [FIRST_RUN_SETUP_STEP_WIFI] = s_ui->live.wifi_configured ||
            s_ui->live.wifi_state == FIRST_RUN_SETUP_UI_WIFI_CONNECTED,
        [FIRST_RUN_SETUP_STEP_TIME] = s_ui->live.time_configured ||
            s_ui->live.time_state == FIRST_RUN_SETUP_UI_TIME_SET,
        [FIRST_RUN_SETUP_STEP_AIRSENSE] = s_ui->live.airsense_paired ||
            s_ui->live.airsense_state == FIRST_RUN_SETUP_UI_AIRSENSE_PAIRED,
        [FIRST_RUN_SETUP_STEP_CARD] =
            s_ui->live.card_state == FIRST_RUN_SETUP_UI_CARD_READY,
        [FIRST_RUN_SETUP_STEP_ALERTS] = s_ui->live.alerts_configured,
        [FIRST_RUN_SETUP_STEP_UPLOADS] = s_ui->live.uploads_configured,
    };

    refresh_durable();
    for (unsigned i = 0; i < FIRST_RUN_SETUP_STEP_COUNT; i++) {
        if (!observed[i] ||
            (s_ui->durable.state.completed_mask & STEP_BIT(i)) != 0) {
            continue;
        }
        esp_err_t err = first_run_setup_update(
            (first_run_setup_step_t)i, FIRST_RUN_SETUP_UPDATE_COMPLETE);
        if (err != ESP_OK) {
            set_action_error(err, "Saving completed setup step");
            ESP_LOGE(TAG, "could not persist completed step %u: %s",
                     i, esp_err_to_name(err));
            break;
        }
        refresh_durable();
    }
}

esp_err_t first_run_setup_ui_create(
    lv_obj_t *parent, const first_run_setup_ui_controller_t *controller)
{
    if (s_ui) return ESP_ERR_INVALID_STATE;

    first_run_setup_ui_t *ui = heap_caps_calloc(
        1, sizeof(*ui), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ui) ui = heap_caps_calloc(1, sizeof(*ui), MALLOC_CAP_8BIT);
    if (!ui) return ESP_ERR_NO_MEM;
    s_ui = ui;
    if (controller) s_ui->controller = *controller;
    live_defaults(&s_ui->live);
    s_ui->selected_wifi = -1;
    s_ui->selected_airsense = -1;
    refresh_durable();
    if (!s_ui->durable.schema_compatible) {
        free(s_ui);
        s_ui = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    s_ui->displayed_step = s_ui->durable.state.current_step;
    make_shell(parent);
    render_all();
    return ESP_OK;
}

void first_run_setup_ui_destroy(void)
{
    if (!s_ui) return;
    if (s_ui->render_pending) lv_async_call_cancel(render_async, NULL);
    if (s_ui->root) lv_obj_del(s_ui->root);
    first_run_setup_ui_t *released = s_ui;
    s_ui = NULL;
    memset(released, 0, sizeof(*released));
    free(released);
}

esp_err_t first_run_setup_ui_show(void)
{
    if (!s_ui || !s_ui->root) return ESP_ERR_INVALID_STATE;
    refresh_durable();
    if (s_ui->displayed_step > FIRST_RUN_SETUP_STEP_FINISHED) {
        s_ui->displayed_step = s_ui->durable.state.current_step;
    }
    render_all();
    lv_obj_clear_flag(s_ui->root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui->root);
    s_ui->visible = true;
    return ESP_OK;
}

void first_run_setup_ui_hide(void)
{
    if (!s_ui || !s_ui->root) return;
    lv_obj_add_flag(s_ui->root, LV_OBJ_FLAG_HIDDEN);
    s_ui->visible = false;
}

bool first_run_setup_ui_is_visible(void)
{
    return s_ui && s_ui->visible;
}

esp_err_t first_run_setup_ui_update(
    const first_run_setup_ui_live_t *snapshot)
{
    if (!s_ui || !snapshot) return ESP_ERR_INVALID_ARG;
    char selected_ssid[sizeof(s_ui->live.selected_ssid)];
    char selected_airsense[sizeof(s_ui->live.selected_airsense_address)];
    copy_text(selected_ssid, sizeof(selected_ssid),
              s_ui->live.selected_ssid);
    copy_text(selected_airsense, sizeof(selected_airsense),
              s_ui->live.selected_airsense_address);
    s_ui->live = *snapshot;
    s_ui->live.wifi_result_count = (uint8_t)bounded_count(
        s_ui->live.wifi_result_count, FIRST_RUN_SETUP_UI_WIFI_RESULT_MAX);
    s_ui->live.timezone_result_count = (uint8_t)bounded_count(
        s_ui->live.timezone_result_count,
        FIRST_RUN_SETUP_UI_TIMEZONE_RESULT_MAX);
    s_ui->live.airsense_result_count = (uint8_t)bounded_count(
        s_ui->live.airsense_result_count,
        FIRST_RUN_SETUP_UI_AIRSENSE_RESULT_MAX);
    if (!s_ui->live.selected_ssid[0] &&
        (s_ui->live.wifi_state == FIRST_RUN_SETUP_UI_WIFI_PASSWORD ||
         s_ui->live.wifi_state == FIRST_RUN_SETUP_UI_WIFI_CONNECTING ||
         s_ui->live.wifi_state == FIRST_RUN_SETUP_UI_WIFI_AUTH_FAILED)) {
        copy_text(s_ui->live.selected_ssid,
                  sizeof(s_ui->live.selected_ssid), selected_ssid);
    }
    if (!s_ui->live.selected_airsense_address[0] &&
        s_ui->live.airsense_state >= FIRST_RUN_SETUP_UI_AIRSENSE_STARTING &&
        s_ui->live.airsense_state <= FIRST_RUN_SETUP_UI_AIRSENSE_CODE_REJECTED) {
        copy_text(s_ui->live.selected_airsense_address,
                  sizeof(s_ui->live.selected_airsense_address),
                  selected_airsense);
    }
    s_ui->selected_wifi = -1;
    for (unsigned i = 0; i < s_ui->live.wifi_result_count; i++) {
        if (s_ui->live.selected_ssid[0] &&
            strcmp(s_ui->live.selected_ssid,
                   s_ui->live.wifi_results[i].ssid) == 0) {
            s_ui->selected_wifi = (int)i;
            break;
        }
    }
    s_ui->selected_airsense = -1;
    for (unsigned i = 0; i < s_ui->live.airsense_result_count; i++) {
        if (s_ui->live.selected_airsense_address[0] &&
            strcmp(s_ui->live.selected_airsense_address,
                   s_ui->live.airsense_results[i].address) == 0) {
            s_ui->selected_airsense = (int)i;
            break;
        }
    }
    s_ui->action_error[0] = '\0';
    complete_observed_steps();
    if (s_ui->visible) render_all();
    return ESP_OK;
}

void first_run_setup_ui_snapshot(first_run_setup_ui_snapshot_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_ui) return;
    out->durable = s_ui->durable;
    out->live = s_ui->live;
    out->displayed_step = s_ui->displayed_step;
    out->created = true;
    out->visible = s_ui->visible;
}

lv_obj_t *first_run_setup_ui_root(void)
{
    return s_ui ? s_ui->root : NULL;
}
