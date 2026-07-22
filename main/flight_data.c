#include "flight_data.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "http_util.h"

static const char *TAG = "flights";

#define FETCH_BUF_SIZE (128 * 1024)

bool flight_is_airline(const aircraft_t *ac)
{
    const char *cs = ac->callsign;
    if (!(isalpha((unsigned char)cs[0]) &&
          isalpha((unsigned char)cs[1]) &&
          isalpha((unsigned char)cs[2]) &&
          isdigit((unsigned char)cs[3]))) {
        return false;
    }
    /* Airline-shaped callsigns are also used by air taxis and club planes;
     * weed those out by ADS-B emitter category: A1 = light (<7 t),
     * A7 = rotorcraft, B* = glider/balloon/UAV, C* = surface. */
    if (strcmp(ac->category, "A1") == 0 || strcmp(ac->category, "A7") == 0 ||
        ac->category[0] == 'B' || ac->category[0] == 'C') {
        return false;
    }
    return true;
}

bool flight_is_interesting(const aircraft_t *ac, const char *watchlist)
{
    if (ac->military) {
        return true;
    }
    static const char *types[] = {
        "A388", "A124", "A225", "B748", "B744", "C17", "C5M", "C5",
        "K35R", "A400", "B52", "VC25", "CONC",
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        if (strcmp(ac->type_icao, types[i]) == 0) {
            return true;
        }
    }
    if (watchlist != NULL && watchlist[0] != '\0') {
        char list[96];
        strlcpy(list, watchlist, sizeof(list));
        char *save = NULL;
        for (char *tok = strtok_r(list, ", ", &save); tok != NULL;
             tok = strtok_r(NULL, ", ", &save)) {
            size_t n = strlen(tok);
            if (n >= 2 && (strncasecmp(ac->reg, tok, n) == 0 ||
                           strncasecmp(ac->callsign, tok, n) == 0)) {
                return true;
            }
        }
    }
    return false;
}

static void copy_trimmed(char *dst, size_t dst_size, const char *src)
{
    while (*src == ' ') {
        src++;
    }
    strlcpy(dst, src, dst_size);
    for (int i = (int)strlen(dst) - 1; i >= 0 && dst[i] == ' '; i--) {
        dst[i] = '\0';
    }
}

static void parse_aircraft(const cJSON *jac, aircraft_t *ac)
{
    memset(ac, 0, sizeof(*ac));

    const cJSON *j;
    if ((j = cJSON_GetObjectItem(jac, "hex")) && cJSON_IsString(j)) {
        strlcpy(ac->hex, j->valuestring, sizeof(ac->hex));
    }
    if ((j = cJSON_GetObjectItem(jac, "flight")) && cJSON_IsString(j)) {
        copy_trimmed(ac->callsign, sizeof(ac->callsign), j->valuestring);
    }
    if ((j = cJSON_GetObjectItem(jac, "r")) && cJSON_IsString(j)) {
        strlcpy(ac->reg, j->valuestring, sizeof(ac->reg));
    }
    if ((j = cJSON_GetObjectItem(jac, "t")) && cJSON_IsString(j)) {
        strlcpy(ac->type_icao, j->valuestring, sizeof(ac->type_icao));
    }
    if ((j = cJSON_GetObjectItem(jac, "desc")) && cJSON_IsString(j)) {
        strlcpy(ac->type_desc, j->valuestring, sizeof(ac->type_desc));
    }
    if ((j = cJSON_GetObjectItem(jac, "lat")) && cJSON_IsNumber(j)) {
        ac->lat = j->valuedouble;
        ac->has_pos = true;
    }
    if ((j = cJSON_GetObjectItem(jac, "lon")) && cJSON_IsNumber(j)) {
        ac->lon = j->valuedouble;
    } else {
        ac->has_pos = false;
    }

    j = cJSON_GetObjectItem(jac, "alt_baro");
    if (cJSON_IsNumber(j)) {
        ac->alt_baro_ft = (int)j->valuedouble;
    } else if (cJSON_IsString(j) && strcmp(j->valuestring, "ground") == 0) {
        ac->on_ground = true;
    }

    if ((j = cJSON_GetObjectItem(jac, "gs")) && cJSON_IsNumber(j)) {
        ac->gs_kts = (float)j->valuedouble;
    }
    if ((j = cJSON_GetObjectItem(jac, "track")) && cJSON_IsNumber(j)) {
        ac->track_deg = (float)j->valuedouble;
    }
    if ((j = cJSON_GetObjectItem(jac, "baro_rate")) && cJSON_IsNumber(j)) {
        ac->baro_rate_fpm = (int)j->valuedouble;
    }
    if ((j = cJSON_GetObjectItem(jac, "dst")) && cJSON_IsNumber(j)) {
        ac->dist_nm = (float)j->valuedouble;
    } else {
        ac->dist_nm = -1.0f;
    }
    if ((j = cJSON_GetObjectItem(jac, "dir")) && cJSON_IsNumber(j)) {
        ac->dir_deg = (float)j->valuedouble;
    }
    if ((j = cJSON_GetObjectItem(jac, "category")) && cJSON_IsString(j)) {
        strlcpy(ac->category, j->valuestring, sizeof(ac->category));
    }
    if ((j = cJSON_GetObjectItem(jac, "squawk")) && cJSON_IsString(j)) {
        strlcpy(ac->squawk, j->valuestring, sizeof(ac->squawk));
    }
    if ((j = cJSON_GetObjectItem(jac, "dbFlags")) && cJSON_IsNumber(j)) {
        ac->military = ((int)j->valuedouble & 1) != 0;
    }
}

static int cmp_by_dist(const void *a, const void *b)
{
    const aircraft_t *aa = a, *bb = b;
    float da = aa->dist_nm < 0 ? 1e9f : aa->dist_nm;
    float db = bb->dist_nm < 0 ? 1e9f : bb->dist_nm;
    return (da > db) - (da < db);
}

static esp_err_t parse_point_response(const char *json, aircraft_list_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *jlist = cJSON_GetObjectItem(root, "ac");
    if (!cJSON_IsArray(jlist)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    out->count = 0;
    const cJSON *jac;
    cJSON_ArrayForEach(jac, jlist) {
        if (out->count >= MAX_AIRCRAFT) {
            break;
        }
        aircraft_t *ac = &out->ac[out->count];
        parse_aircraft(jac, ac);
        /* Skip targets without position: nothing to show or compute. */
        if (ac->has_pos) {
            out->count++;
        }
    }
    cJSON_Delete(root);

    qsort(out->ac, out->count, sizeof(aircraft_t), cmp_by_dist);
    out->fetched_at_ms = esp_timer_get_time() / 1000;
    return ESP_OK;
}

esp_err_t flight_fetch_nearby(double lat, double lon, int radius_nm, aircraft_list_t *out)
{
    char *buf = heap_caps_malloc(FETCH_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const char *bases[] = {
        "https://api.airplanes.live/v2/point",
        "https://api.adsb.lol/v2/point",
    };
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 2; i++) {
        char url[128];
        snprintf(url, sizeof(url), "%s/%.4f/%.4f/%d", bases[i], lat, lon, radius_nm);
        err = http_get_to_buffer(url, buf, FETCH_BUF_SIZE, NULL);
        if (err == ESP_OK) {
            err = parse_point_response(buf, out);
        }
        if (err == ESP_OK) {
            if (i > 0) {
                ESP_LOGI(TAG, "using fallback source adsb.lol");
            }
            break;
        }
        ESP_LOGW(TAG, "source %d failed (%s)", i, esp_err_to_name(err));
    }

    free(buf);
    return err;
}
