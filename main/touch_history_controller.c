/* Single-worker, generation-safe native History controller. */
#include "touch_history_controller.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "psram_task.h"

#define HISTORY_CONTROLLER_QUEUE_LENGTH 1U
#define HISTORY_CONTROLLER_WORKER_STACK 12288U
#define HISTORY_CONTROLLER_EVENT_PAGE 32U
#define HISTORY_CONTROLLER_ZOOM_90_MIN_MS (90LL * 60LL * 1000LL)
#define HISTORY_CONTROLLER_ZOOM_22_MIN_MS (22LL * 60LL * 1000LL)
#define HISTORY_CONTROLLER_ZOOM_10_MIN_MS (10LL * 60LL * 1000LL)
#define HISTORY_CONTROLLER_ZOOM_5_MIN_MS  (5LL * 60LL * 1000LL)

typedef enum {
    HISTORY_JOB_INITIAL = 0,
    HISTORY_JOB_PAGE,
    HISTORY_JOB_DAY,
    HISTORY_JOB_VIEW,
    HISTORY_JOB_MONTH,
    HISTORY_JOB_STOP,
} history_job_kind_t;

typedef struct {
    history_job_kind_t kind;
    uint32_t generation;
    size_t page_offset;
    size_t global_index;
    touch_history_signal_t signal;
    int64_t window_start_ms;
    int64_t window_end_ms;
    uint16_t year;
    uint8_t month;
    bool therapy_only;
    bool non_cancellable;
    char day[9];
} history_job_t;

typedef struct {
    touch_history_day_t days[TOUCH_HISTORY_UI_LIST_ROWS];
    touch_history_index_page_t page;
    size_t selected_row;
    size_t selected_global_index;

    touch_history_night_t night;
    touch_history_session_t sessions[TOUCH_HISTORY_UI_MAX_SESSIONS];
    size_t session_count;
    touch_history_overview_t overview;
    touch_history_event_t events[TOUCH_HISTORY_UI_MAX_VISIBLE_EVENTS];
    size_t event_count;
    touch_history_stats_t stats;
    touch_history_month_t month;

    touch_history_ui_state_t state;
    touch_history_signal_t signal;
    int64_t window_start_ms;
    int64_t window_end_ms;
    int64_t cursor_ms;
    uint16_t progress_per_mille;
    bool has_night;
    bool has_overview;
    bool has_month;
    bool cursor_valid;
    bool therapy_only;
    bool usage_target_known;
    bool usage_on_target;
    bool events_truncated;
    char status[TOUCH_HISTORY_UI_TEXT_MAX];
    char error[TOUCH_HISTORY_UI_TEXT_MAX];
    char degraded[TOUCH_HISTORY_UI_TEXT_MAX];
} history_model_t;

/* Small LVGL-side decision snapshot. The 480-bin overview and event/session
 * arrays never leave the PSRAM model merely to interpret a touch intent. */
typedef struct {
    touch_history_day_t days[TOUCH_HISTORY_UI_LIST_ROWS];
    touch_history_index_page_t page;
    touch_history_night_t night;
    touch_history_month_t month;
    size_t selected_global_index;
    touch_history_signal_t signal;
    int64_t window_start_ms;
    int64_t window_end_ms;
    int64_t cursor_ms;
    bool has_night;
    bool has_month;
    bool cursor_valid;
    bool therapy_only;
} history_navigation_t;

struct touch_history_controller {
    history_model_t model;
    touch_history_controller_config_t config;
    StaticSemaphore_t mutex_storage;
    SemaphoreHandle_t mutex;
    StaticQueue_t queue_storage;
    QueueHandle_t queue;
    uint8_t queue_bytes[HISTORY_CONTROLLER_QUEUE_LENGTH *
                        sizeof(history_job_t)];
    TaskHandle_t worker;
    history_job_t retry_job;
    uint32_t generation;
    uint32_t revision;
    uint32_t rendered_revision;
    bool active;
    bool ever_loaded;
    bool closing;
};

typedef struct {
    touch_history_controller_t *controller;
    uint32_t generation;
    uint16_t base;
    uint16_t span;
} history_operation_context_t;

static void history_controller_notify(touch_history_controller_t *controller)
{
    if (controller && controller->config.changed)
        controller->config.changed(controller->config.context);
}

static void history_controller_text(char *out, size_t size, const char *text)
{
    if (!out || !size) return;
    snprintf(out, size, "%s", text ? text : "");
}

static bool history_controller_generation_current(
    touch_history_controller_t *controller, uint32_t generation)
{
    bool current = false;
    if (!controller || xSemaphoreTake(controller->mutex, portMAX_DELAY) != pdTRUE)
        return false;
    current = !controller->closing && controller->active &&
              controller->generation == generation;
    xSemaphoreGive(controller->mutex);
    return current;
}

static bool history_controller_should_cancel(void *context)
{
    history_operation_context_t *operation = context;
    return !operation || !history_controller_generation_current(
        operation->controller, operation->generation);
}

static void history_controller_progress(void *context, uint16_t per_mille)
{
    history_operation_context_t *operation = context;
    if (!operation || !operation->controller) return;
    touch_history_controller_t *controller = operation->controller;
    uint16_t mapped = operation->base + (uint16_t)(
        (uint32_t)operation->span * per_mille / 1000U);
    bool changed = false;
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
        if (!controller->closing && controller->active &&
            controller->generation == operation->generation &&
            controller->model.progress_per_mille < mapped) {
            controller->model.progress_per_mille = mapped;
            controller->revision++;
            changed = true;
        }
        xSemaphoreGive(controller->mutex);
    }
    if (changed) history_controller_notify(controller);
}

