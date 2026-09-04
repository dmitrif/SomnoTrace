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

#define SNT_MAGIC 0x534E5442u
#define FLOW_TRACE_MAX_RECORDS (24U * 60U * 60U)
#define FLOW_READ_RECORDS 128U

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t tier;
    uint8_t n_channels;
    uint8_t sample_bytes;
    uint16_t sample_hz_x10;
    uint16_t reserved;
    int64_t start_epoch_ms;
    uint32_t sample_count;
    uint32_t reserved2;
} touch_snt_header_t;

typedef struct {
    char path[384];
    touch_snt_header_t header;
    uint32_t records;
} flow_candidate_t;

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

static bool terminal_session(const char *state)
{
    return state && (!strcmp(state, "completed") ||
                     !strcmp(state, "interrupted") ||
                     !strcmp(state, "timed_out") ||
                     !strcmp(state, "rotated") ||
                     !strcmp(state, "split"));
}

static bool inspect_flow_candidate(const char *path, uint8_t channels,
                                   flow_candidate_t *candidate)
{
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    touch_snt_header_t header;
    bool valid = fread(&header, sizeof(header), 1, file) == 1 &&
                 header.magic == SNT_MAGIC && header.version >= 1 &&
                 header.version <= 2 && header.tier == 1 &&
                 header.n_channels == channels && header.sample_bytes == 2 &&
                 header.sample_hz_x10 == 10;
    if (!valid || fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long size = ftell(file);
    fclose(file);
    if (size < (long)sizeof(header)) return false;
    uint32_t actual = (uint32_t)((size - (long)sizeof(header)) /
                                 (header.n_channels * sizeof(int16_t)));
    if (actual > header.sample_count) actual = header.sample_count;
    if (actual > FLOW_TRACE_MAX_RECORDS) actual = FLOW_TRACE_MAX_RECORDS;
    if (actual < 2) return false;
    strlcpy(candidate->path, path, sizeof(candidate->path));
    candidate->header = header;
    candidate->records = actual;
    return true;
}

/* Pick the longest terminal session in the noon-day folder. A day summary can
 * contain several short mask-on periods; choosing one real session avoids
 * drawing a false continuous line across hours where nothing was recorded. */
static bool load_flow_trace_leased(touch_history_day_t *day)
{
    char day_path[320];
    snprintf(day_path, sizeof(day_path), "%s/%s", SD_STREAMS_DIR, day->day);
    DIR *dir = opendir(day_path);
    if (!dir) return false;

    flow_candidate_t best = {0};
    struct dirent *entry;
    const char *suffix = "_session.json";
    const size_t suffix_len = strlen(suffix);
    while ((entry = readdir(dir)) != NULL) {
        size_t name_len = strlen(entry->d_name);
        if (name_len <= suffix_len ||
            strcmp(entry->d_name + name_len - suffix_len, suffix)) continue;

        char manifest_path[384];
        snprintf(manifest_path, sizeof(manifest_path), "%s/%s",
                 day_path, entry->d_name);
        FILE *manifest = fopen(manifest_path, "rb");
        if (!manifest) continue;
        if (fseek(manifest, 0, SEEK_END) != 0) { fclose(manifest); continue; }
        long manifest_size = ftell(manifest);
        if (manifest_size <= 0 || manifest_size > 4096 ||
            fseek(manifest, 0, SEEK_SET) != 0) {
            fclose(manifest);
            continue;
        }
        char *json_text = malloc((size_t)manifest_size + 1);
        if (!json_text) { fclose(manifest); continue; }
        size_t got = fread(json_text, 1, (size_t)manifest_size, manifest);
        fclose(manifest);
        json_text[got] = '\0';
        cJSON *json = cJSON_Parse(json_text);
        free(json_text);
        if (!json) continue;
        cJSON *state = cJSON_GetObjectItemCaseSensitive(json, "state");
        cJSON *format = cJSON_GetObjectItemCaseSensitive(json, "fmt");
        bool usable = cJSON_IsString(state) && terminal_session(state->valuestring);
        int fmt = cJSON_IsNumber(format) ? format->valueint : 1;
        cJSON_Delete(json);
        if (!usable) continue;

        size_t prefix_len = name_len - suffix_len;
        if (prefix_len == 0 || prefix_len >= 32) continue;
        char prefix[32];
        memcpy(prefix, entry->d_name, prefix_len);
        prefix[prefix_len] = '\0';
        char flow_path[384];
        uint8_t channels;
        if (fmt >= 2) {
            snprintf(flow_path, sizeof(flow_path), "%s/%s_flow_mm.snt",
                     day_path, prefix);
            channels = 2;
        } else {
            snprintf(flow_path, sizeof(flow_path), "%s/%s_brp_mm.snt",
                     day_path, prefix);
            channels = 4;
        }
        flow_candidate_t current = {0};
        if (!inspect_flow_candidate(flow_path, channels, &current)) continue;
        uint64_t duration = (uint64_t)current.records * 10000U /
                            current.header.sample_hz_x10;
        uint64_t best_duration = best.records
                                     ? (uint64_t)best.records * 10000U /
                                           best.header.sample_hz_x10
                                     : 0;
        if (duration > best_duration ||
            (duration == best_duration &&
             current.header.start_epoch_ms > best.header.start_epoch_ms)) {
            best = current;
        }
    }
    closedir(dir);
    if (!best.records) return false;

    FILE *file = fopen(best.path, "rb");
    if (!file || fseek(file, sizeof(touch_snt_header_t), SEEK_SET) != 0) {
        if (file) fclose(file);
        return false;
    }
    int16_t minimum[TOUCH_HISTORY_TRACE_POINTS];
    int16_t maximum[TOUCH_HISTORY_TRACE_POINTS];
    bool present[TOUCH_HISTORY_TRACE_POINTS] = {0};
    for (size_t i = 0; i < TOUCH_HISTORY_TRACE_POINTS; ++i) {
        minimum[i] = INT16_MAX;
        maximum[i] = INT16_MIN;
        day->flow_trace[i] = TOUCH_HISTORY_TRACE_MISSING;
    }
    const int16_t missing = best.header.version >= 2 ? INT16_MIN : -1;
    int16_t records[FLOW_READ_RECORDS][4];
    uint32_t processed = 0;
    while (processed < best.records) {
        size_t wanted = best.records - processed;
        if (wanted > FLOW_READ_RECORDS) wanted = FLOW_READ_RECORDS;
        size_t got = fread(records, best.header.n_channels * sizeof(int16_t),
                           wanted, file);
        for (size_t r = 0; r < got; ++r) {
            uint32_t index = processed + (uint32_t)r;
            size_t bin = (size_t)(((uint64_t)index * TOUCH_HISTORY_TRACE_POINTS) /
                                  best.records);
            if (bin >= TOUCH_HISTORY_TRACE_POINTS)
                bin = TOUCH_HISTORY_TRACE_POINTS - 1;
            if (records[r][0] != missing) {
                if (records[r][0] < minimum[bin]) minimum[bin] = records[r][0];
                if (records[r][0] > maximum[bin]) maximum[bin] = records[r][0];
                present[bin] = true;
            }
            if (records[r][1] != missing) {
                if (records[r][1] < minimum[bin]) minimum[bin] = records[r][1];
                if (records[r][1] > maximum[bin]) maximum[bin] = records[r][1];
                present[bin] = true;
            }
        }
        processed += (uint32_t)got;
        if (got != wanted) break;
    }
    fclose(file);

    unsigned populated = 0;
    for (size_t i = 0; i < TOUCH_HISTORY_TRACE_POINTS; ++i) {
        if (!present[i]) continue;
        int16_t extreme = abs((int)maximum[i]) >= abs((int)minimum[i])
                              ? maximum[i] : minimum[i];
        /* Stored flow is hundredths of L/s; the bedside chart uses L/min. */
        int32_t lpm = (int32_t)lroundf((float)extreme * 0.6f);
        if (lpm > INT16_MAX) lpm = INT16_MAX;
        if (lpm <= INT16_MIN) lpm = INT16_MIN + 1;
        day->flow_trace[i] = (int16_t)lpm;
        populated++;
    }
    if (populated < 2) return false;
    day->flow_trace_count = TOUCH_HISTORY_TRACE_POINTS;
    day->flow_trace_start_ms = best.header.start_epoch_ms;
    day->flow_trace_end_ms = best.header.start_epoch_ms +
        (int64_t)processed * 10000 / best.header.sample_hz_x10;
    day->has_flow_trace = true;
    return true;
}

esp_err_t touch_history_load_flow_trace(touch_history_day_t *day)
{
    if (!day || !valid_day(day->day)) return ESP_ERR_INVALID_ARG;
    day->has_flow_trace = false;
    day->flow_trace_loaded = false;
    day->flow_trace_count = 0;
    if (!sd_storage_is_ready()) return ESP_ERR_NOT_FOUND;
    if (sd_storage_recording_active()) return ESP_ERR_INVALID_STATE;
    if (!sd_storage_lease_acquire(SD_LEASE_UPLOAD, 250)) return ESP_ERR_TIMEOUT;
    bool loaded = load_flow_trace_leased(day);
    sd_storage_lease_release(SD_LEASE_UPLOAD);
    day->flow_trace_loaded = true;
    return loaded ? ESP_OK : ESP_ERR_NOT_FOUND;
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
