#include "routes.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "airports.h"
#include "esp_timer.h"
#include "geo_math.h"
#include "http_util.h"
#include "esp_http_client.h"
#include "tz.h"

/* Negative entries retry after this long: route DBs fill in during the day */
#define NEGATIVE_TTL_MS (30LL * 60 * 1000)
#define TRANSIENT_RETRY_MS (60LL * 1000)   /* failed lookup, not an authoritative miss */

static const char *TAG = "routes";

#define ROUTE_CACHE_SIZE 160

/* Large enough to hold a busy area without eviction churn; lives in PSRAM. */
static route_info_t *s_cache;
static int s_used;
static int s_next_evict;

static bool cache_ready(void)
{
    if (s_cache == NULL) {
        s_cache = heap_caps_calloc(ROUTE_CACHE_SIZE, sizeof(route_info_t),
                                   MALLOC_CAP_SPIRAM);
    }
    return s_cache != NULL;
}

const route_info_t *routes_get_cached(const char *callsign)
{
    if (callsign == NULL || callsign[0] == '\0' || !cache_ready()) {
        return NULL;
    }
    for (int i = 0; i < s_used; i++) {
        if (strcmp(s_cache[i].callsign, callsign) == 0) {
            if (!s_cache[i].valid &&
                esp_timer_get_time() / 1000 - s_cache[i].fetched_ms > NEGATIVE_TTL_MS) {
                s_cache[i].callsign[0] = '\0';   /* expired: allow a re-fetch */
                return NULL;
            }
            return &s_cache[i];
        }
    }
    return NULL;
}

static route_info_t *cache_slot(const char *callsign)
{
    if (!cache_ready()) {
        return NULL;
    }
    route_info_t *slot;
    if (s_used < ROUTE_CACHE_SIZE) {
        slot = &s_cache[s_used++];
    } else {
        slot = &s_cache[s_next_evict];
        s_next_evict = (s_next_evict + 1) % ROUTE_CACHE_SIZE;
    }
    memset(slot, 0, sizeof(*slot));
    strlcpy(slot->callsign, callsign, sizeof(slot->callsign));
    slot->fetched_ms = esp_timer_get_time() / 1000;
    return slot;
}

/* adsbdb's municipality is sometimes the airport's village rather than the
 * city everyone knows (e.g. UGKO -> "Kopitnari"). Curated overrides: */
static void fix_city(airport_t *ap)
{
    static const struct { const char *icao, *city; } overrides[] = {
        { "UGKO", "Kutaisi" },
        { "EPMO", "Warsaw-Modlin" },
        { "LTFM", "Istanbul" },
        { "EPKK", "Kraków" },
        { "EPWR", "Wrocław" },
        { "EPPO", "Poznań" },
        { "LIMC", "Milan" },
        { "LIME", "Bergamo" },
        { "LFPG", "Paris" },
        { "LFPO", "Paris" },
        { "EGKK", "London" },
        { "EGSS", "London" },
        { "EGGW", "London" },
        { "EGLC", "London" },
    };
    for (size_t i = 0; i < sizeof(overrides) / sizeof(overrides[0]); i++) {
        if (strcmp(ap->icao, overrides[i].icao) == 0) {
            strlcpy(ap->city, overrides[i].city, sizeof(ap->city));
            return;
        }
    }
    /* If the "city" is actually an airport name, extract the city part.
     * "Debrecen International Airport" -> "Debrecen" (suffix at the end),
     * "Copernicus Airport Wroclaw" -> "Wroclaw" (city after "Airport"). */
    static const char *suffixes[] = {
        " International Airport", " Intl Airport", " Airport",
        " Airfield", " Air Base", " Airbase",
    };
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        char *at = strstr(ap->city, suffixes[i]);
        if (at == NULL || at == ap->city) {
            continue;
        }
        const char *tail = at + strlen(suffixes[i]);
        if (*tail == '\0') {
            *at = '\0';                 /* "... Airport" at the end */
        } else if (*tail == ' ' && tail[1] != '\0') {
            memmove(ap->city, tail + 1, strlen(tail + 1) + 1);
        } else {
            continue;
        }
        break;
    }
    /* "Poznań–Ławica Henryk Wieniawski" -> "Poznań" (en-dash separates the
     * city from the airport's proper name) */
    char *dash = strstr(ap->city, "\xE2\x80\x93");
    if (dash != NULL && dash != ap->city) {
        *dash = '\0';
    }
}

