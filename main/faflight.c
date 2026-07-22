#include "faflight.h"

#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "http_util.h"
#include "settings.h"

static const char *TAG = "faflight";

#define FA_CACHE_SIZE 48
#define FA_BUF_SIZE   (48 * 1024)

typedef struct {
    char callsign[9];
    char iata[10];      /* empty = negative */
    bool used;
} fa_entry_t;

static fa_entry_t s_cache[FA_CACHE_SIZE];
static int s_next;

const char *faflight_get_cached(const char *callsign)
{
    if (callsign == NULL || callsign[0] == '\0') {
        return NULL;
    }
    for (int i = 0; i < FA_CACHE_SIZE; i++) {
        if (s_cache[i].used && strcmp(s_cache[i].callsign, callsign) == 0) {
            return s_cache[i].iata;
        }
    }
    return NULL;
}

const char *faflight_fetch(const char *callsign)
{
    const char *cached = faflight_get_cached(callsign);
    if (cached != NULL) {
        return cached;
    }
    const char *key = settings_get()->fa_key;
    if (key[0] == '\0') {
        return NULL;
    }

    fa_entry_t *slot = &s_cache[s_next];
    s_next = (s_next + 1) % FA_CACHE_SIZE;
    memset(slot, 0, sizeof(*slot));
    strlcpy(slot->callsign, callsign, sizeof(slot->callsign));
    slot->used = true;

    char *buf = heap_caps_malloc(FA_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        return slot->iata;
    }
    char url[128];
    snprintf(url, sizeof(url),
             "https://aeroapi.flightaware.com/aeroapi/flights/%s?max_pages=1", callsign);
    if (http_get_to_buffer_hdr(url, buf, FA_BUF_SIZE, NULL, "x-apikey", key) == ESP_OK) {
        cJSON *root = cJSON_Parse(buf);
        if (root != NULL) {
            const cJSON *flights = cJSON_GetObjectItem(root, "flights");
            const cJSON *first = cJSON_IsArray(flights) ? cJSON_GetArrayItem(flights, 0) : NULL;
            const cJSON *iata = first ? cJSON_GetObjectItem(first, "ident_iata") : NULL;
            if (cJSON_IsString(iata) && iata->valuestring[0] != '\0' &&
                strcmp(iata->valuestring, callsign) != 0) {
                strlcpy(slot->iata, iata->valuestring, sizeof(slot->iata));
            }
            cJSON_Delete(root);
        }
    }
    free(buf);
    ESP_LOGI(TAG, "%s -> \"%s\"", callsign, slot->iata);
    return slot->iata;
}
