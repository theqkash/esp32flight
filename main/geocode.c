#include "geocode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "http_util.h"

static const char *TAG = "geocode";

static void url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *src && o + 4 < dst_size; src++) {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            dst[o++] = c;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 0xF];
        }
    }
    dst[o] = '\0';
}

esp_err_t geocode_search(const char *query, geocode_result_t *results, int max, int *count)
{
    *count = 0;

    char encoded[96];
    url_encode(query, encoded, sizeof(encoded));
    char url[224];
    snprintf(url, sizeof(url),
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=%d&language=en&format=json",
             encoded, max);

    char *buf = malloc(16 * 1024);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = http_get_to_buffer(url, buf, 16 * 1024, NULL);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *jlist = cJSON_GetObjectItem(root, "results");
    const cJSON *item;
    cJSON_ArrayForEach(item, jlist) {
        if (*count >= max) {
            break;
        }
        const cJSON *jname = cJSON_GetObjectItem(item, "name");
        const cJSON *jlat = cJSON_GetObjectItem(item, "latitude");
        const cJSON *jlon = cJSON_GetObjectItem(item, "longitude");
        if (!cJSON_IsString(jname) || !cJSON_IsNumber(jlat) || !cJSON_IsNumber(jlon)) {
            continue;
        }
        geocode_result_t *r = &results[*count];
        memset(r, 0, sizeof(*r));
        strlcpy(r->name, jname->valuestring, sizeof(r->name));
        r->lat = jlat->valuedouble;
        r->lon = jlon->valuedouble;
        const cJSON *jc = cJSON_GetObjectItem(item, "country_code");
        if (cJSON_IsString(jc)) {
            strlcpy(r->country, jc->valuestring, sizeof(r->country));
        }
        const cJSON *ja = cJSON_GetObjectItem(item, "admin1");
        if (cJSON_IsString(ja)) {
            strlcpy(r->region, ja->valuestring, sizeof(r->region));
        }
        (*count)++;
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "\"%s\" -> %d results", query, *count);
    return ESP_OK;
}
