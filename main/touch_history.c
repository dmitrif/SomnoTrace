/* SD history adapter for the Waveshare native UI. */
#include "touch_history.h"

#include <dirent.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "edf_gen.h"
#include "sd_storage.h"

static bool valid_day(const char *name)
{
    if (!name || strlen(name) != 8) return false;
    for (int i = 0; i < 8; ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    return true;
}

static int newest_first(const void *a, const void *b)
{
    const touch_history_day_t *da = a;
    const touch_history_day_t *db = b;
    return strcmp(db->day, da->day);
}

static int session_count(const char *day)
{
    char path[320];
    snprintf(path, sizeof(path), "%s/%s", SD_STREAMS_DIR, day);
    DIR *dir = opendir(path);
    if (!dir) return 0;
    const char *suffix = "_session.json";
    const size_t suffix_len = strlen(suffix);
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len > suffix_len &&
            strcmp(entry->d_name + len - suffix_len, suffix) == 0) count++;
    }
    closedir(dir);
    return count;
}

static bool json_number(cJSON *object, const char *key, float *out)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble)) return false;
    *out = (float)value->valuedouble;
    return true;
}

esp_err_t touch_history_load(touch_history_day_t *days, size_t capacity,
                             size_t *count)
{
    if (!days || !count || capacity == 0) return ESP_ERR_INVALID_ARG;
    *count = 0;
    const size_t limit = capacity < TOUCH_HISTORY_MAX_DAYS
                         ? capacity : TOUCH_HISTORY_MAX_DAYS;
    if (!sd_storage_is_ready()) return ESP_ERR_NOT_FOUND;
    if (sd_storage_recording_active()) return ESP_ERR_INVALID_STATE;
    if (!sd_storage_lease_acquire(SD_LEASE_UPLOAD, 250)) return ESP_ERR_TIMEOUT;

    DIR *dir = opendir(SD_STREAMS_DIR);
    if (!dir) {
        sd_storage_lease_release(SD_LEASE_UPLOAD);
        return ESP_ERR_NOT_FOUND;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!valid_day(entry->d_name)) continue;
        int sessions = session_count(entry->d_name);
        if (sessions == 0) continue;
        size_t slot;
        if (*count < limit) {
            slot = (*count)++;
        } else {
            /* Directory iteration is not chronological. Keep the newest N
             * entries even when the card contains years of history. */
            slot = 0;
            for (size_t i = 1; i < limit; ++i) {
                if (strcmp(days[i].day, days[slot].day) < 0) slot = i;
            }
            if (strcmp(entry->d_name, days[slot].day) <= 0) continue;
        }
        touch_history_day_t *day = &days[slot];
        memset(day, 0, sizeof(*day));
        strlcpy(day->day, entry->d_name, sizeof(day->day));
        day->sessions = sessions;
    }
    closedir(dir);
    qsort(days, *count, sizeof(*days), newest_first);

    for (size_t i = 0; i < *count; ++i) {
        char *json = NULL;
        if (edf_gen_summary_json(days[i].day, &json) != ESP_OK || !json) continue;
        cJSON *root = cJSON_Parse(json);
        free(json);
        if (!root) continue;
        float number = 0.0f;
        if (json_number(root, "sessions", &number)) days[i].sessions = (int)number;
        days[i].has_usage = json_number(root, "usage_min", &number);
        if (days[i].has_usage) days[i].usage_min = (int)number;
        days[i].has_ahi = json_number(root, "ahi", &days[i].ahi);
        days[i].has_oai = json_number(root, "oai", &days[i].oai);
        days[i].has_cai = json_number(root, "cai", &days[i].cai);
        days[i].has_hi = json_number(root, "hi", &days[i].hi);
        days[i].has_rera = json_number(root, "rera", &days[i].rera);
        cJSON *pressure = cJSON_GetObjectItemCaseSensitive(root, "pressure");
        cJSON *leak = cJSON_GetObjectItemCaseSensitive(root, "leak");
        days[i].has_pressure_p95 = cJSON_IsObject(pressure) &&
                                   json_number(pressure, "p95", &days[i].pressure_p95);
        days[i].has_leak_p95 = cJSON_IsObject(leak) &&
                               json_number(leak, "p95", &days[i].leak_p95);
        days[i].has_summary = true;
        cJSON_Delete(root);
    }
    sd_storage_lease_release(SD_LEASE_UPLOAD);
    return ESP_OK;
}