/* Prefer the local OurAirports data (consistent city names with proper
 * diacritics) over whatever the route API returned. */
static void enrich_from_local_db(airport_t *ap)
{
    airport_t local = { 0 };
    if (!airports_lookup(ap->icao, &local)) {
        return;
    }
    if (local.iata[0]) {
        strlcpy(ap->iata, local.iata, sizeof(ap->iata));
    }
    if (local.city[0]) {
        strlcpy(ap->city, local.city, sizeof(ap->city));
    }
    if (local.country[0]) {
        strlcpy(ap->country, local.country, sizeof(ap->country));
    }
    if (local.name[0]) {
        strlcpy(ap->name, local.name, sizeof(ap->name));
    }
    if (local.lat != 0 || local.lon != 0) {
        ap->lat = local.lat;
        ap->lon = local.lon;
    }
}

static void parse_airport(const cJSON *j, airport_t *ap)
{
    const cJSON *f;
    if ((f = cJSON_GetObjectItem(j, "icao_code")) && cJSON_IsString(f)) {
        strlcpy(ap->icao, f->valuestring, sizeof(ap->icao));
    }
    if ((f = cJSON_GetObjectItem(j, "iata_code")) && cJSON_IsString(f)) {
        strlcpy(ap->iata, f->valuestring, sizeof(ap->iata));
    }
    if ((f = cJSON_GetObjectItem(j, "name")) && cJSON_IsString(f)) {
        strlcpy(ap->name, f->valuestring, sizeof(ap->name));
    }
    if ((f = cJSON_GetObjectItem(j, "municipality")) && cJSON_IsString(f)) {
        strlcpy(ap->city, f->valuestring, sizeof(ap->city));
    } else if ((f = cJSON_GetObjectItem(j, "name")) && cJSON_IsString(f)) {
        strlcpy(ap->city, f->valuestring, sizeof(ap->city));
    }
    if ((f = cJSON_GetObjectItem(j, "country_iso_name")) && cJSON_IsString(f)) {
        strlcpy(ap->country, f->valuestring, sizeof(ap->country));
    }
    if ((f = cJSON_GetObjectItem(j, "latitude")) && cJSON_IsNumber(f)) {
        ap->lat = f->valuedouble;
    }
    if ((f = cJSON_GetObjectItem(j, "longitude")) && cJSON_IsNumber(f)) {
        ap->lon = f->valuedouble;
    }
    enrich_from_local_db(ap);
    fix_city(ap);
}

/* Fallback: hexdb.io route (ICAO pair) + airport lookups. No airline info,
 * but the UI derives the logo from the callsign prefix anyway. */
