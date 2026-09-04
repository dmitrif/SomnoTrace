/* SD history adapter for the Waveshare native UI. */
#include "touch_history.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "cJSON.h"
#include "edf_gen.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "oximetry_canonical.h"
#include "sd_storage.h"

#define SNT_MAGIC 0x534E5442u
#define HISTORY_TRACE_MAX_RECORDS (24U * 60U * 60U)
#define HISTORY_READ_VALUES 512U
#define HISTORY_STORAGE_WAIT_MS 15000U
#define HISTORY_RECORDING_POLL_MS 25U

static const char *TAG = "touch_history";

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
    char path[OXIMETRY_CANONICAL_MAX_PATH];
    uint8_t version;
    uint8_t n_channels;
    uint16_t header_bytes;
    uint16_t sample_hz_x10;
    uint32_t period_num_us;
    uint32_t period_den;
    int64_t start_epoch_ms;
    uint32_t records;
    uint32_t valid_records;
} trace_candidate_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t tier;
    uint8_t timing;
    uint8_t n_channels;
    uint8_t sample_bytes;
    uint8_t flags8;
    uint16_t header_bytes;
    uint32_t period_num_us;
    uint32_t period_den;
    int64_t start_epoch_ms;
    uint32_t sample_count;
    uint32_t data_bytes;
    uint32_t data_crc32;
    uint32_t reserved0;
    uint32_t reserved1;
    uint8_t reserved[16];
} touch_ox_snt3_header_t;

_Static_assert(sizeof(touch_ox_snt3_header_t) ==
                   OXIMETRY_CANONICAL_SNT_HEADER_LEN,
               "canonical oximetry header size");

typedef union {
    struct {
        int16_t minimum[TOUCH_HISTORY_TRACE_POINTS];
        int16_t maximum[TOUCH_HISTORY_TRACE_POINTS];
        uint16_t samples[TOUCH_HISTORY_TRACE_POINTS];
    } flow;
    struct {
        int16_t extreme[TOUCH_HISTORY_TRACE_POINTS];
        uint16_t samples[TOUCH_HISTORY_TRACE_POINTS];
    } trend;
} trace_aggregate_t;

typedef struct {
    trace_aggregate_t aggregate;
    int16_t records[HISTORY_READ_VALUES];
} trace_scratch_t;

typedef struct {
    char day_path[OXIMETRY_CANONICAL_MAX_PATH];
    char record_path[OXIMETRY_CANONICAL_MAX_PATH];
    char pointer_path[OXIMETRY_CANONICAL_MAX_PATH];
    char track_path[OXIMETRY_CANONICAL_MAX_PATH];
    char manifest_path[OXIMETRY_CANONICAL_MAX_PATH];
    trace_candidate_t best;
} ox_trace_find_t;

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

static esp_err_t session_count(const char *day, int *out_count)
{
    if (!out_count) return ESP_ERR_INVALID_ARG;
    *out_count = 0;
    char path[320];
    snprintf(path, sizeof(path), "%s/%s", SD_STREAMS_DIR, day);
    DIR *dir = opendir(path);
    if (!dir) {
        int err = errno;
        if (err == ENOENT || err == ENOTDIR) return ESP_ERR_NOT_FOUND;
        ESP_LOGE(TAG, "cannot open history day %s: errno=%d (%s)",
                 path, err, strerror(err));
        return ESP_FAIL;
    }
    const char *suffix = "_session.json";
    const size_t suffix_len = strlen(suffix);
    int count = 0;
    int read_err = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            read_err = errno;
            break;
        }
        size_t len = strlen(entry->d_name);
        if (len > suffix_len &&
            strcmp(entry->d_name + len - suffix_len, suffix) == 0) count++;
    }
    int close_err = closedir(dir) == 0 ? 0 : errno;
    if (read_err || close_err) {
        int err = read_err ? read_err : close_err;
        ESP_LOGE(TAG, "cannot enumerate history day %s: errno=%d (%s)",
                 path, err, strerror(err));
        return ESP_FAIL;
    }
    *out_count = count;
    return ESP_OK;
}

/* History runs off the LVGL task, so it can afford to wait for the short
 * storage-finalise/export window after TherapyStop.  One shared deadline
 * bounds both waits; a genuinely active therapy session still returns busy. */