static touch_history_operation_t history_controller_operation(
    history_operation_context_t *context,
    touch_history_controller_t *controller, uint32_t generation,
    uint16_t base, uint16_t span)
{
    *context = (history_operation_context_t) {
        .controller = controller,
        .generation = generation,
        .base = base,
        .span = span,
    };
    return (touch_history_operation_t) {
        .should_cancel = history_controller_should_cancel,
        .progress = history_controller_progress,
        .context = context,
    };
}

static void history_controller_begin_job(touch_history_controller_t *controller,
                                         const history_job_t *job)
{
    bool notify = false;
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
        if (!controller->closing && controller->generation == job->generation) {
            controller->model.state = job->non_cancellable
                ? TOUCH_HISTORY_UI_STATE_ZOOM_LOADING
                : TOUCH_HISTORY_UI_STATE_AUTO_LOADING;
            controller->model.progress_per_mille = 0;
            controller->model.error[0] = '\0';
            history_controller_text(
                controller->model.status, sizeof(controller->model.status),
                job->non_cancellable ? "Reading selected window…"
                                     : "Loading History…");
            controller->revision++;
            notify = true;
        }
        xSemaphoreGive(controller->mutex);
    }
    if (notify) history_controller_notify(controller);
}

static bool history_controller_copy_model(touch_history_controller_t *controller,
                                          history_model_t *result)
{
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) != pdTRUE)
        return false;
    *result = controller->model;
    xSemaphoreGive(controller->mutex);
    return true;
}

static bool history_controller_publish(touch_history_controller_t *controller,
                                       const history_job_t *job,
                                       const history_model_t *result)
{
    bool published = false;
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) != pdTRUE)
        return false;
    if (!controller->closing && controller->active &&
        controller->generation == job->generation) {
        controller->model = *result;
        controller->model.progress_per_mille = 1000;
        controller->retry_job = *job;
        controller->ever_loaded = true;
        controller->revision++;
        published = true;
    }
    xSemaphoreGive(controller->mutex);
    if (published) history_controller_notify(controller);
    return published;
}

static void history_controller_publish_error(
    touch_history_controller_t *controller, const history_job_t *job,
    esp_err_t error)
{
    bool notify = false;
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) != pdTRUE) return;
    if (!controller->closing && controller->active &&
        controller->generation == job->generation) {
        controller->model.state = TOUCH_HISTORY_UI_STATE_READ_ERROR;
        controller->model.progress_per_mille = 0;
        controller->model.status[0] = '\0';
        if (error == ESP_ERR_NOT_FOUND) {
            history_controller_text(controller->model.error,
                                    sizeof(controller->model.error),
                                    "No readable History data was found.");
        } else if (error == ESP_ERR_NO_MEM) {
            history_controller_text(controller->model.error,
                                    sizeof(controller->model.error),
                                    "History ran out of external memory.");
        } else if (error == ESP_ERR_TIMEOUT || error == ESP_ERR_INVALID_STATE) {
            history_controller_text(controller->model.error,
                                    sizeof(controller->model.error),
                                    "The card is busy. Try again in a moment.");
        } else {
            history_controller_text(controller->model.error,
                                    sizeof(controller->model.error),
                                    "Could not read the microSD card.");
        }
        controller->retry_job = *job;
        controller->revision++;
        notify = true;
    }
    xSemaphoreGive(controller->mutex);
    if (notify) history_controller_notify(controller);
}

static touch_history_signal_t history_controller_first_signal(uint16_t mask)
{
    for (int signal = 0; signal < TOUCH_HISTORY_SIGNAL_COUNT; ++signal) {
        if (mask & TOUCH_HISTORY_SIGNAL_BIT(signal))
            return (touch_history_signal_t)signal;
    }
    return TOUCH_HISTORY_SIGNAL_FLOW;
}

static bool history_controller_signal_available(const history_model_t *model,
                                                touch_history_signal_t signal)
{
    return model->has_night && signal >= TOUCH_HISTORY_SIGNAL_FLOW &&
           signal < TOUCH_HISTORY_SIGNAL_COUNT &&
           (model->night.available_signals &
            TOUCH_HISTORY_SIGNAL_BIT(signal)) != 0;
}

static void history_controller_update_selection(history_model_t *model,
                                                const char day[9],
                                                size_t global_index)
{
    model->selected_row = SIZE_MAX;
    model->selected_global_index = global_index;
    for (size_t i = 0; i < model->page.returned; ++i) {
        if (!strcmp(model->days[i].day, day)) {
            model->selected_row = i;
            if (global_index == SIZE_MAX)
                model->selected_global_index = model->page.offset + i;
            break;
        }
    }
}

static esp_err_t history_controller_load_events(
    const history_job_t *job, history_model_t *model,
    const touch_history_operation_t *operation)
{
    model->event_count = 0;
    model->events_truncated = false;
    size_t offset = 0;
    for (;;) {
        touch_history_event_t page_events[HISTORY_CONTROLLER_EVENT_PAGE];
        touch_history_event_page_t page = {0};
        esp_err_t result = touch_history_load_events_ex(
            job->day, offset, page_events, HISTORY_CONTROLLER_EVENT_PAGE,
            &page, operation);
        if (result == ESP_ERR_NOT_FOUND) return ESP_OK;
        if (result != ESP_OK) return result;
        for (size_t i = 0; i < page.returned; ++i) {
            const touch_history_event_t *event = &page_events[i];
            if (event->end_ms < model->window_start_ms ||
                event->start_ms >= model->window_end_ms) continue;
            if (model->event_count < TOUCH_HISTORY_UI_MAX_VISIBLE_EVENTS) {
                model->events[model->event_count++] = *event;
            } else {
                model->events_truncated = true;
            }
        }
        if (!page.has_more || page.returned == 0) break;
        if (offset > SIZE_MAX - page.returned) return ESP_FAIL;
        offset += page.returned;
    }
    return ESP_OK;
}

