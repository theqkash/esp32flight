#include "airlines.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "http_util.h"

static const char *TAG = "airlines";

#define AIRLINE_CACHE_SIZE 96
#define AIRLINE_NAME_LEN   48

typedef struct {
    char icao[4];
    char name[AIRLINE_NAME_LEN];   /* empty string = negative-cached */
    bool used;
} airline_entry_t;

static airline_entry_t s_cache[AIRLINE_CACHE_SIZE];
static int s_next;

const char *airlines_get_cached(const char *icao)
{
    if (icao == NULL || icao[0] == '\0') {
        return NULL;
    }
    for (int i = 0; i < AIRLINE_CACHE_SIZE; i++) {
        if (s_cache[i].used && strcmp(s_cache[i].icao, icao) == 0) {
            return s_cache[i].name;
        }
    }
    return NULL;
}

const char *airlines_fetch(const char *icao)
{
    const char *cached = airlines_get_cached(icao);
    if (cached != NULL) {
        return cached;
    }

    airline_entry_t *slot = &s_cache[s_next];
    s_next = (s_next + 1) % AIRLINE_CACHE_SIZE;
    memset(slot, 0, sizeof(*slot));
    strlcpy(slot->icao, icao, sizeof(slot->icao));
    slot->used = true;

    char *buf = malloc(4096);
    if (buf == NULL) {
        return slot->name;
    }
    char url[80];
    snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/airline/%s", icao);
    if (http_get_to_buffer(url, buf, 4096, NULL) == ESP_OK) {
        cJSON *root = cJSON_Parse(buf);
        if (root != NULL) {
            const cJSON *resp = cJSON_GetObjectItem(root, "response");
            const cJSON *first = cJSON_IsArray(resp) ? cJSON_GetArrayItem(resp, 0) : NULL;
            const cJSON *name = first ? cJSON_GetObjectItem(first, "name") : NULL;
            if (cJSON_IsString(name)) {
                strlcpy(slot->name, name->valuestring, sizeof(slot->name));
            }
            cJSON_Delete(root);
        }
    }
    free(buf);
    ESP_LOGI(TAG, "%s -> \"%s\"", icao, slot->name);
    return slot->name;
}