static esp_err_t history_lease_acquire(void)
{
    if (!sd_storage_is_ready()) return ESP_ERR_NOT_FOUND;

    const int64_t deadline_us = esp_timer_get_time() +
                                (int64_t)HISTORY_STORAGE_WAIT_MS * 1000;
    while (sd_storage_recording_active() && esp_timer_get_time() < deadline_us) {
        vTaskDelay(pdMS_TO_TICKS(HISTORY_RECORDING_POLL_MS));
    }
    if (sd_storage_recording_active()) return ESP_ERR_INVALID_STATE;

    int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) return ESP_ERR_TIMEOUT;
    uint32_t remaining_ms = (uint32_t)((remaining_us + 999) / 1000);
    if (!sd_storage_lease_acquire(SD_LEASE_UPLOAD, remaining_ms))
        return ESP_ERR_TIMEOUT;

    /* A fresh session can start between the poll and lease acquisition. */
    if (sd_storage_recording_active()) {
        sd_storage_lease_release(SD_LEASE_UPLOAD);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
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

/* Session manifests are generated locally and state/fmt are scalar fields.
 * Extracting those two values directly avoids allocating a cJSON tree from
 * the already tight internal heap once for every session in a night. */
static bool manifest_terminal_and_format(const char *json, int *format)
{
    const char *state_key = json ? strstr(json, "\"state\"") : NULL;
    const char *colon = state_key ? strchr(state_key + 7, ':') : NULL;
    const char *quote = colon ? strchr(colon + 1, '"') : NULL;
    if (!quote) return false;
    quote++;
    const char *end = strchr(quote, '"');
    if (!end || end == quote || (size_t)(end - quote) >= 24) return false;
    char state[24];
    memcpy(state, quote, (size_t)(end - quote));
    state[end - quote] = '\0';
    if (!terminal_session(state)) return false;

    *format = 1;
    const char *format_key = strstr(json, "\"fmt\"");
    colon = format_key ? strchr(format_key + 5, ':') : NULL;
    if (colon) {
        char *number_end = NULL;
        long parsed = strtol(colon + 1, &number_end, 10);
        if (number_end != colon + 1 && parsed >= 1 && parsed <= INT_MAX)
            *format = (int)parsed;
    }
    return true;
}

static bool json_string_equals(const char *json, const char *key,
                               const char *expected)
{
    char token[48];
    if (snprintf(token, sizeof(token), "\"%s\"", key) >= (int)sizeof(token))
        return false;
    size_t expected_len = strlen(expected);
    const char *found = json;
    while ((found = found ? strstr(found, token) : NULL) != NULL) {
        const char *colon = strchr(found + strlen(token), ':');
        const char *quote = colon ? strchr(colon + 1, '"') : NULL;
        if (!quote) return false;
        quote++;
        if (!strncmp(quote, expected, expected_len) &&
            quote[expected_len] == '"') return true;
        found += strlen(token);
    }
    return false;
}

static esp_err_t read_json_text(const char *path, char **out)
{
    if (!path || !out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    struct stat st;
    if (stat(path, &st) != 0) {
        return errno == ENOENT || errno == ENOTDIR
                   ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > OXIMETRY_CANONICAL_MAX_JSON_BYTES) return ESP_FAIL;
    FILE *file = fopen(path, "rb");
    /* stat already observed the file. Disappearance/open failure after that
     * is a raced or transient read, not proof that the package is absent. */
    if (!file) return ESP_FAIL;
    char *text = heap_caps_malloc((size_t)st.st_size + 1,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!text) text = malloc((size_t)st.st_size + 1);
    if (!text) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(text, 1, (size_t)st.st_size, file);
    bool read_failed = got != (size_t)st.st_size || ferror(file);
    int close_result = fclose(file);
    if (read_failed || close_result != 0) {
        free(text);
        return ESP_FAIL;
    }
    text[got] = '\0';
    *out = text;
    return ESP_OK;
}

static esp_err_t read_ready_generation(const char *path, int *generation)
{
    char *text = NULL;
    esp_err_t result = read_json_text(path, &text);
    if (result != ESP_OK) return result;
    bool ready = json_string_equals(text, "schema",
                                    "somnotrace.oximetry.recording/1") &&
                 json_string_equals(text, "state", "ready");
    const char *key = ready ? strstr(text, "\"active_generation\"") : NULL;
    const char *colon = key ? strchr(key + 19, ':') : NULL;
    char *end = NULL;
    errno = 0;
    long parsed = colon ? strtol(colon + 1, &end, 10) : 0;
    free(text);
    if (!ready || !colon || end == colon + 1 || errno == ERANGE ||
        parsed <= 0 || parsed > 100000) return ESP_FAIL;
    *generation = (int)parsed;
    return ESP_OK;
}

static esp_err_t validate_generation_manifest(const char *path)
{
    char *text = NULL;
    esp_err_t result = read_json_text(path, &text);
    /* A ready pointer is a publication promise. A missing or partial active
     * generation is therefore a retryable/corrupt read, not genuine no-data. */
    if (result == ESP_ERR_NOT_FOUND) return ESP_FAIL;
    if (result != ESP_OK) return result;
    bool valid = json_string_equals(text, "schema",
                                    "somnotrace.oximetry.generation/1") &&
                 json_string_equals(text, "state", "ready") &&
                 json_string_equals(text, "path", "data/vitals.snt");
    free(text);
    return valid ? ESP_OK : ESP_FAIL;
}

static esp_err_t inspect_ox_trace_candidate(const char *path,
                                            trace_candidate_t *candidate)
{
    FILE *file = fopen(path, "rb");
    /* The ready pointer and generation manifest both name this track, so even
     * ENOENT here is an incomplete publication rather than an empty night. */
    if (!file) return ESP_FAIL;
    touch_ox_snt3_header_t header;
    bool header_read = fread(&header, sizeof(header), 1, file) == 1;
    bool valid = header_read &&
                 header.magic == OXIMETRY_CANONICAL_SNT_MAGIC &&
                 header.version == OXIMETRY_CANONICAL_SNT_VERSION &&
                 header.tier == 0 && header.timing == 0 &&
                 header.header_bytes == OXIMETRY_CANONICAL_SNT_HEADER_LEN &&
                 header.n_channels == OXIMETRY_CANONICAL_VITALS_CHANNELS &&
                 header.sample_bytes == sizeof(int16_t) &&
                 header.period_num_us > 0 && header.period_den > 0;
    if (!valid || fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    long size = ftell(file);
    if (size < header.header_bytes) {
        fclose(file);
        return ESP_FAIL;
    }
    uint32_t record_bytes = header.n_channels * sizeof(int16_t);
    uint64_t expected_bytes = (uint64_t)header.sample_count * record_bytes;
    if (header.data_bytes != expected_bytes ||
        (uint64_t)(size - header.header_bytes) != expected_bytes) {
        fclose(file);
        return ESP_FAIL;
    }
    /* Canonical noon-day recordings are bounded to one day. Reject an
     * implausible header instead of ranking from only a prefix: coverage must
     * describe the whole candidate recording. */
    if (header.sample_count > HISTORY_TRACE_MAX_RECORDS) {
        fclose(file);
        return ESP_FAIL;
    }
    uint32_t actual = header.sample_count;
    if (actual < 2) {
        fclose(file);
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(file, header.header_bytes, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    int16_t *probe = heap_caps_malloc(HISTORY_READ_VALUES * sizeof(*probe),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!probe) probe = malloc(HISTORY_READ_VALUES * sizeof(*probe));
    if (!probe) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    const size_t probe_records = HISTORY_READ_VALUES / header.n_channels;
    uint32_t checked = 0;
    uint32_t valid_spo2 = 0;
    bool probe_ok = true;
    while (checked < actual) {
        size_t wanted = actual - checked;
        if (wanted > probe_records) wanted = probe_records;
        size_t got = fread(probe, header.n_channels * sizeof(int16_t),
                           wanted, file);
        for (size_t i = 0; i < got; ++i) {
            const int16_t *record = &probe[i * header.n_channels];
            int16_t value = record[OXIMETRY_CANONICAL_VITALS_SPO2];
            bool status_missing =
                ((uint16_t)record[OXIMETRY_CANONICAL_VITALS_STATUS] & 1U) != 0;
            if (!status_missing && value > 0 && value <= 10000)
                valid_spo2++;
        }
        checked += (uint32_t)got;
        if (got != wanted || ferror(file)) {
            probe_ok = false;
            break;
        }
    }
    free(probe);
    if (fclose(file) != 0) probe_ok = false;
    if (!probe_ok) return ESP_FAIL;
    if (valid_spo2 < 2) return ESP_ERR_NOT_FOUND;
    strlcpy(candidate->path, path, sizeof(candidate->path));
    candidate->version = header.version;
    candidate->n_channels = header.n_channels;
    candidate->header_bytes = header.header_bytes;
    candidate->period_num_us = header.period_num_us;
    candidate->period_den = header.period_den;
    candidate->start_epoch_ms = header.start_epoch_ms;
    candidate->records = actual;
    candidate->valid_records = valid_spo2;
    return ESP_OK;
}

static uint64_t candidate_duration_us(const trace_candidate_t *candidate)
{
    if (candidate->period_num_us && candidate->period_den)
        return (uint64_t)candidate->records * candidate->period_num_us /
               candidate->period_den;
    return candidate->sample_hz_x10
               ? (uint64_t)candidate->records * 10000000U /
                     candidate->sample_hz_x10
               : 0;
}

static uint64_t candidate_valid_coverage_us(const trace_candidate_t *candidate)
{
    if (!candidate->period_num_us || !candidate->period_den) return 0;
    return (uint64_t)candidate->valid_records * candidate->period_num_us /
           candidate->period_den;
}

static void remember_discovery_error(esp_err_t *saved, esp_err_t error)
{
    if (!saved || error == ESP_OK || error == ESP_ERR_NOT_FOUND) return;
    if (*saved == ESP_OK || error == ESP_ERR_NO_MEM) *saved = error;
}

static esp_err_t find_spo2_candidate(const char *day, trace_candidate_t *best)
{
    ox_trace_find_t *ctx = heap_caps_calloc(
        1, sizeof(*ctx), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx) ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return ESP_ERR_NO_MEM;
    snprintf(ctx->day_path, sizeof(ctx->day_path),
             SD_OXYMETRY_DIR "/recordings/%s", day);
    DIR *dir = opendir(ctx->day_path);
    if (!dir) {
        int err = errno;
        free(ctx);
        return err == ENOENT || err == ENOTDIR ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    int read_err = 0;
    esp_err_t discovery_error = ESP_OK;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            read_err = errno;
            break;
        }
        if (entry->d_name[0] == '.') continue;
        if (snprintf(ctx->record_path, sizeof(ctx->record_path), "%s/%s",
                     ctx->day_path, entry->d_name) >=
            (int)sizeof(ctx->record_path)) {
            remember_discovery_error(&discovery_error, ESP_FAIL);
            continue;
        }
        struct stat st;
        if (stat(ctx->record_path, &st) != 0) {
            /* readdir already observed this entry; a failed follow-up stat
             * makes the scan incomplete even when the failure is ENOENT. */
            remember_discovery_error(&discovery_error, ESP_FAIL);
            continue;
        }
        if (!S_ISDIR(st.st_mode)) continue;
        if (snprintf(ctx->pointer_path, sizeof(ctx->pointer_path),
                     "%s/recording.json", ctx->record_path) >=
            (int)sizeof(ctx->pointer_path)) {
            remember_discovery_error(&discovery_error, ESP_FAIL);
            continue;
        }
        int generation = 0;
        esp_err_t pointer_result =
            read_ready_generation(ctx->pointer_path, &generation);
        /* Published recording directories must contain their ready pointer.
         * A night with no recording directories is genuine absence; an
         * observed directory with no pointer is an incomplete package. */
        if (pointer_result == ESP_ERR_NOT_FOUND) pointer_result = ESP_FAIL;
        if (pointer_result != ESP_OK) {
            remember_discovery_error(&discovery_error, pointer_result);
            continue;
        }
        if (snprintf(ctx->track_path, sizeof(ctx->track_path),
                     "%s/generations/%d/data/vitals.snt",
                     ctx->record_path, generation) >=
            (int)sizeof(ctx->track_path)) {
            remember_discovery_error(&discovery_error, ESP_FAIL);
            continue;
        }
        if (snprintf(ctx->manifest_path, sizeof(ctx->manifest_path),
                     "%s/generations/%d/manifest.json",
                     ctx->record_path, generation) >=
            (int)sizeof(ctx->manifest_path)) {
            remember_discovery_error(&discovery_error, ESP_FAIL);
            continue;
        }
        esp_err_t manifest_result =
            validate_generation_manifest(ctx->manifest_path);
        if (manifest_result != ESP_OK) {
            remember_discovery_error(&discovery_error, manifest_result);
            continue;
        }
        trace_candidate_t current = {0};
        esp_err_t inspect_result =
            inspect_ox_trace_candidate(ctx->track_path, &current);
        if (inspect_result == ESP_ERR_NOT_FOUND) continue;
        if (inspect_result != ESP_OK) {
            remember_discovery_error(&discovery_error, inspect_result);
            continue;
        }
        uint64_t coverage = candidate_valid_coverage_us(&current);
        uint64_t best_coverage = candidate_valid_coverage_us(&ctx->best);
        uint64_t duration = candidate_duration_us(&current);
        uint64_t best_duration = candidate_duration_us(&ctx->best);
        /* Valid time coverage is the primary selection metric. A mostly
         * no-finger all-night file must not hide a shorter healthy recording.
         * Total duration and then latest start provide deterministic ties. */
        if (coverage > best_coverage ||
            (coverage == best_coverage && duration > best_duration) ||
            (coverage == best_coverage && duration == best_duration &&
             current.start_epoch_ms > ctx->best.start_epoch_ms)) {
            ctx->best = current;
        }
    }
    int close_err = closedir(dir) == 0 ? 0 : errno;
    bool found = ctx->best.valid_records >= 2;
    if (found) *best = ctx->best;
    free(ctx);
    if (read_err || close_err) return ESP_FAIL;
    /* Do not cache an incomplete directory scan as genuine no-data. The
     * canonical tree is published ready-only, so any non-absence discovery
     * failure means candidate selection may be incomplete and must retry. */
    if (discovery_error != ESP_OK) return discovery_error;
    if (found) return ESP_OK;
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t inspect_trace_candidate(const char *path, uint8_t tier,
                                         uint8_t channels, uint16_t hz_x10,
                                         trace_candidate_t *candidate)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return errno == ENOENT || errno == ENOTDIR
                   ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    touch_snt_header_t header;
    bool valid = fread(&header, sizeof(header), 1, file) == 1 &&
                 header.magic == SNT_MAGIC && header.version >= 1 &&
                 header.version <= 2 && header.tier == tier &&
                 header.n_channels == channels && header.sample_bytes == 2 &&
                 header.sample_hz_x10 == hz_x10;
    if (!valid || fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    long size = ftell(file);
    int close_result = fclose(file);
    if (size < (long)sizeof(header) || close_result != 0) return ESP_FAIL;
    uint32_t actual = (uint32_t)((size - (long)sizeof(header)) /
                                 (header.n_channels * sizeof(int16_t)));
    if (actual > header.sample_count) actual = header.sample_count;
    if (actual > HISTORY_TRACE_MAX_RECORDS) actual = HISTORY_TRACE_MAX_RECORDS;
    if (actual < 2) return ESP_ERR_NOT_FOUND;
    strlcpy(candidate->path, path, sizeof(candidate->path));
    candidate->version = header.version;
    candidate->n_channels = header.n_channels;
    candidate->header_bytes = sizeof(header);
    candidate->sample_hz_x10 = header.sample_hz_x10;
    candidate->start_epoch_ms = header.start_epoch_ms;
    candidate->records = actual;
    return ESP_OK;
}

/* Pick the longest terminal session in the noon-day folder. A day summary can
 * contain several short mask-on periods; choosing one real session avoids
 * drawing a false continuous line across hours where nothing was recorded. */
static esp_err_t load_trace_leased(const char *day,
                                   touch_history_channel_t channel,
                                   touch_history_trace_t *trace)
{
    trace_candidate_t best = {0};
    if (channel == TOUCH_HISTORY_CHANNEL_SPO2) {
        esp_err_t find_result = find_spo2_candidate(day, &best);
        if (find_result != ESP_OK) return find_result;
        goto candidate_ready;
    }

    char day_path[320];
    snprintf(day_path, sizeof(day_path), "%s/%s", SD_STREAMS_DIR, day);
    DIR *dir = opendir(day_path);
    if (!dir) return errno == ENOENT || errno == ENOTDIR
                         ? ESP_ERR_NOT_FOUND : ESP_FAIL;

    const char *suffix = "_session.json";
    const size_t suffix_len = strlen(suffix);
    int read_err = 0;
    esp_err_t candidate_error = ESP_OK;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            read_err = errno;
            break;
        }
        size_t name_len = strlen(entry->d_name);
        if (name_len <= suffix_len ||
            strcmp(entry->d_name + name_len - suffix_len, suffix)) continue;

        char manifest_path[384];
        snprintf(manifest_path, sizeof(manifest_path), "%s/%s",
                 day_path, entry->d_name);
        FILE *manifest = fopen(manifest_path, "rb");
        if (!manifest) {
            /* The directory entry came from this same scan; failure to reopen
             * it is transient/incomplete and must remain retryable. */
            remember_discovery_error(&candidate_error, ESP_FAIL);
            continue;
        }
        if (fseek(manifest, 0, SEEK_END) != 0) {
            remember_discovery_error(&candidate_error, ESP_FAIL);
            fclose(manifest);
            continue;
        }
        long manifest_size = ftell(manifest);
        if (manifest_size <= 0 || manifest_size > 4096 ||
            fseek(manifest, 0, SEEK_SET) != 0) {
            remember_discovery_error(&candidate_error, ESP_FAIL);
            fclose(manifest);
            continue;
        }
        char *json_text = heap_caps_malloc((size_t)manifest_size + 1,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!json_text) json_text = malloc((size_t)manifest_size + 1);
        if (!json_text) {
            fclose(manifest);
            remember_discovery_error(&candidate_error, ESP_ERR_NO_MEM);
            continue;
        }
        size_t got = fread(json_text, 1, (size_t)manifest_size, manifest);
        bool manifest_read_failed =
            got != (size_t)manifest_size || ferror(manifest);
        int manifest_close_result = fclose(manifest);
        if (manifest_read_failed || manifest_close_result != 0) {
            remember_discovery_error(&candidate_error, ESP_FAIL);
            free(json_text);
            continue;
        }
        json_text[got] = '\0';
        int fmt = 1;
        bool usable = manifest_terminal_and_format(json_text, &fmt);
        free(json_text);
        if (!usable) continue;

        size_t prefix_len = name_len - suffix_len;
        if (prefix_len == 0 || prefix_len >= 32) continue;
        char prefix[32];
        memcpy(prefix, entry->d_name, prefix_len);
        prefix[prefix_len] = '\0';
        char trace_path[384];
        uint8_t tier = 0;
        uint8_t channels = 0;
        uint16_t hz_x10 = 0;
        switch (channel) {
        case TOUCH_HISTORY_CHANNEL_FLOW:
            tier = 1;
            hz_x10 = 10;
            if (fmt >= 2) {
                snprintf(trace_path, sizeof(trace_path), "%s/%s_flow_mm.snt",
                         day_path, prefix);
                channels = 2;
            } else {
                snprintf(trace_path, sizeof(trace_path), "%s/%s_brp_mm.snt",
                         day_path, prefix);
                channels = 4;
            }
            break;
        case TOUCH_HISTORY_CHANNEL_LEAK:
            snprintf(trace_path, sizeof(trace_path), "%s/%s_pld.snt",
                     day_path, prefix);
            channels = 12;
            hz_x10 = 5;
            break;
        default:
            continue;
        }
        trace_candidate_t current = {0};
        esp_err_t inspect_result = inspect_trace_candidate(
            trace_path, tier, channels, hz_x10, &current);
        if (inspect_result == ESP_ERR_NOT_FOUND) continue;
        if (inspect_result != ESP_OK) {
            remember_discovery_error(&candidate_error, inspect_result);
            continue;
        }
        uint64_t duration = (uint64_t)current.records * 10000U /
                            current.sample_hz_x10;
        uint64_t best_duration = best.records
                                     ? (uint64_t)best.records * 10000U /
                                           best.sample_hz_x10
                                     : 0;
        if (duration > best_duration ||
            (duration == best_duration &&
             current.start_epoch_ms > best.start_epoch_ms)) {
            best = current;
        }
    }
    int close_err = closedir(dir) == 0 ? 0 : errno;
    if (read_err || close_err) return ESP_FAIL;
    if (candidate_error != ESP_OK) return candidate_error;
    if (!best.records) return ESP_ERR_NOT_FOUND;

candidate_ready:
    ;
    FILE *file = fopen(best.path, "rb");
    if (!file || fseek(file, best.header_bytes, SEEK_SET) != 0) {
        if (file) fclose(file);
        return ESP_FAIL;
    }
    trace_scratch_t *scratch = heap_caps_calloc(
        1, sizeof(*scratch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!scratch) scratch = calloc(1, sizeof(*scratch));
    if (!scratch) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    /* Flow is an overnight envelope. Preserve both ends of every time bin as
     * parallel arrays; serialising low/high into one connected line would
     * fabricate a sawtooth that never occurred in the recording. */
    const size_t bins = TOUCH_HISTORY_TRACE_POINTS;
    trace_aggregate_t *aggregate = &scratch->aggregate;
    for (size_t i = 0; i < bins; ++i) {
        if (channel == TOUCH_HISTORY_CHANNEL_FLOW) {
            aggregate->flow.minimum[i] = INT16_MAX;
            aggregate->flow.maximum[i] = INT16_MIN;
        } else {
            aggregate->trend.extreme[i] =
                channel == TOUCH_HISTORY_CHANNEL_SPO2 ? INT16_MAX : INT16_MIN;
        }
        trace->points[i] = TOUCH_HISTORY_TRACE_MISSING;
        trace->upper_points[i] = TOUCH_HISTORY_TRACE_MISSING;
    }
    const int16_t missing = best.version >= 2 ? INT16_MIN : -1;
    /* Flat storage is deliberate. A [N][4] array combined with a 2-channel
     * fread advances rows by four int16s and silently skips every other v2
     * flow record (then reads uninitialised stack at the tail). */
    int16_t *records = scratch->records;
    const size_t records_per_read =
        HISTORY_READ_VALUES / best.n_channels;
    uint32_t processed = 0;
    bool read_ok = true;
    while (processed < best.records) {
        size_t wanted = best.records - processed;
        if (wanted > records_per_read) wanted = records_per_read;
        size_t got = fread(records, best.n_channels * sizeof(int16_t),
                           wanted, file);
        for (size_t r = 0; r < got; ++r) {
            uint32_t index = processed + (uint32_t)r;
            size_t bin = (size_t)(((uint64_t)index * bins) / best.records);
            if (bin >= bins) bin = bins - 1;
            const int16_t *record = &records[r * best.n_channels];
            if (channel == TOUCH_HISTORY_CHANNEL_FLOW) {
                for (size_t value_index = 0; value_index < 2; ++value_index) {
                    int16_t value = record[value_index];
                    if (value == missing) continue;
                    if (value < aggregate->flow.minimum[bin])
                        aggregate->flow.minimum[bin] = value;
                    if (value > aggregate->flow.maximum[bin])
                        aggregate->flow.maximum[bin] = value;
                    if (aggregate->flow.samples[bin] < UINT16_MAX)
                        aggregate->flow.samples[bin]++;
                }
            } else {
                size_t value_index = channel == TOUCH_HISTORY_CHANNEL_SPO2
                                         ? OXIMETRY_CANONICAL_VITALS_SPO2 : 3;
                int16_t value = record[value_index];
                /* Older capture paths wrote -1 when an optional PLD/SA2
                 * channel was absent even in v2 files. Neither SpO2 nor Leak
                 * has a valid negative physical value, so treat it as a gap. */
                bool invalid_spo2 = channel == TOUCH_HISTORY_CHANNEL_SPO2 &&
                    (value <= 0 || value > 10000 ||
                     (((uint16_t)record[OXIMETRY_CANONICAL_VITALS_STATUS] & 1U) != 0));
                if (value == missing || value < 0 || invalid_spo2) continue;
                if (channel == TOUCH_HISTORY_CHANNEL_SPO2) {
                    if (value < aggregate->trend.extreme[bin])
                        aggregate->trend.extreme[bin] = value;
                } else if (value > aggregate->trend.extreme[bin]) {
                    aggregate->trend.extreme[bin] = value;
                }
                if (aggregate->trend.samples[bin] < UINT16_MAX)
                    aggregate->trend.samples[bin]++;
            }
        }
        processed += (uint32_t)got;
        if (got != wanted) {
            read_ok = false;
            break;
        }
    }
    if (fclose(file) != 0) read_ok = false;

    if (!read_ok) {
        free(scratch);
        return ESP_FAIL;
    }

    unsigned populated = 0;
    for (size_t i = 0; i < bins; ++i) {
        uint16_t samples = channel == TOUCH_HISTORY_CHANNEL_FLOW
                               ? aggregate->flow.samples[i]
                               : aggregate->trend.samples[i];
        if (!samples) continue;
        if (channel == TOUCH_HISTORY_CHANNEL_FLOW) {
            int32_t low_lpm = (int32_t)lroundf(
                (float)aggregate->flow.minimum[i] * 0.6f);
            int32_t high_lpm = (int32_t)lroundf(
                (float)aggregate->flow.maximum[i] * 0.6f);
            if (low_lpm <= INT16_MIN) low_lpm = INT16_MIN + 1;
            if (high_lpm > INT16_MAX) high_lpm = INT16_MAX;
            trace->points[i] = (int16_t)low_lpm;
            trace->upper_points[i] = (int16_t)high_lpm;
            populated++;
        } else {
            float extreme = (float)aggregate->trend.extreme[i];
            /* Canonical SpO2 is percent x100; PLD Leak is L/s x100. Keep
             * desaturation nadirs and leak peaks instead of averaging away
             * the clinically useful excursions in each overnight bin. */
            int32_t physical = channel == TOUCH_HISTORY_CHANNEL_SPO2
                                   ? (int32_t)lroundf(extreme / 100.0f)
                                   : (int32_t)lroundf(extreme * 0.6f);
            if (physical > INT16_MAX) physical = INT16_MAX;
            trace->points[i] = (int16_t)physical;
            populated++;
        }
    }
    free(scratch);
    if (populated < 2) return ESP_ERR_NOT_FOUND;
    trace->count = TOUCH_HISTORY_TRACE_POINTS;
    trace->start_ms = best.start_epoch_ms;
    if (best.period_num_us && best.period_den) {
        trace->end_ms = best.start_epoch_ms +
            (int64_t)((uint64_t)processed * best.period_num_us /
                      best.period_den / 1000U);
    } else {
        trace->end_ms = best.start_epoch_ms +
            (int64_t)processed * 10000 / best.sample_hz_x10;
    }
    trace->has_data = true;
    return ESP_OK;
}

esp_err_t touch_history_load_trace(const char *day,
                                   touch_history_channel_t channel,
                                   touch_history_trace_t *trace)
{
    if (!trace || !valid_day(day) || channel < TOUCH_HISTORY_CHANNEL_FLOW ||
        channel >= TOUCH_HISTORY_CHANNEL_COUNT) return ESP_ERR_INVALID_ARG;
    memset(trace, 0, sizeof(*trace));
    trace->channel = channel;
    for (size_t i = 0; i < TOUCH_HISTORY_TRACE_POINTS; ++i) {
        trace->points[i] = TOUCH_HISTORY_TRACE_MISSING;
        trace->upper_points[i] = TOUCH_HISTORY_TRACE_MISSING;
    }
    esp_err_t lease_result = history_lease_acquire();
    if (lease_result != ESP_OK) return lease_result;
    esp_err_t result = load_trace_leased(day, channel, trace);
    sd_storage_lease_release(SD_LEASE_UPLOAD);
    trace->loaded = result == ESP_OK || result == ESP_ERR_NOT_FOUND;
    return result;
}

esp_err_t touch_history_load(touch_history_day_t *days, size_t capacity,
                             size_t *count)
{
    if (!days || !count || capacity == 0) return ESP_ERR_INVALID_ARG;
    *count = 0;
    const size_t limit = capacity < TOUCH_HISTORY_MAX_DAYS
                         ? capacity : TOUCH_HISTORY_MAX_DAYS;
    esp_err_t lease_result = history_lease_acquire();
    if (lease_result != ESP_OK) return lease_result;

    DIR *dir = opendir(SD_STREAMS_DIR);
    if (!dir) {
        int err = errno;
        ESP_LOGE(TAG, "cannot open history root %s: errno=%d (%s)",
                 SD_STREAMS_DIR, err, strerror(err));
        sd_storage_lease_release(SD_LEASE_UPLOAD);
        return err == ENOENT || err == ENOTDIR ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    esp_err_t result = ESP_OK;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            if (errno != 0) {
                ESP_LOGE(TAG, "cannot enumerate history root %s: errno=%d (%s)",
                         SD_STREAMS_DIR, errno, strerror(errno));
                result = ESP_FAIL;
            }
            break;
        }
        if (!valid_day(entry->d_name)) continue;
        int sessions = 0;
        esp_err_t count_result = session_count(entry->d_name, &sessions);
        if (count_result == ESP_ERR_NOT_FOUND) continue;
        if (count_result != ESP_OK) {
            result = count_result;
            break;
        }
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
    if (closedir(dir) != 0 && result == ESP_OK) {
        ESP_LOGE(TAG, "cannot close history root %s: errno=%d (%s)",
                 SD_STREAMS_DIR, errno, strerror(errno));
        result = ESP_FAIL;
    }
    if (result != ESP_OK) {
        *count = 0;
        sd_storage_lease_release(SD_LEASE_UPLOAD);
        return result;
    }
    qsort(days, *count, sizeof(*days), newest_first);

    for (size_t i = 0; i < *count; ++i) {
        char *json = NULL;
        if (edf_gen_summary_json(days[i].day, &json) != ESP_OK || !json) continue;
        cJSON *root = cJSON_Parse(json);
        free(json);
        if (!root) continue;
        float number = 0.0f;
        if (json_number(root, "sessions", &number)) days[i].sessions = (int)number;
        days[i].has_mask_off_count =
            json_number(root, "mask_off_count", &number);
        if (days[i].has_mask_off_count)
            days[i].mask_off_count = (int)number;
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
