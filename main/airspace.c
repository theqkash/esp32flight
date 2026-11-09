#include "airspace.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "geo_math.h"
#include "http_util.h"
#include "settings.h"

static const char *TAG = "airspace";

#define FETCH_DIST_M   50000   /* openAIP caps dist at 50 km */
#define RETRY_US       (30LL * 60 * 1000 * 1000)
/* With the server-side type filter below a 50 km answer is ~40 KB
 * (was ~340 KB unfiltered, which cJSON could not parse in 7B PSRAM). */
#define BUF_SIZE       (128 * 1024)

static airspace_t *s_asp;     /* PSRAM, MAX_AIRSPACES entries */
static int s_count;
static double s_try_lat = 999, s_try_lon = 999;
static int64_t s_last_try = -RETRY_US;
static char s_try_key[64];
static bool s_ok;             /* last fetch for s_try location and key succeeded */

/* openAIP numeric airspace types, the handful worth naming on screen */
const char *airspace_type_str(int type)
{
    switch (type) {
    case 1:  return "R";     /* restricted */
    case 2:  return "D";     /* danger */
    case 3:  return "P";     /* prohibited */
    case 4:  return "CTR";
    case 7:  return "TMA";
    case 13: return "ATZ";
    case 21: return "TRA";
    case 26: return "MTMA";
    case 27: return "MCTR";
    default: return "";
    }
}

static bool type_wanted(int type)
{
    return airspace_type_str(type)[0] != '\0';
}

static bool parse_airspaces(const char *json, double home_lat, double home_lon)
{
    /* Parse first: a failed parse (out of memory, truncated answer) used
     * to wipe the outlines on screen and log nothing, which looked
     * exactly like a bad API key (#35). Now the old set stays. */
    cJSON *root = cJSON_Parse(json);
    const cJSON *items = cJSON_GetObjectItem(root, "items");
    if (root == NULL || !cJSON_IsArray(items)) {
        ESP_LOGW(TAG, "airspace answer not parsed (%s)", root ? "no items" : "JSON/memory");
        cJSON_Delete(root);
        return false;
    }
    s_count = 0;   /* renderer reads from another task; hide during rewrite */
    int n = 0;
    for (const cJSON *it = items != NULL ? items->child : NULL;
         it != NULL && n < MAX_AIRSPACES; it = it->next) {
        const cJSON *ty = cJSON_GetObjectItem(it, "type");
        if (!cJSON_IsNumber(ty) || !type_wanted(ty->valueint)) {
            continue;
        }
        const cJSON *geom = cJSON_GetObjectItem(it, "geometry");
        const cJSON *coords = geom != NULL ? cJSON_GetObjectItem(geom, "coordinates") : NULL;
        /* Polygon: coordinates[0] = outer ring of [lon, lat] pairs.
           MultiPolygon nests one level deeper; take its first polygon. */
        const cJSON *ring = coords != NULL ? coords->child : NULL;
        if (ring != NULL && ring->child != NULL && ring->child->child != NULL &&
            cJSON_IsArray(ring->child->child)) {
            ring = ring->child;
        }
        if (ring == NULL) {
            continue;
        }
        int total = cJSON_GetArraySize((cJSON *)ring);
        if (total < 4) {
            continue;
        }
        airspace_t *a = &s_asp[n];
        memset(a, 0, sizeof(*a));
        a->type = ty->valueint;
        const cJSON *nm = cJSON_GetObjectItem(it, "name");
        if (cJSON_IsString(nm)) {
            strlcpy(a->name, nm->valuestring, sizeof(a->name));
        }
        /* decimate long rings down to MAX_ASP_POINTS */
        int step = (total + MAX_ASP_POINTS - 1) / MAX_ASP_POINTS;
        if (step < 1) {
            step = 1;
        }
        int idx = 0;
        for (const cJSON *pt = ring->child; pt != NULL && a->n_points < MAX_ASP_POINTS;
             pt = pt->next, idx++) {
            if (idx % step != 0) {
                continue;
            }
            const cJSON *plon = pt->child;
            const cJSON *plat = plon != NULL ? plon->next : NULL;
            if (!cJSON_IsNumber(plon) || !cJSON_IsNumber(plat)) {
                break;
            }
            a->lat[a->n_points] = (float)plat->valuedouble;
            a->lon[a->n_points] = (float)plon->valuedouble;
            a->n_points++;
        }
        if (a->n_points >= 4) {
            n++;
        }
    }
    if (root != NULL) {
        cJSON_Delete(root);
    }
    s_count = n;
    (void)home_lat;
    (void)home_lon;
    ESP_LOGI(TAG, "%d airspace outlines cached", n);
    return true;
}

void airspace_poll(double home_lat, double home_lon)
{
    const settings_t *cfg = settings_get();
    if (!cfg->airspace_enabled || cfg->openaip_key[0] == '\0') {
        s_count = 0;
        return;
    }
    bool moved = geo_haversine_km(s_try_lat, s_try_lon, home_lat, home_lon) > 25.0;
    bool rekeyed = strcmp(s_try_key, cfg->openaip_key) != 0;
    if (s_ok && !moved && !rekeyed) {
        return;
    }
    /* after a failed attempt wait out the retry window, even for a new key:
       hammering the API every poll cycle trips its rate limit */
    int64_t now = esp_timer_get_time();
    if (now - s_last_try < RETRY_US) {
        return;
    }
    s_last_try = now;
    s_try_lat = home_lat;
    s_try_lon = home_lon;
    strlcpy(s_try_key, cfg->openaip_key, sizeof(s_try_key));

    if (s_asp == NULL) {
        s_asp = heap_caps_calloc(MAX_AIRSPACES, sizeof(airspace_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_asp == NULL) {
            return;
        }
    }
    char *buf = heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        return;
    }
    char url[280];
    snprintf(url, sizeof(url),
             "https://api.core.openaip.net/api/airspaces?pos=%.4f,%.4f&dist=%d&limit=60"
             /* only the types airspace_type_str() draws */
             "&type=1&type=2&type=3&type=4&type=7&type=13&type=21&type=26&type=27",
             home_lat, home_lon, FETCH_DIST_M);
    if (http_get_to_buffer_hdr(url, buf, BUF_SIZE, NULL,
                               "x-openaip-api-key", cfg->openaip_key) == ESP_OK) {
        s_ok = parse_airspaces(buf, home_lat, home_lon);
    } else {
        s_ok = false;
        ESP_LOGW(TAG, "fetch failed (rate limit, bad key or offline), retry in 30 min");
    }
    heap_caps_free(buf);
}

int airspace_count(void)
{
    return s_count;
}

const airspace_t *airspace_get(int idx)
{
    return (idx >= 0 && idx < s_count) ? &s_asp[idx] : NULL;
}