static esp_err_t history_controller_load_view(
    touch_history_controller_t *controller, const history_job_t *job,
    history_model_t *model, bool load_night)
{
    history_operation_context_t operation_context;
    touch_history_operation_t operation = history_controller_operation(
        &operation_context, controller, job->generation, 50, 900);
    const touch_history_operation_t *cancellable = job->non_cancellable
        ? NULL : &operation;

    if (load_night) {
        memset(&model->night, 0, sizeof(model->night));
        memset(model->sessions, 0, sizeof(model->sessions));
        esp_err_t night_result = touch_history_load_night_ex(
            job->day, &model->night, model->sessions,
            TOUCH_HISTORY_UI_MAX_SESSIONS, cancellable);
        if (night_result != ESP_OK) return night_result;
        model->has_night = true;
        model->session_count = model->night.sessions_returned;
        history_controller_update_selection(model, job->day,
                                            job->global_index);
        if (!history_controller_signal_available(model, job->signal))
            model->signal = history_controller_first_signal(
                model->night.available_signals);
        else
            model->signal = job->signal;
        model->therapy_only = job->therapy_only &&
                              model->signal == TOUCH_HISTORY_SIGNAL_SPO2;
        model->window_start_ms = job->window_start_ms > 0
            ? job->window_start_ms : model->night.axis_start_ms;
        model->window_end_ms = job->window_end_ms > model->window_start_ms
            ? job->window_end_ms : model->night.axis_end_ms;
        if (!model->cursor_valid ||
            model->cursor_ms < model->night.axis_start_ms ||
            model->cursor_ms >= model->night.axis_end_ms) {
            model->cursor_ms = model->window_start_ms +
                (model->window_end_ms - model->window_start_ms) / 2;
            model->cursor_valid = true;
        }
    } else {
        model->signal = job->signal;
        model->therapy_only = job->therapy_only &&
                              job->signal == TOUCH_HISTORY_SIGNAL_SPO2;
        model->window_start_ms = job->window_start_ms;
        model->window_end_ms = job->window_end_ms;
    }
    if (!model->has_night || model->window_start_ms < model->night.axis_start_ms ||
        model->window_end_ms > model->night.axis_end_ms ||
        model->window_end_ms <= model->window_start_ms)
        return ESP_ERR_INVALID_ARG;

    touch_history_overview_t *next_overview = &model->overview;
    esp_err_t graph_result;
    bool fit = model->window_start_ms == model->night.axis_start_ms &&
               model->window_end_ms == model->night.axis_end_ms;
    graph_result = touch_history_load_view_ex(
        job->day, model->signal,
        fit ? 0 : model->window_start_ms,
        fit ? 0 : model->window_end_ms,
        model->therapy_only, next_overview, cancellable);
    if (graph_result != ESP_OK) return graph_result;

    touch_history_stats_t *next_stats = &model->stats;
    esp_err_t stats_result = touch_history_load_stats_ex(
        job->day, model->signal, model->window_start_ms,
        model->window_end_ms, model->therapy_only, next_stats,
        cancellable);
    if (stats_result != ESP_OK && stats_result != ESP_ERR_NOT_FOUND)
        return stats_result;

    esp_err_t events_result = history_controller_load_events(
        job, model, cancellable);
    if (events_result != ESP_OK) return events_result;

    model->has_overview = true;
    model->state = TOUCH_HISTORY_UI_STATE_READY;
    model->status[0] = '\0';
    model->error[0] = '\0';
    model->degraded[0] = '\0';
    if (model->night.sessions_truncated) {
        model->state = TOUCH_HISTORY_UI_STATE_DEGRADED_UNKNOWN;
        history_controller_text(model->degraded, sizeof(model->degraded),
                                "Some session captions could not be shown.");
    } else if (model->events_truncated) {
        model->state = TOUCH_HISTORY_UI_STATE_DEGRADED_UNKNOWN;
        history_controller_text(model->degraded, sizeof(model->degraded),
                                "Additional event markers are outside this display limit.");
    } else if (stats_result == ESP_ERR_NOT_FOUND &&
               model->signal != TOUCH_HISTORY_SIGNAL_MOTION) {
        model->state = TOUCH_HISTORY_UI_STATE_DEGRADED_UNKNOWN;
        history_controller_text(model->degraded, sizeof(model->degraded),
                                "Exact source statistics are unavailable.");
    }

    const touch_history_day_t *selected = model->selected_row < model->page.returned
        ? &model->days[model->selected_row] : NULL;
    model->usage_target_known = controller->config.usage_target_minutes > 0;
    model->usage_on_target = selected && selected->has_usage &&
        selected->usage_min >= controller->config.usage_target_minutes;
    return ESP_OK;
}

static esp_err_t history_controller_load_page(const history_job_t *job,
                                              history_model_t *model)
{
    touch_history_day_t days[TOUCH_HISTORY_UI_LIST_ROWS] = {0};
    touch_history_index_page_t page = {0};
    esp_err_t result = touch_history_load_page(
        job->page_offset, days, TOUCH_HISTORY_UI_LIST_ROWS, &page);
    if (result != ESP_OK) return result;
    memcpy(model->days, days, sizeof(days));
    model->page = page;
    model->selected_row = SIZE_MAX;
    if (model->has_night)
        history_controller_update_selection(
            model, model->night.day, model->selected_global_index);
    return ESP_OK;
}