static bool hexdb_airport(char *buf, const char *icao, airport_t *ap)
{
    /* Local database first; the HTTP lookup is only a fallback */
    if (airports_lookup(icao, ap) && ap->iata[0] && (ap->lat != 0 || ap->lon != 0)) {
        fix_city(ap);
        return true;
    }

    char url[80];
    snprintf(url, sizeof(url), "https://hexdb.io/api/v1/airport/icao/%s", icao);
    if (http_get_to_buffer(url, buf, 4096, NULL) != ESP_OK) {
        return false;
    }
    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return false;
    }
    const cJSON *f;
    strlcpy(ap->icao, icao, sizeof(ap->icao));
    if ((f = cJSON_GetObjectItem(root, "country_code")) && cJSON_IsString(f)) {
        strlcpy(ap->country, f->valuestring, sizeof(ap->country));
    }
    if ((f = cJSON_GetObjectItem(root, "iata")) && cJSON_IsString(f)) {
        strlcpy(ap->iata, f->valuestring, sizeof(ap->iata));
    }
    if ((f = cJSON_GetObjectItem(root, "airport")) && cJSON_IsString(f)) {
        strlcpy(ap->name, f->valuestring, sizeof(ap->name));
        /* hexdb has no municipality field; fix_city() trims the airport-name
         * suffix ("Debrecen International Airport" -> "Debrecen"). */
        strlcpy(ap->city, f->valuestring, sizeof(ap->city));
    }
    bool ok = false;
    if ((f = cJSON_GetObjectItem(root, "latitude")) && cJSON_IsNumber(f)) {
        ap->lat = f->valuedouble;
        ok = true;
    }
    if ((f = cJSON_GetObjectItem(root, "longitude")) && cJSON_IsNumber(f)) {
        ap->lon = f->valuedouble;
    } else {
        ok = false;
    }
    cJSON_Delete(root);
    enrich_from_local_db(ap);
    fix_city(ap);
    return ok && ap->iata[0] != '\0';
}

/* adsb.lol routeset: crowd-sourced callsign->route with server-side
 * position plausibility. Better coverage of US carriers than adsbdb. */
static void lol_routeset(char *buf, route_info_t *slot,
                         double ac_lat, double ac_lon, bool has_pos)
{
    char body[160];
    if (has_pos) {
        snprintf(body, sizeof(body),
                 "{\"planes\":[{\"callsign\":\"%s\",\"lat\":%.4f,\"lng\":%.4f}]}",
                 slot->callsign, ac_lat, ac_lon);
    } else {
        snprintf(body, sizeof(body),
                 "{\"planes\":[{\"callsign\":\"%s\",\"lat\":0,\"lng\":0}]}",
                 slot->callsign);
    }
    if (http_post_to_buffer("https://api.adsb.lol/api/0/routeset", body,
                            buf, 8192) != ESP_OK) {
        return;
    }
    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return;
    }
    const cJSON *first = cJSON_IsArray(root) ? cJSON_GetArrayItem(root, 0) : NULL;
    const cJSON *aps = first ? cJSON_GetObjectItem(first, "_airports") : NULL;
    const cJSON *pl = first ? cJSON_GetObjectItem(first, "plausible") : NULL;
    bool plausible = pl == NULL || cJSON_IsTrue(pl) ||
                     (cJSON_IsNumber(pl) && pl->valuedouble != 0);
    char orig[8] = "", dest[8] = "";
    if (plausible && cJSON_IsArray(aps) && cJSON_GetArraySize(aps) >= 2) {
        const cJSON *a0 = cJSON_GetArrayItem(aps, 0);
        const cJSON *a1 = cJSON_GetArrayItem(aps, cJSON_GetArraySize(aps) - 1);
        const cJSON *f;
        if (a0 && (f = cJSON_GetObjectItem(a0, "icao")) && cJSON_IsString(f)) {
            strlcpy(orig, f->valuestring, sizeof(orig));
        }
        if (a1 && (f = cJSON_GetObjectItem(a1, "icao")) && cJSON_IsString(f)) {
            strlcpy(dest, f->valuestring, sizeof(dest));
        }
    }
    cJSON_Delete(root);

    if (strlen(orig) != 4 || strlen(dest) != 4 || strcmp(orig, dest) == 0) {
        return;
    }
    if (hexdb_airport(buf, orig, &slot->origin) &&
        hexdb_airport(buf, dest, &slot->destination)) {
        slot->valid = true;
        ESP_LOGI(TAG, "%s: route from adsb.lol routeset", slot->callsign);
    } else {
        memset(&slot->origin, 0, sizeof(slot->origin));
        memset(&slot->destination, 0, sizeof(slot->destination));
    }
}

