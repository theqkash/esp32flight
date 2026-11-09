#include "faflight.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "http_util.h"
#include "settings.h"
#include "routes.h"

static const char *TAG = "faflight";

/* Only the selected and the nearest flight are ever looked up, so this
 * holds far more than a sky's worth; FIFO eviction is fine. PSRAM. */
#define FA_CACHE_SIZE 256
#define FA_BUF_SIZE   (48 * 1024)

/* Budget pacing: the allowance grows linearly through the month, plus a
 * small burst so the first flights after boot or a month change still get
 * numbers. Unused allowance carries forward within the month. */
#define FA_BURST        10
#define FA_MIN_GAP_US   (60LL * 1000000)      /* never more than one a minute */
#define FA_AUTH_HOLD_US (60LL * 60 * 1000000) /* 401/403: key wrong or blocked */
#define FA_RATE_HOLD_US (10LL * 60 * 1000000) /* 429 */

typedef struct {
    char callsign[9];
    char iata[10];      /* empty = negative */
    bool used;
} fa_entry_t;

static fa_entry_t *s_cache;
static int s_next;
static int64_t s_last_call_us = -FA_MIN_GAP_US;
static int64_t s_hold_until_us;

const char *faflight_get_cached(const char *callsign)
{
    if (callsign == NULL || callsign[0] == '\0' || s_cache == NULL) {
        return NULL;
    }
    for (int i = 0; i < FA_CACHE_SIZE; i++) {
        if (s_cache[i].used && strcmp(s_cache[i].callsign, callsign) == 0) {
            return s_cache[i].iata;
        }
    }
    return NULL;
}

static int month_limit(void)
{
    int l = settings_get()->fa_limit;
    return l > 0 ? l : FA_DEFAULT_MONTHLY;
}

/* UTC month index (year*12+month) and the fraction of it elapsed;
 * -1 while the clock is not set yet. */
static int month_now(double *elapsed)
{
    time_t now = time(NULL);
    if (now < 1600000000) {
        return -1;
    }
    struct tm tm;
    gmtime_r(&now, &tm);
    int idx = (tm.tm_year + 1900) * 12 + tm.tm_mon;
    if (elapsed != NULL) {
        static const uint8_t mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        int y = tm.tm_year + 1900;
        int days = mdays[tm.tm_mon];
        if (tm.tm_mon == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) {
            days = 29;
        }
        double secs = (tm.tm_mday - 1) * 86400.0 + tm.tm_hour * 3600.0 +
                      tm.tm_min * 60.0 + tm.tm_sec;
        *elapsed = secs / (days * 86400.0);
        if (*elapsed > 1) {
            *elapsed = 1;
        }
    }
    return idx;
}

/* Roll the persisted counter over when a new month starts. */
static void month_roll(int idx)
{
    settings_t *cfg = settings_get();
    if (idx > 0 && cfg->fa_month != (uint16_t)idx) {
        cfg->fa_month = (uint16_t)idx;
        cfg->fa_used = 0;
        settings_save_fa_usage();
    }
}

void faflight_usage(int *used, int *limit)
{
    int idx = month_now(NULL);
    const settings_t *cfg = settings_get();
    *used = (idx > 0 && cfg->fa_month == (uint16_t)idx) ? cfg->fa_used : 0;
    *limit = month_limit();
}

bool faflight_can_fetch(void)
{
    const settings_t *cfg = settings_get();
    if (cfg->fa_key[0] == '\0') {
        return false;
    }
    int64_t now_us = esp_timer_get_time();
    if (now_us < s_hold_until_us || now_us - s_last_call_us < FA_MIN_GAP_US) {
        return false;
    }
    double elapsed;
    int idx = month_now(&elapsed);
    if (idx < 0) {
        return false;     /* no clock = no idea which month is billed */
    }
    month_roll(idx);
    int limit = month_limit();
    int allowed = (int)(limit * elapsed) + FA_BURST;
    if (allowed > limit) {
        allowed = limit;
    }
    return cfg->fa_used < allowed;
}

const char *faflight_fetch(const char *callsign)
{
    const char *cached = faflight_get_cached(callsign);
    if (cached != NULL) {
        return cached;
    }
    if (!faflight_can_fetch()) {
        return NULL;
    }
    if (s_cache == NULL) {
        s_cache = heap_caps_calloc(FA_CACHE_SIZE, sizeof(fa_entry_t), MALLOC_CAP_SPIRAM);
        if (s_cache == NULL) {
            return NULL;
        }
    }
    char *buf = heap_caps_malloc(FA_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        return NULL;
    }

    /* Count the call before it goes out: a crash or power cut mid-request
     * must never hand the budget back. */
    settings_t *cfg = settings_get();
    cfg->fa_used++;
    settings_save_fa_usage();
    s_last_call_us = esp_timer_get_time();

    fa_entry_t *slot = &s_cache[s_next];
    s_next = (s_next + 1) % FA_CACHE_SIZE;
    memset(slot, 0, sizeof(*slot));
    strlcpy(slot->callsign, callsign, sizeof(slot->callsign));
    slot->used = true;

    char url[128];
    snprintf(url, sizeof(url),
             "https://aeroapi.flightaware.com/aeroapi/flights/%s?max_pages=1", callsign);
    esp_err_t err = http_get_to_buffer_hdr(url, buf, FA_BUF_SIZE, NULL, "x-apikey", cfg->fa_key);
    if (err == ESP_OK) {
        cJSON *root = cJSON_Parse(buf);
        if (root != NULL) {
            const cJSON *flights = cJSON_GetObjectItem(root, "flights");
            const cJSON *first = cJSON_IsArray(flights) ? cJSON_GetArrayItem(flights, 0) : NULL;
            const cJSON *iata = first ? cJSON_GetObjectItem(first, "ident_iata") : NULL;
            if (cJSON_IsString(iata) && iata->valuestring[0] != '\0' &&
                strcmp(iata->valuestring, callsign) != 0) {
                strlcpy(slot->iata, iata->valuestring, sizeof(slot->iata));
            }
            /* the same response carries the live origin/destination: use it
             * as an authoritative route when the free DBs came up empty */
            const cJSON *jo = first ? cJSON_GetObjectItem(first, "origin") : NULL;
            const cJSON *jd = first ? cJSON_GetObjectItem(first, "destination") : NULL;
            const cJSON *oc = jo ? cJSON_GetObjectItem(jo, "code_icao") : NULL;
            const cJSON *dc = jd ? cJSON_GetObjectItem(jd, "code_icao") : NULL;
            if (!cJSON_IsString(oc) && jo) {
                oc = cJSON_GetObjectItem(jo, "code");
            }
            if (!cJSON_IsString(dc) && jd) {
                dc = cJSON_GetObjectItem(jd, "code");
            }
            if (cJSON_IsString(oc) && cJSON_IsString(dc)) {
                routes_inject(callsign, oc->valuestring, dc->valuestring);
            }
            cJSON_Delete(root);
        }
    } else {
        int status = err - ESP_ERR_HTTP_BASE;
        if (status == 401 || status == 403) {
            s_hold_until_us = s_last_call_us + FA_AUTH_HOLD_US;
        } else if (status == 429) {
            s_hold_until_us = s_last_call_us + FA_RATE_HOLD_US;
        }
        if (status < 400 || status >= 600) {
            slot->used = false;   /* network trouble: not an answer, ask later */
        }
    }
    free(buf);
    ESP_LOGI(TAG, "%s -> \"%s\" (%u/%d this month)", callsign, slot->iata,
             cfg->fa_used, month_limit());
    return slot->used ? slot->iata : NULL;
}