static void history_controller_month_from_day(const char day[9],
                                              uint16_t *year, uint8_t *month)
{
    if (!day || strlen(day) != 8) return;
    *year = (uint16_t)((day[0] - '0') * 1000 + (day[1] - '0') * 100 +
                       (day[2] - '0') * 10 + day[3] - '0');
    *month = (uint8_t)((day[4] - '0') * 10 + day[5] - '0');
}

static esp_err_t history_controller_process(
    touch_history_controller_t *controller, const history_job_t *job,
    history_model_t *result)
{
    if (!history_controller_copy_model(controller, result))
        return ESP_FAIL;
    switch (job->kind) {
    case HISTORY_JOB_INITIAL: {
        memset(result, 0, sizeof(*result));
        result->selected_row = SIZE_MAX;
        result->selected_global_index = SIZE_MAX;
        result->signal = job->signal;
        esp_err_t load = history_controller_load_page(job, result);
        if (load == ESP_ERR_NOT_FOUND) {
            result->state = TOUCH_HISTORY_UI_STATE_EMPTY;
            history_controller_text(result->status, sizeof(result->status),
                                    "No recorded nights were found.");
            return ESP_OK;
        }
        if (load != ESP_OK) return load;
        if (!result->page.returned) return ESP_ERR_NOT_FOUND;
        history_job_t detail = *job;
        detail.kind = HISTORY_JOB_DAY;
        detail.global_index = 0;
        memcpy(detail.day, result->days[0].day, sizeof(detail.day));
        load = history_controller_load_view(controller, &detail, result, true);
        if (load != ESP_OK) return load;
        uint16_t year = 0;
        uint8_t month = 0;
        history_controller_month_from_day(detail.day, &year, &month);
        if (year && month &&
            touch_history_load_month(year, month, &result->month) == ESP_OK)
            result->has_month = true;
        return ESP_OK;
    }
    case HISTORY_JOB_PAGE: {
        esp_err_t page_result = history_controller_load_page(job, result);
        if (page_result != ESP_OK) return page_result;
        if (job->global_index != SIZE_MAX &&
            job->global_index >= result->page.offset &&
            job->global_index - result->page.offset < result->page.returned) {
            history_job_t detail = *job;
            detail.kind = HISTORY_JOB_DAY;
            size_t row = job->global_index - result->page.offset;
            memcpy(detail.day, result->days[row].day, sizeof(detail.day));
            return history_controller_load_view(
                controller, &detail, result, true);
        }
        result->state = result->has_night ? TOUCH_HISTORY_UI_STATE_READY
                                         : TOUCH_HISTORY_UI_STATE_EMPTY;
        result->status[0] = '\0';
        return ESP_OK;
    }
    case HISTORY_JOB_DAY: {
        history_job_t resolved = *job;
        if (resolved.global_index == SIZE_MAX) {
            size_t total_days = 0;
            esp_err_t found = touch_history_find_day_index(
                resolved.day, &resolved.global_index, &total_days);
            if (found != ESP_OK) return found;
            resolved.page_offset =
                (resolved.global_index / TOUCH_HISTORY_UI_LIST_ROWS) *
                TOUCH_HISTORY_UI_LIST_ROWS;
            result->page.total_days = total_days;
        }
        if (resolved.page_offset != result->page.offset) {
            esp_err_t page = history_controller_load_page(&resolved, result);
            if (page != ESP_OK) return page;
        }
        return history_controller_load_view(
            controller, &resolved, result, true);
    }
    case HISTORY_JOB_VIEW:
        return history_controller_load_view(controller, job, result, false);
    case HISTORY_JOB_MONTH: {
        esp_err_t month = touch_history_load_month(
            job->year, job->month, &result->month);
        if (month != ESP_OK) return month;
        result->has_month = true;
        result->state = TOUCH_HISTORY_UI_STATE_CALENDAR;
        result->status[0] = '\0';
        return ESP_OK;
    }
    case HISTORY_JOB_STOP:
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

static void history_controller_worker(void *argument)
{
    touch_history_controller_t *controller = argument;
    history_job_t job;
    while (xQueueReceive(controller->queue, &job, portMAX_DELAY) == pdTRUE) {
        if (job.kind == HISTORY_JOB_STOP) break;
        if (!history_controller_generation_current(controller, job.generation))
            continue;
        history_controller_begin_job(controller, &job);
        history_model_t *result = heap_caps_malloc(
            sizeof(*result), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!result) {
            history_controller_publish_error(controller, &job, ESP_ERR_NO_MEM);
            continue;
        }
        esp_err_t status = history_controller_process(controller, &job, result);
        if (status == ESP_OK) {
            (void)history_controller_publish(controller, &job, result);
        } else if (status != TOUCH_HISTORY_ERR_CANCELLED &&
                   history_controller_generation_current(
                       controller, job.generation)) {
            history_controller_publish_error(controller, &job, status);
        }
        heap_caps_free(result);
    }
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
        controller->worker = NULL;
        xSemaphoreGive(controller->mutex);
    }
    psram_task_delete(NULL);
}

static esp_err_t history_controller_enqueue(
    touch_history_controller_t *controller, history_job_t *job)
{
    if (!controller || !job) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) != pdTRUE)
        return ESP_FAIL;
    if (controller->closing || !controller->active) {
        xSemaphoreGive(controller->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    controller->generation++;
    if (!controller->generation) controller->generation = 1;
    job->generation = controller->generation;
    controller->retry_job = *job;
    xSemaphoreGive(controller->mutex);
    return xQueueOverwrite(controller->queue, job) == pdPASS
        ? ESP_OK : ESP_FAIL;
}

esp_err_t touch_history_controller_create(
    const touch_history_controller_config_t *config,
    touch_history_controller_t **out_controller)
{
    if (!out_controller) return ESP_ERR_INVALID_ARG;
    *out_controller = NULL;
    touch_history_controller_t *controller = heap_caps_calloc(
        1, sizeof(*controller), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!controller) return ESP_ERR_NO_MEM;
    if (config) controller->config = *config;
    controller->model.selected_row = SIZE_MAX;
    controller->model.selected_global_index = SIZE_MAX;
    controller->model.signal = TOUCH_HISTORY_SIGNAL_FLOW;
    controller->model.state = TOUCH_HISTORY_UI_STATE_EMPTY;
    controller->mutex = xSemaphoreCreateMutexStatic(&controller->mutex_storage);
    controller->queue = xQueueCreateStatic(
        HISTORY_CONTROLLER_QUEUE_LENGTH, sizeof(history_job_t),
        controller->queue_bytes, &controller->queue_storage);
    if (!controller->mutex || !controller->queue) {
        heap_caps_free(controller);
        return ESP_ERR_NO_MEM;
    }
    controller->worker = psram_task_create(
        history_controller_worker, "ui_history", HISTORY_CONTROLLER_WORKER_STACK,
        controller, 3, 0, NULL, NULL);
    if (!controller->worker) {
        heap_caps_free(controller);
        return ESP_ERR_NO_MEM;
    }
    *out_controller = controller;
    return ESP_OK;
}

void touch_history_controller_destroy(touch_history_controller_t *controller)
{
    if (!controller) return;
    history_job_t stop = {.kind = HISTORY_JOB_STOP};
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
        controller->closing = true;
        controller->active = false;
        controller->generation++;
        xSemaphoreGive(controller->mutex);
    }
    xQueueReset(controller->queue);
    (void)xQueueSend(controller->queue, &stop, portMAX_DELAY);
    for (;;) {
        TaskHandle_t worker = NULL;
        if (xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
            worker = controller->worker;
            xSemaphoreGive(controller->mutex);
        }
        if (!worker) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    heap_caps_free(controller);
}

esp_err_t touch_history_controller_set_active(
    touch_history_controller_t *controller, bool active)
{
    if (!controller) return ESP_ERR_INVALID_ARG;
    bool load = false;
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) != pdTRUE)
        return ESP_FAIL;
    if (controller->closing) {
        xSemaphoreGive(controller->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    controller->active = active;
    if (!active) {
        controller->generation++;
        controller->model.progress_per_mille = 0;
    } else if (!controller->ever_loaded ||
               controller->model.state == TOUCH_HISTORY_UI_STATE_EMPTY ||
               controller->model.state == TOUCH_HISTORY_UI_STATE_READ_ERROR) {
        load = true;
    }
    xSemaphoreGive(controller->mutex);
    if (!load) return ESP_OK;
    history_job_t job = {
        .kind = HISTORY_JOB_INITIAL,
        .page_offset = 0,
        .global_index = 0,
        .signal = TOUCH_HISTORY_SIGNAL_FLOW,
    };
    return history_controller_enqueue(controller, &job);
}

static bool history_controller_get_navigation(
    touch_history_controller_t *controller, history_navigation_t *navigation)
{
    if (!controller || !navigation ||
        xSemaphoreTake(controller->mutex, portMAX_DELAY) != pdTRUE)
        return false;
    memcpy(navigation->days, controller->model.days,
           sizeof(navigation->days));
    navigation->page = controller->model.page;
    navigation->night = controller->model.night;
    navigation->month = controller->model.month;
    navigation->selected_global_index =
        controller->model.selected_global_index;
    navigation->signal = controller->model.signal;
    navigation->window_start_ms = controller->model.window_start_ms;
    navigation->window_end_ms = controller->model.window_end_ms;
    navigation->cursor_ms = controller->model.cursor_ms;
    navigation->has_night = controller->model.has_night;
    navigation->has_month = controller->model.has_month;
    navigation->cursor_valid = controller->model.cursor_valid;
    navigation->therapy_only = controller->model.therapy_only;
    xSemaphoreGive(controller->mutex);
    return true;
}

static void history_controller_queue_day(
    touch_history_controller_t *controller, const history_navigation_t *model,
    const char day[9], size_t global_index, size_t page_offset)
{
    history_job_t job = {
        .kind = HISTORY_JOB_DAY,
        .page_offset = page_offset,
        .global_index = global_index,
        .signal = model->signal,
        .therapy_only = model->therapy_only,
    };
    memcpy(job.day, day, sizeof(job.day));
    (void)history_controller_enqueue(controller, &job);
}

static int64_t history_controller_zoom_span(int64_t current,
                                           int64_t night_span,
                                           int direction)
{
    if (direction <= 0) {
        if (current >= night_span) return night_span;
        if (current < HISTORY_CONTROLLER_ZOOM_10_MIN_MS)
            return HISTORY_CONTROLLER_ZOOM_10_MIN_MS;
        if (current < HISTORY_CONTROLLER_ZOOM_22_MIN_MS)
            return HISTORY_CONTROLLER_ZOOM_22_MIN_MS;
        if (current < HISTORY_CONTROLLER_ZOOM_90_MIN_MS)
            return HISTORY_CONTROLLER_ZOOM_90_MIN_MS;
        int64_t quarter = night_span / 4;
        if (current < quarter) return quarter;
        return night_span;
    }
    int64_t quarter = night_span / 4;
    if (current > quarter && quarter > HISTORY_CONTROLLER_ZOOM_90_MIN_MS)
        return quarter;
    if (current > HISTORY_CONTROLLER_ZOOM_90_MIN_MS)
        return HISTORY_CONTROLLER_ZOOM_90_MIN_MS;
    if (current > HISTORY_CONTROLLER_ZOOM_22_MIN_MS)
        return HISTORY_CONTROLLER_ZOOM_22_MIN_MS;
    if (current > HISTORY_CONTROLLER_ZOOM_10_MIN_MS)
        return HISTORY_CONTROLLER_ZOOM_10_MIN_MS;
    return current > HISTORY_CONTROLLER_ZOOM_5_MIN_MS
        ? HISTORY_CONTROLLER_ZOOM_5_MIN_MS : current;
}

static void history_controller_clamp_window(const history_navigation_t *model,
                                            int64_t centre, int64_t span,
                                            int64_t *start, int64_t *end)
{
    int64_t night_span = model->night.axis_end_ms - model->night.axis_start_ms;
    if (span >= night_span) {
        *start = model->night.axis_start_ms;
        *end = model->night.axis_end_ms;
        return;
    }
    *start = centre - span / 2;
    *end = *start + span;
    if (*start < model->night.axis_start_ms) {
        *start = model->night.axis_start_ms;
        *end = *start + span;
    }
    if (*end > model->night.axis_end_ms) {
        *end = model->night.axis_end_ms;
        *start = *end - span;
    }
}

static void history_controller_queue_view(
    touch_history_controller_t *controller, const history_navigation_t *model,
    touch_history_signal_t signal, int64_t start_ms, int64_t end_ms,
    bool therapy_only, bool non_cancellable)
{
    history_job_t job = {
        .kind = HISTORY_JOB_VIEW,
        .signal = signal,
        .window_start_ms = start_ms,
        .window_end_ms = end_ms,
        .therapy_only = therapy_only,
        .non_cancellable = non_cancellable,
        .global_index = model->selected_global_index,
        .page_offset = model->page.offset,
    };
    memcpy(job.day, model->night.day, sizeof(job.day));
    (void)history_controller_enqueue(controller, &job);
}

void touch_history_controller_handle_intent(
    void *context, const touch_history_ui_intent_t *intent)
{
    touch_history_controller_t *controller = context;
    if (!controller || !intent) return;
    if (intent->type == TOUCH_HISTORY_UI_INTENT_OPEN_CARD) {
        if (controller->config.route_card)
            controller->config.route_card(controller->config.context);
        return;
    }
    if (intent->type == TOUCH_HISTORY_UI_INTENT_CANCEL_AUTO_LOAD) {
        bool notify = false;
        if (xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
            if (controller->model.state == TOUCH_HISTORY_UI_STATE_AUTO_LOADING) {
                controller->generation++;
                controller->model.state = controller->model.has_night
                    ? TOUCH_HISTORY_UI_STATE_READY
                    : TOUCH_HISTORY_UI_STATE_EMPTY;
                controller->model.progress_per_mille = 0;
                controller->model.status[0] = '\0';
                controller->revision++;
                notify = true;
            }
            xSemaphoreGive(controller->mutex);
        }
        if (notify) history_controller_notify(controller);
        return;
    }

    history_navigation_t model;
    if (!history_controller_get_navigation(controller, &model)) return;
    switch (intent->type) {
    case TOUCH_HISTORY_UI_INTENT_SELECT_DAY:
        if (intent->row_index < model.page.returned)
            history_controller_queue_day(
                controller, &model, model.days[intent->row_index].day,
                model.page.offset + intent->row_index, model.page.offset);
        break;
    case TOUCH_HISTORY_UI_INTENT_PAGE_RELATIVE: {
        int64_t offset = (int64_t)model.page.offset +
            intent->relative * TOUCH_HISTORY_UI_LIST_ROWS;
        if (offset < 0) offset = 0;
        if ((size_t)offset >= model.page.total_days && model.page.total_days)
            offset = (int64_t)(((model.page.total_days - 1U) /
                      TOUCH_HISTORY_UI_LIST_ROWS) *
                      TOUCH_HISTORY_UI_LIST_ROWS);
        history_job_t job = {
            .kind = HISTORY_JOB_PAGE,
            .page_offset = (size_t)offset,
            .signal = model.signal,
            .global_index = SIZE_MAX,
        };
        (void)history_controller_enqueue(controller, &job);
        break;
    }
    case TOUCH_HISTORY_UI_INTENT_OPEN_CALENDAR: {
        uint16_t year = model.has_month ? model.month.year : 0;
        uint8_t month = model.has_month ? model.month.month : 0;
        if ((!year || !month) && model.has_night)
            history_controller_month_from_day(model.night.day, &year, &month);
        if (year && month) {
            history_job_t job = {
                .kind = HISTORY_JOB_MONTH, .year = year, .month = month,
                .signal = model.signal,
                .global_index = SIZE_MAX,
            };
            (void)history_controller_enqueue(controller, &job);
        }
        break;
    }
    case TOUCH_HISTORY_UI_INTENT_CLOSE_CALENDAR: {
        bool notify = false;
        if (xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
            controller->generation++;
            controller->model.state = controller->model.has_night
                ? TOUCH_HISTORY_UI_STATE_READY : TOUCH_HISTORY_UI_STATE_EMPTY;
            controller->revision++;
            notify = true;
            xSemaphoreGive(controller->mutex);
        }
        if (notify) history_controller_notify(controller);
        break;
    }
    case TOUCH_HISTORY_UI_INTENT_MONTH_RELATIVE: {
        int year = model.month.year;
        int month = model.month.month + (int)intent->relative;
        while (month < 1) { month += 12; year--; }
        while (month > 12) { month -= 12; year++; }
        if (year >= 2000 && year <= 2200) {
            history_job_t job = {
                .kind = HISTORY_JOB_MONTH, .year = (uint16_t)year,
                .month = (uint8_t)month, .signal = model.signal,
                .global_index = SIZE_MAX,
            };
            (void)history_controller_enqueue(controller, &job);
        }
        break;
    }
    case TOUCH_HISTORY_UI_INTENT_SELECT_CALENDAR_DAY:
        if (intent->day[0])
            history_controller_queue_day(controller, &model, intent->day,
                                         SIZE_MAX, model.page.offset);
        break;
    case TOUCH_HISTORY_UI_INTENT_PREVIOUS_NIGHT:
    case TOUCH_HISTORY_UI_INTENT_NEXT_NIGHT: {
        if (model.selected_global_index == SIZE_MAX) break;
        size_t target = model.selected_global_index;
        if (intent->type == TOUCH_HISTORY_UI_INTENT_PREVIOUS_NIGHT) {
            if (target + 1U >= model.page.total_days) break;
            target++;
        } else {
            if (!target) break;
            target--;
        }
        size_t page_offset = (target / TOUCH_HISTORY_UI_LIST_ROWS) *
                             TOUCH_HISTORY_UI_LIST_ROWS;
        if (page_offset == model.page.offset &&
            target - page_offset < model.page.returned) {
            history_controller_queue_day(
                controller, &model, model.days[target - page_offset].day,
                target, page_offset);
        } else {
            /* DAY asks the worker to load the destination page first. The day
             * string is filled from that page by a small special-case job. */
            history_job_t job = {
                .kind = HISTORY_JOB_PAGE, .page_offset = page_offset,
                .global_index = target, .signal = model.signal,
                .therapy_only = model.therapy_only,
            };
            (void)history_controller_enqueue(controller, &job);
        }
        break;
    }
    case TOUCH_HISTORY_UI_INTENT_SELECT_CHANNEL:
        if (model.has_night && intent->signal >= TOUCH_HISTORY_SIGNAL_FLOW &&
            intent->signal < TOUCH_HISTORY_SIGNAL_COUNT &&
            (model.night.available_signals &
             TOUCH_HISTORY_SIGNAL_BIT(intent->signal)) != 0)
            history_controller_queue_view(
                controller, &model, intent->signal, model.window_start_ms,
                model.window_end_ms,
                model.therapy_only && intent->signal == TOUCH_HISTORY_SIGNAL_SPO2,
                model.window_start_ms != model.night.axis_start_ms ||
                model.window_end_ms != model.night.axis_end_ms);
        break;
    case TOUCH_HISTORY_UI_INTENT_FIT_NIGHT:
        if (model.has_night)
            history_controller_queue_view(
                controller, &model, model.signal, model.night.axis_start_ms,
                model.night.axis_end_ms, model.therapy_only, false);
        break;
    case TOUCH_HISTORY_UI_INTENT_ZOOM_RELATIVE:
        if (model.has_night) {
            int64_t current = model.window_end_ms - model.window_start_ms;
            int64_t night_span = model.night.axis_end_ms -
                                 model.night.axis_start_ms;
            int64_t span = history_controller_zoom_span(
                current, night_span, intent->relative > 0 ? 1 : -1);
            int64_t centre = model.cursor_valid ? model.cursor_ms
                : model.window_start_ms + current / 2;
            int64_t start = 0, end = 0;
            history_controller_clamp_window(&model, centre, span, &start, &end);
            if (start != model.window_start_ms || end != model.window_end_ms)
                history_controller_queue_view(
                    controller, &model, model.signal, start, end,
                    model.therapy_only, true);
        }
        break;
    case TOUCH_HISTORY_UI_INTENT_PAN_RELATIVE:
        if (model.has_night && intent->relative) {
            int64_t span = model.window_end_ms - model.window_start_ms;
            int64_t centre = model.window_start_ms + span / 2;
            if ((intent->relative > 0 && centre <= INT64_MAX - intent->relative) ||
                (intent->relative < 0 && centre >= INT64_MIN - intent->relative))
                centre += intent->relative;
            int64_t start = 0, end = 0;
            history_controller_clamp_window(&model, centre, span, &start, &end);
            history_controller_queue_view(
                controller, &model, model.signal, start, end,
                model.therapy_only, true);
        }
        break;
    case TOUCH_HISTORY_UI_INTENT_SET_CURSOR: {
        bool notify = false;
        if (model.has_night && intent->timestamp_ms >= model.window_start_ms &&
            intent->timestamp_ms < model.window_end_ms &&
            xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
            controller->model.cursor_ms = intent->timestamp_ms;
            controller->model.cursor_valid = true;
            controller->revision++;
            notify = true;
            xSemaphoreGive(controller->mutex);
        }
        if (notify) history_controller_notify(controller);
        break;
    }
    case TOUCH_HISTORY_UI_INTENT_TOGGLE_THERAPY_ONLY:
        if (model.has_night && model.signal == TOUCH_HISTORY_SIGNAL_SPO2)
            history_controller_queue_view(
                controller, &model, model.signal, model.window_start_ms,
                model.window_end_ms, intent->relative != 0,
                model.window_start_ms != model.night.axis_start_ms ||
                model.window_end_ms != model.night.axis_end_ms);
        break;
    case TOUCH_HISTORY_UI_INTENT_RETRY_READ: {
        history_job_t job;
        if (xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
            job = controller->retry_job;
            xSemaphoreGive(controller->mutex);
            (void)history_controller_enqueue(controller, &job);
        }
        break;
    }
    case TOUCH_HISTORY_UI_INTENT_CANCEL_AUTO_LOAD:
    case TOUCH_HISTORY_UI_INTENT_OPEN_CARD:
        break;
    }
}

static const char *history_controller_stat_label(touch_history_stat_kind_t kind)
{
    switch (kind) {
    case TOUCH_HISTORY_STAT_P50: return "P50";
    case TOUCH_HISTORY_STAT_P95: return "P95";
    case TOUCH_HISTORY_STAT_P995: return "P99.5";
    case TOUCH_HISTORY_STAT_ABSOLUTE_P50: return "|P50|";
    case TOUCH_HISTORY_STAT_ABSOLUTE_P95: return "|P95|";
    case TOUCH_HISTORY_STAT_ABSOLUTE_P995: return "|P99.5|";
    case TOUCH_HISTORY_STAT_MINIMUM: return "Minimum";
    case TOUCH_HISTORY_STAT_P5: return "P5";
    case TOUCH_HISTORY_STAT_P05: return "P0.5";
    case TOUCH_HISTORY_STAT_TIME_BELOW_88: return "Time <88%";
    case TOUCH_HISTORY_STAT_MEDIAN: return "Median";
    case TOUCH_HISTORY_STAT_MAXIMUM: return "Maximum";
    }
    return "—";
}

static const char *history_controller_signal_unit(touch_history_signal_t signal)
{
    switch (signal) {
    case TOUCH_HISTORY_SIGNAL_FLOW: return "L/s";
    case TOUCH_HISTORY_SIGNAL_LEAK: return "L/min";
    case TOUCH_HISTORY_SIGNAL_PRESSURE: return "cmH2O";
    case TOUCH_HISTORY_SIGNAL_SPO2: return "%";
    case TOUCH_HISTORY_SIGNAL_PULSE: return "bpm";
    case TOUCH_HISTORY_SIGNAL_FLOW_LIMIT:
    case TOUCH_HISTORY_SIGNAL_SNORE:
    case TOUCH_HISTORY_SIGNAL_MOTION:
    case TOUCH_HISTORY_SIGNAL_COUNT: return "";
    }
    return "";
}

esp_err_t touch_history_controller_apply(
    touch_history_controller_t *controller, touch_history_ui_t *ui)
{
    if (!controller || !ui) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) != pdTRUE)
        return ESP_FAIL;
    history_model_t *model = &controller->model;
    touch_history_ui_snapshot_t snapshot = {
        .state = model->state,
        .days = model->days,
        .day_count = model->page.returned,
        .page = model->page,
        .selected_row = model->selected_row,
        .night = model->has_night ? &model->night : NULL,
        .sessions = model->sessions,
        .session_count = model->session_count,
        .overview = model->has_overview ? &model->overview : NULL,
        .events = model->events,
        .event_count = model->event_count,
        .month = model->has_month ? &model->month : NULL,
        .can_previous_month = model->has_month && model->month.year > 2000,
        .can_next_month = model->has_month && model->month.year < 2200,
        .selected_signal = model->signal,
        .can_previous_night = model->selected_global_index != SIZE_MAX &&
            model->selected_global_index + 1U < model->page.total_days,
        .can_next_night = model->selected_global_index != SIZE_MAX &&
            model->selected_global_index > 0,
        .usage_target_known = model->usage_target_known,
        .usage_on_target = model->usage_on_target,
        .therapy_only = model->therapy_only,
        .cursor_valid = model->cursor_valid,
        .cursor_ms = model->cursor_ms,
        .progress_per_mille = model->progress_per_mille,
        .status_text = model->status,
        .error_text = model->error,
        .degraded_text = model->degraded,
    };
    const char *unit = history_controller_signal_unit(model->signal);
    for (size_t i = 0; i < TOUCH_HISTORY_UI_STAT_COUNT; ++i) {
        if (i < model->stats.value_count) {
            const touch_history_stat_value_t *value = &model->stats.values[i];
            snapshot.stats[i].label = history_controller_stat_label(value->kind);
            snapshot.stats[i].unit = value->kind == TOUCH_HISTORY_STAT_TIME_BELOW_88
                ? "min" : unit;
            snapshot.stats[i].value_x100 = value->value_x100;
            snapshot.stats[i].available = value->available;
        } else {
            snapshot.stats[i].label = "";
            snapshot.stats[i].unit = "";
        }
    }
    esp_err_t result = touch_history_ui_apply(ui, &snapshot);
    if (result == ESP_OK) controller->rendered_revision = controller->revision;
    xSemaphoreGive(controller->mutex);
    return result;
}

uint32_t touch_history_controller_revision(
    touch_history_controller_t *controller)
{
    if (!controller || xSemaphoreTake(controller->mutex, portMAX_DELAY) != pdTRUE)
        return 0;
    uint32_t revision = controller->revision;
    xSemaphoreGive(controller->mutex);
    return revision;
}