static void hexdb_fallback(char *buf, route_info_t *slot)
{
    char url[80];
    snprintf(url, sizeof(url), "https://hexdb.io/api/v1/route/icao/%s", slot->callsign);
    if (http_get_to_buffer(url, buf, 4096, NULL) != ESP_OK) {
        return;
    }
    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return;
    }
    char pair[16] = "";
    const cJSON *jr = cJSON_GetObjectItem(root, "route");
    if (cJSON_IsString(jr)) {
        strlcpy(pair, jr->valuestring, sizeof(pair));
    }
    cJSON_Delete(root);

    char *dash = strchr(pair, '-');
    if (dash == NULL) {
        return;
    }
    *dash = '\0';
    const char *orig = pair, *dest = dash + 1;
    if (strlen(orig) != 4 || strlen(dest) != 4 || strcmp(orig, dest) == 0) {
        return;
    }
    if (hexdb_airport(buf, orig, &slot->origin) &&
        hexdb_airport(buf, dest, &slot->destination)) {
        slot->valid = true;
    }
}

static bool route_fits_position(const route_info_t *rt,
                                double ac_lat, double ac_lon, bool has_pos)
{
    if (!rt->valid || !has_pos) {
        return rt->valid;
    }
    return geo_route_plausible(rt->origin.lat, rt->origin.lon,
                               rt->destination.lat, rt->destination.lon,
                               ac_lat, ac_lon);
}

const route_info_t *routes_fetch(const char *callsign,
                                 double ac_lat, double ac_lon, bool has_pos)
{
    const route_info_t *cached = routes_get_cached(callsign);
    if (cached != NULL) {
        return cached;
    }

    char *buf = malloc(8192);
    if (buf == NULL) {
        return NULL;
    }

    char url[96];
    snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", callsign);
    esp_err_t err = http_get_to_buffer(url, buf, 8192, NULL);
    /* 200 (parsed below) and 404 are authoritative answers; timeouts,
     * rate limits and server errors are not - those must retry soon
     * instead of poisoning the negative cache for half an hour. */
    bool authoritative = (err == ESP_OK) || (err == ESP_ERR_HTTP_BASE + 404);

    route_info_t *slot = cache_slot(callsign);   /* negative by default */
    if (slot == NULL) {
        free(buf);
        return NULL;
    }

    if (err == ESP_OK) {
        cJSON *root = cJSON_Parse(buf);
        if (root != NULL) {
            const cJSON *resp = cJSON_GetObjectItem(root, "response");
            const cJSON *fr = resp ? cJSON_GetObjectItem(resp, "flightroute") : NULL;
            if (fr != NULL) {
                const cJSON *airline = cJSON_GetObjectItem(fr, "airline");
                const cJSON *f;
                if (airline != NULL) {
                    if ((f = cJSON_GetObjectItem(airline, "name")) && cJSON_IsString(f)) {
                        strlcpy(slot->airline_name, f->valuestring, sizeof(slot->airline_name));
                    }
                    if ((f = cJSON_GetObjectItem(airline, "icao")) && cJSON_IsString(f)) {
                        strlcpy(slot->airline_icao, f->valuestring, sizeof(slot->airline_icao));
                    }
                }
                const cJSON *orig = cJSON_GetObjectItem(fr, "origin");
                const cJSON *dest = cJSON_GetObjectItem(fr, "destination");
                if (orig != NULL && dest != NULL) {
                    parse_airport(orig, &slot->origin);
                    parse_airport(dest, &slot->destination);
                    slot->valid = slot->origin.icao[0] != '\0' && slot->destination.icao[0] != '\0';
                }
            }
            cJSON_Delete(root);
        }
    }

    if (slot->valid && !route_fits_position(slot, ac_lat, ac_lon, has_pos)) {
        ESP_LOGW(TAG, "%s: adsbdb route %s->%s doesn't fit position, trying hexdb",
                 callsign, slot->origin.icao, slot->destination.icao);
        slot->valid = false;
        memset(&slot->origin, 0, sizeof(slot->origin));
        memset(&slot->destination, 0, sizeof(slot->destination));
    }
    if (!slot->valid) {
        lol_routeset(buf, slot, ac_lat, ac_lon, has_pos);
        if (slot->valid && !route_fits_position(slot, ac_lat, ac_lon, has_pos)) {
            ESP_LOGW(TAG, "%s: routeset %s->%s doesn't fit position",
                     callsign, slot->origin.icao, slot->destination.icao);
            slot->valid = false;
        }
    }
    if (!slot->valid) {
        hexdb_fallback(buf, slot);
        if (slot->valid && !route_fits_position(slot, ac_lat, ac_lon, has_pos)) {
            ESP_LOGW(TAG, "%s: hexdb route %s->%s doesn't fit position either",
                     callsign, slot->origin.icao, slot->destination.icao);
            slot->valid = false;
        }
    }

    free(buf);
    if (slot->valid) {
        slot->origin.tz_known = tz_offset_for(slot->origin.icao,
                                              slot->origin.lat, slot->origin.lon,
                                              &slot->origin.tz_offset_s);
        slot->destination.tz_known = tz_offset_for(slot->destination.icao,
                                                   slot->destination.lat, slot->destination.lon,
                                                   &slot->destination.tz_offset_s);
    }
    if (!slot->valid && !authoritative) {
        slot->fetched_ms -= NEGATIVE_TTL_MS - TRANSIENT_RETRY_MS;
        ESP_LOGW(TAG, "%s: lookup failed (0x%x), retry in %ds", callsign,
                 (unsigned)err, (int)(TRANSIENT_RETRY_MS / 1000));
    } else if (!slot->valid) {
        ESP_LOGI(TAG, "no route for %s (negative-cached)", callsign);
    } else {
        ESP_LOGI(TAG, "%s: %s -> %s (%s)", callsign,
                 slot->origin.icao, slot->destination.icao, slot->airline_name);
    }
    return slot;
}

