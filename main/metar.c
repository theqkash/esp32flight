#include "metar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "http_util.h"

static const char *TAG = "metar";

static char s_metar[160];

bool metar_fetch(const char *icao)
{
    char url[128];
    snprintf(url, sizeof(url),
             "https://aviationweather.gov/api/data/metar?ids=%s&format=json", icao);
    char *buf = malloc(4096);
    if (buf == NULL) {
        return false;
    }
    bool ok = false;
    if (http_get_to_buffer(url, buf, 4096, NULL) == ESP_OK) {
        cJSON *root = cJSON_Parse(buf);
        const cJSON *first = cJSON_IsArray(root) ? cJSON_GetArrayItem(root, 0) : NULL;
        const cJSON *raw = first ? cJSON_GetObjectItem(first, "rawOb") : NULL;
        if (cJSON_IsString(raw)) {
            strlcpy(s_metar, raw->valuestring, sizeof(s_metar));
            ok = true;
            ESP_LOGI(TAG, "%s", s_metar);
        }
        if (root != NULL) {
            cJSON_Delete(root);
        }
    }
    free(buf);
    return ok;
}

const char *metar_get(void)
{
    return s_metar;
}
