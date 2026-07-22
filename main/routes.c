#include "routes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "geo_math.h"
#include "http_util.h"
#include "tz.h"

static const char *TAG = "routes";

#define ROUTE_CACHE_SIZE 48

static route_info_t s_cache[ROUTE_CACHE_SIZE];
static int s_used;
static int s_next_evict;

const route_info_t *routes_get_cached(const char *callsign)
{
    if (callsign == NULL || callsign[0] == '\0') {
        return NULL;
    }
    for (int i = 0; i < s_used; i++) {
        if (strcmp(s_cache[i].callsign, callsign) == 0) {
            return &s_cache[i];
        }
    }
    return NULL;
}

static route_info_t *cache_slot(const char *callsign)
{
    route_info_t *slot;
    if (s_used < ROUTE_CACHE_SIZE) {
        slot = &s_cache[s_used++];
    } else {
        slot = &s_cache[s_next_evict];
        s_next_evict = (s_next_evict + 1) % ROUTE_CACHE_SIZE;
    }
    memset(slot, 0, sizeof(*slot));
    strlcpy(slot->callsign, callsign, sizeof(slot->callsign));
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
    };
    for (size_t i = 0; i < sizeof(overrides) / sizeof(overrides[0]); i++) {
        if (strcmp(ap->icao, overrides[i].icao) == 0) {
            strlcpy(ap->city, overrides[i].city, sizeof(ap->city));
            return;
        }
    }
    /* If the "city" is actually an airport name, trim the suffix - but never
     * down to nothing. */
    static const char *suffixes[] = {
        " International Airport", " Intl Airport", " Airport",
        " Airfield", " Air Base", " Airbase",
    };
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        char *at = strstr(ap->city, suffixes[i]);
        if (at != NULL && at != ap->city) {
            *at = '\0';
            break;
        }
    }
    /* "Poznań–Ławica Henryk Wieniawski" -> "Poznań" (en-dash separates the
     * city from the airport's proper name) */
    char *dash = strstr(ap->city, "\xE2\x80\x93");
    if (dash != NULL && dash != ap->city) {
        *dash = '\0';
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
    fix_city(ap);
}

/* Fallback: hexdb.io route (ICAO pair) + airport lookups. No airline info,
 * but the UI derives the logo from the callsign prefix anyway. */
static bool hexdb_airport(char *buf, const char *icao, airport_t *ap)
{
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
    fix_city(ap);
    return ok && ap->iata[0] != '\0';
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

    route_info_t *slot = cache_slot(callsign);   /* negative by default */

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
    if (!slot->valid) {
        ESP_LOGI(TAG, "no route for %s (negative-cached)", callsign);
    } else {
        ESP_LOGI(TAG, "%s: %s -> %s (%s)", callsign,
                 slot->origin.icao, slot->destination.icao, slot->airline_name);
    }
    return slot;
}