void routes_inject(const char *callsign, const char *orig_icao, const char *dest_icao)
{
    if (callsign == NULL || callsign[0] == '\0' || !cache_ready() ||
        orig_icao == NULL || dest_icao == NULL ||
        strlen(orig_icao) != 4 || strlen(dest_icao) != 4 ||
        strcmp(orig_icao, dest_icao) == 0) {
        return;
    }
    route_info_t *slot = NULL;
    for (int i = 0; i < s_used; i++) {
        if (strcmp(s_cache[i].callsign, callsign) == 0) {
            slot = &s_cache[i];
            break;
        }
    }
    if (slot != NULL && slot->valid) {
        return;     /* an already validated route wins */
    }
    if (slot == NULL) {
        slot = cache_slot(callsign);
        if (slot == NULL) {
            return;
        }
    }
    char *buf = malloc(4096);
    if (buf == NULL) {
        return;
    }
    memset(&slot->origin, 0, sizeof(slot->origin));
    memset(&slot->destination, 0, sizeof(slot->destination));
    slot->valid = hexdb_airport(buf, orig_icao, &slot->origin) &&
                  hexdb_airport(buf, dest_icao, &slot->destination);
    slot->fetched_ms = esp_timer_get_time() / 1000;
    free(buf);
    if (slot->valid) {
        slot->origin.tz_known = tz_offset_for(slot->origin.icao,
                                              slot->origin.lat, slot->origin.lon,
                                              &slot->origin.tz_offset_s);
        slot->destination.tz_known = tz_offset_for(slot->destination.icao,
                                                   slot->destination.lat,
                                                   slot->destination.lon,
                                                   &slot->destination.tz_offset_s);
        ESP_LOGI(TAG, "%s: %s -> %s (injected)", callsign, orig_icao, dest_icao);
    }
}
