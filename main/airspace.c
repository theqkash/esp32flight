#include "airspace.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "airports.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "geo_math.h"
#include "http_util.h"
#include "settings.h"

static const char *TAG = "airspace";

#define RETRY_US       (30LL * 60 * 1000 * 1000)
/* OpenAIP removed spatial pos/bbox params from /api/airspaces (WAF returns
 * HTTP 403). Fetch by country with fields=name,type,geometry and filter
 * polygons near home locally. */
#define PAGE_LIMIT     80
#define MAX_PAGES      10
#define BUF_SIZE       (256 * 1024)

static airspace_t *s_asp;     /* PSRAM, MAX_AIRSPACES entries */
static int s_count;
static double s_try_lat = 999, s_try_lon = 999;
static int64_t s_last_try = -RETRY_US;
static char s_try_key[64];
static bool s_ok;             /* last fetch for s_try location and key succeeded */

/* openAIP numeric airspace types (per api.core.openaip.net schema),
 * the handful worth naming on screen */
const char *airspace_type_str(int type)
{
    switch (type) {
    case 1:  return "R";     /* restricted */
    case 2:  return "D";     /* danger */
    case 3:  return "P";     /* prohibited */
    case 4:  return "CTR";
    case 7:  return "TMA";
    case 8:  return "TRA";   /* temporary reserved area */
    case 13: return "ATZ";
    case 26: return "CTA";   /* control area */
    case 36: return "MCTR";  /* military CTR */
    default: return "";
    }
}

static bool type_wanted(int type)
{
    return airspace_type_str(type)[0] != '\0';
}

typedef struct {
    airspace_t asp;
    float      dist_km;
} asp_cand_t;

/* Parse one page of OpenAIP /airspaces items, keeping polygons within
 * max_km of (home_lat, home_lon). Keeps the closest MAX_AIRSPACES entries.
 * Returns totalPages (>0) on success, or 0 on parse failure. */
static int parse_airspace_page(const char *json, double home_lat, double home_lon,
                               double max_km, asp_cand_t *cands, int *cand_n)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *items = cJSON_GetObjectItem(root, "items");
    if (root == NULL || !cJSON_IsArray(items)) {
        ESP_LOGW(TAG, "airspace answer not parsed (%s)", root ? "no items" : "JSON/memory");
        cJSON_Delete(root);
        return 0;
    }
    int total_pages = 1;
    const cJSON *jtp = cJSON_GetObjectItem(root, "totalPages");
    if (cJSON_IsNumber(jtp) && jtp->valueint >= 1) {
        total_pages = jtp->valueint;
    }

    for (const cJSON *it = items->child; it != NULL; it = it->next) {
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

        /* Quick spatial filter: find polygon bbox and closest vertex to home. */
        double min_lat = 999, max_lat = -999, min_lon = 999, max_lon = -999;
        double min_d = 1e9;
        for (const cJSON *pt = ring->child; pt != NULL; pt = pt->next) {
            const cJSON *plon = pt->child;
            const cJSON *plat = plon != NULL ? plon->next : NULL;
            if (!cJSON_IsNumber(plon) || !cJSON_IsNumber(plat)) {
                break;
            }
            double la = plat->valuedouble;
            double lo = plon->valuedouble;
            if (la < min_lat) min_lat = la;
            if (la > max_lat) max_lat = la;
            if (lo < min_lon) min_lon = lo;
            if (lo > max_lon) max_lon = lo;
            double d = geo_haversine_km(home_lat, home_lon, la, lo);
            if (d < min_d) {
                min_d = d;
            }
        }
        /* Home inside the polygon's bbox (e.g. a wide TMA surrounding us)
         * counts as distance 0 so it is never dropped. */
        if (home_lat >= min_lat && home_lat <= max_lat &&
            home_lon >= min_lon && home_lon <= max_lon) {
            min_d = 0.0;
        }
        if (min_d > max_km) {
            continue;
        }

        /* Find insertion slot: fill up to MAX_AIRSPACES, else replace the
         * farthest entry if this polygon is closer. */
        int slot = -1;
        if (*cand_n < MAX_AIRSPACES) {
            slot = (*cand_n)++;
        } else {
            int worst = 0;
            for (int i = 1; i < MAX_AIRSPACES; i++) {
                if (cands[i].dist_km > cands[worst].dist_km) {
                    worst = i;
                }
            }
            if ((float)min_d < cands[worst].dist_km) {
                slot = worst;
            }
        }
        if (slot < 0) {
            continue;
        }

        airspace_t *a = &cands[slot].asp;
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
            cands[slot].dist_km = (float)min_d;
        } else if (slot == *cand_n - 1) {
            (*cand_n)--;
        }
    }

    cJSON_Delete(root);
    return total_pages;
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

    /* Resolve home ISO alpha-2 country code offline from the bundled airport DB. */
    char icao[5] = { 0 };
    char cc[4] = { 0 };
    airport_t ap;
    if (airports_nearest(home_lat, home_lon, icao) &&
        airports_lookup(icao, &ap) && ap.country[0] != '\0') {
        strlcpy(cc, ap.country, sizeof(cc));
    }
    if (cc[0] == '\0') {
        ESP_LOGW(TAG, "cannot resolve home country for openAIP query");
        s_ok = false;
        return;
    }

    if (s_asp == NULL) {
        s_asp = heap_caps_calloc(MAX_AIRSPACES, sizeof(airspace_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_asp == NULL) {
            return;
        }
    }
    char *buf = heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    asp_cand_t *cands = heap_caps_calloc(MAX_AIRSPACES, sizeof(asp_cand_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL || cands == NULL) {
        heap_caps_free(buf);
        heap_caps_free(cands);
        return;
    }

    double max_km = (double)cfg->radius_nm * 1.852 * 1.5;
    if (max_km < 80.0) {
        max_km = 80.0;
    }
    if (max_km > 250.0) {
        max_km = 250.0;
    }

    int cand_n = 0;
    int total_pages = 1;
    bool any_ok = false;
    for (int page = 1; page <= total_pages && page <= MAX_PAGES; page++) {
        char url[320];
        snprintf(url, sizeof(url),
                 "https://api.core.openaip.net/api/airspaces"
                 "?country=%s&fields=name,type,geometry&limit=%d&page=%d"
                 "&type=1&type=2&type=3&type=4&type=7&type=8&type=13&type=26&type=36",
                 cc, PAGE_LIMIT, page);
        if (http_get_to_buffer_hdr(url, buf, BUF_SIZE, NULL,
                                   "x-openaip-api-key", cfg->openaip_key) != ESP_OK) {
            if (!any_ok) {
                ESP_LOGW(TAG, "fetch failed (rate limit, bad key or offline), retry in 30 min");
            }
            break;
        }
        int tp = parse_airspace_page(buf, home_lat, home_lon, max_km, cands, &cand_n);
        if (tp <= 0) {
            break;
        }
        any_ok = true;
        total_pages = tp;
    }

    if (any_ok) {
        s_count = 0;   /* renderer reads from another task; hide during rewrite */
        for (int i = 0; i < cand_n; i++) {
            s_asp[i] = cands[i].asp;
        }
        s_count = cand_n;
        s_ok = true;
        ESP_LOGI(TAG, "%d airspace outlines cached (%s, within %.0f km)",
                 cand_n, cc, max_km);
    } else {
        s_ok = false;
    }

    heap_caps_free(buf);
    heap_caps_free(cands);
}

int airspace_count(void)
{
    return s_count;
}

const airspace_t *airspace_get(int idx)
{
    return (idx >= 0 && idx < s_count) ? &s_asp[idx] : NULL;
}
