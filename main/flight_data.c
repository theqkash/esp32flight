#include "flight_data.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "http_util.h"
#include "geo_math.h"
#include "settings.h"

static const char *TAG = "flights";

#define FETCH_BUF_SIZE (256 * 1024)

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

flight_class_t flight_class(const aircraft_t *ac)
{
    if (ac->military) {
        return FCLS_MIL;
    }
    const char *c = ac->category;
    if (c[0] == 'A') {
        if (c[1] == '1') {
            return FCLS_SMALL;
        }
        if (c[1] == '7') {
            return FCLS_HELI;
        }
        if (c[1] >= '2' && c[1] <= '6') {
            return FCLS_LINER;
        }
    }
    if (c[0] == 'B' && c[1] == '4') {
        return FCLS_SMALL;
    }
    if (c[0] == 'B' || c[0] == 'C') {
        return FCLS_OTHER;
    }
    /* no category broadcast: judge by the callsign shape */
    return flight_is_airline(ac) ? FCLS_LINER : FCLS_OTHER;
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
    if (j == NULL) {
        /* Old dump1090-mutability schema (FR24/PiAware feeder bundles):
         * "altitude"/"speed"/"vert_rate" instead of alt_baro/gs/baro_rate.
         * Issue #17: a local FR24 receiver showed every flight as 0 ft
         * 0 kt because only the new keys were read. */
        j = cJSON_GetObjectItem(jac, "altitude");
    }
    if (cJSON_IsNumber(j)) {
        ac->alt_baro_ft = (int)j->valuedouble;
    } else if (cJSON_IsString(j) && strcmp(j->valuestring, "ground") == 0) {
        ac->on_ground = true;
    }

    if (((j = cJSON_GetObjectItem(jac, "gs")) ||
         (j = cJSON_GetObjectItem(jac, "speed"))) && cJSON_IsNumber(j)) {
        ac->gs_kts = (float)j->valuedouble;
    }
    if ((j = cJSON_GetObjectItem(jac, "track")) && cJSON_IsNumber(j)) {
        ac->track_deg = (float)j->valuedouble;
    }
    if (((j = cJSON_GetObjectItem(jac, "baro_rate")) ||
         (j = cJSON_GetObjectItem(jac, "vert_rate"))) && cJSON_IsNumber(j)) {
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

static esp_err_t parse_point_response(const char *json, double lat, double lon,
                                       aircraft_list_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *jlist = cJSON_GetObjectItem(root, "ac");
    if (!cJSON_IsArray(jlist)) {
        jlist = cJSON_GetObjectItem(root, "aircraft");   /* dump1090/readsb */
    }
    if (!cJSON_IsArray(jlist)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Busy areas return far more targets than we can keep. Keep the
     * NEAREST ones, not the first ones in response order, and do not
     * waste slots on traffic the active filters would drop anyway. */
    const settings_t *st = settings_get();
    out->count = 0;
    int total = 0;
    const cJSON *jac;
    cJSON_ArrayForEach(jac, jlist) {
        aircraft_t tmp;
        parse_aircraft(jac, &tmp);
        /* Skip targets without position: nothing to show or compute. */
        if (!tmp.has_pos) {
            continue;
        }
        if (tmp.dist_nm < 0) {
            tmp.dist_nm = (float)(geo_haversine_km(lat, lon, tmp.lat, tmp.lon) / 1.852);
            tmp.dir_deg = (float)geo_bearing_deg(lat, lon, tmp.lat, tmp.lon);
        }
        total++;
        if (st->hide_ground && tmp.on_ground) {
            continue;
        }
        if (!(st->show_classes & (1u << flight_class(&tmp)))) {
            continue;
        }
        if (!tmp.on_ground &&
            ((st->alt_min_ft > 0 && tmp.alt_baro_ft < st->alt_min_ft) ||
             (st->alt_max_ft > 0 && tmp.alt_baro_ft > st->alt_max_ft))) {
            continue;
        }
        if (out->count < MAX_AIRCRAFT) {
            out->ac[out->count++] = tmp;
        } else {
            int far = 0;
            for (int i = 1; i < out->count; i++) {
                if (out->ac[i].dist_nm > out->ac[far].dist_nm) {
                    far = i;
                }
            }
            if (tmp.dist_nm < out->ac[far].dist_nm) {
                out->ac[far] = tmp;
            }
        }
    }
    cJSON_Delete(root);

    if (total > MAX_AIRCRAFT) {
        ESP_LOGI(TAG, "%d aircraft in the area, keeping the nearest %d", total, out->count);
    }
    qsort(out->ac, out->count, sizeof(aircraft_t), cmp_by_dist);
    out->fetched_at_ms = esp_timer_get_time() / 1000;
    return ESP_OK;
}

/* Fill in distance/bearing from home for entries the source did not
 * provide them for (local receivers), then drop out-of-range targets. */
static void localize_and_filter(aircraft_list_t *out, double lat, double lon, int radius_nm)
{
    int w = 0;
    for (int i = 0; i < out->count; i++) {
        aircraft_t *ac = &out->ac[i];
        if (ac->dist_nm < 0 && ac->has_pos) {
            ac->dist_nm = (float)(geo_haversine_km(lat, lon, ac->lat, ac->lon) / 1.852);
            ac->dir_deg = (float)geo_bearing_deg(lat, lon, ac->lat, ac->lon);
        }
        if (ac->dist_nm >= 0 && ac->dist_nm <= radius_nm) {
            out->ac[w++] = *ac;
        }
    }
    out->count = w;
}

/* Add aircraft from `extra` that `out` does not have yet (by hex). The
 * records already in `out` stay authoritative. Returns how many joined. */
static int union_into(aircraft_list_t *out, const aircraft_list_t *extra)
{
    int added = 0;
    for (int k = 0; k < extra->count && out->count < MAX_AIRCRAFT; k++) {
        bool dup = false;
        for (int m = 0; m < out->count; m++) {
            if (strcmp(out->ac[m].hex, extra->ac[k].hex) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            out->ac[out->count++] = extra->ac[k];
            added++;
        }
    }
    return added;
}

/* Park a source that refused us at the HTTP level instead of asking it
 * again every cycle. airplanes.live closed its public API in Sept 2026
 * (403 "contact us" for non-feeders), so a 401/403 parks for 6 h; a 429
 * rate limit for 5 min. Transport errors are not parked: weak Wi-Fi
 * recovers on its own. */
#define N_SOURCES 3
static int64_t s_parked_until_ms[N_SOURCES];

static void park_if_refused(int src, esp_err_t err, int64_t now_ms)
{
    int status = err - ESP_ERR_HTTP_BASE;
    int64_t hold_ms;
    if (status == 401 || status == 403) {
        hold_ms = 6LL * 3600 * 1000;
    } else if (status == 429) {
        hold_ms = 5LL * 60 * 1000;
    } else {
        return;
    }
    s_parked_until_ms[src] = now_ms + hold_ms;
    ESP_LOGW(TAG, "source %d answered HTTP %d, parked for %d min",
             src, status, (int)(hold_ms / 60000));
}

/* Internet aggregators, in preference order, with the union pass. */
static esp_err_t fetch_internet(double lat, double lon, int radius_nm,
                                aircraft_list_t *out, char *buf)
{
    /* adsb.fi: v3 is the current endpoint (v2 lat/lon is deprecated and
     * answers with the old "aircraft" key); it caps the radius at 250 nm,
     * the same as our settings. Personal, non-commercial use, 1 req/s.
     * airplanes.live stays last and is parked after its first 403. */
    static const char *src_fmt[N_SOURCES] = {
        "https://api.adsb.lol/v2/point/%.4f/%.4f/%d",
        "https://opendata.adsb.fi/api/v3/lat/%.4f/lon/%.4f/dist/%d",
        "https://api.airplanes.live/v2/point/%.4f/%.4f/%d",
    };
    /* The very first fetch skips the union request: it costs a full TLS
     * handshake and the boot screen is waiting. MLAT-only targets join
     * one cycle later and stay (the union runs every cycle after). */
    static bool s_first_fetch = true;
    int64_t now_ms = esp_timer_get_time() / 1000;
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < N_SOURCES; i++) {
        if (now_ms < s_parked_until_ms[i]) {
            continue;
        }
        char url[128];
        snprintf(url, sizeof(url), src_fmt[i], lat, lon, radius_nm);
        /* keep-alive slot per aggregator: steady state re-uses one TLS
         * connection per host instead of a fresh handshake every cycle */
        err = http_get_keepalive(i, url, buf, FETCH_BUF_SIZE, NULL);
        park_if_refused(i, err, now_ms);
        if (err == ESP_OK) {
            err = parse_point_response(buf, lat, lon, out);
        }
        if (err == ESP_OK) {
            /* Union with every other aggregator: each network runs its
             * own MLAT feeders, so a target one cannot multilaterate is
             * often present elsewhere. */
            if (!s_first_fetch) {
                aircraft_list_t *extra = heap_caps_malloc(sizeof(*extra), MALLOC_CAP_SPIRAM);
                for (int j = 0; extra != NULL && j < N_SOURCES; j++) {
                    if (j == i || now_ms < s_parked_until_ms[j]) {
                        continue;
                    }
                    char url2[128];
                    snprintf(url2, sizeof(url2), src_fmt[j], lat, lon, radius_nm);
                    esp_err_t jerr = http_get_keepalive(j, url2, buf, FETCH_BUF_SIZE, NULL);
                    park_if_refused(j, jerr, now_ms);
                    if (jerr != ESP_OK || parse_point_response(buf, lat, lon, extra) != ESP_OK) {
                        continue;
                    }
                    int added = union_into(out, extra);
                    if (added > 0) {
                        ESP_LOGI(TAG, "merged %d extra aircraft from source %d", added, j);
                    }
                }
                free(extra);
            }
            if (i > 0) {
                ESP_LOGI(TAG, "using fallback source %d", i);
            }
            s_first_fetch = false;
            return ESP_OK;
        }
        ESP_LOGW(TAG, "source %d failed (%s)", i, esp_err_to_name(err));
    }
    return err;
}

/* LAN receiver URL with a bare host name ("http://adsbpi:8080/...") that
 * the router's DNS does not know: retry it as host.local over mDNS (#35
 * report). Remembered once it works. */
static bool local_url_mdns(const char *url, char *out, size_t n)
{
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    size_t hl = strcspn(h, ":/");
    if (hl == 0 || hl > 40) {
        return false;
    }
    bool dot = false, alpha = false;
    for (size_t i = 0; i < hl; i++) {
        if (h[i] == '.') dot = true;
        if ((h[i] >= 'a' && h[i] <= 'z') || (h[i] >= 'A' && h[i] <= 'Z')) alpha = true;
    }
    if (dot || !alpha || strncmp(h, "localhost", hl) == 0) {
        return false;
    }
    snprintf(out, n, "%.*s.local%s", (int)(h - url + hl), url, h + hl);
    return true;
}

static esp_err_t fetch_local(const char *local, double lat, double lon,
                             aircraft_list_t *out, char *buf)
{
    static bool s_use_mdns;
    char alt[128];
    bool have_alt = local_url_mdns(local, alt, sizeof(alt));
    const char *url = (s_use_mdns && have_alt) ? alt : local;
    esp_err_t lerr = http_get_to_buffer(url, buf, FETCH_BUF_SIZE, NULL);
    if (lerr != ESP_OK && have_alt && !s_use_mdns &&
        lerr < ESP_ERR_HTTP_BASE + 100) {
        lerr = http_get_to_buffer(alt, buf, FETCH_BUF_SIZE, NULL);
        if (lerr == ESP_OK) {
            s_use_mdns = true;
            ESP_LOGI(TAG, "local receiver reached as %s", alt);
        }
    }
    if (lerr == ESP_OK) {
        lerr = parse_point_response(buf, lat, lon, out);
    }
    return lerr;
}

esp_err_t flight_fetch_nearby(double lat, double lon, int radius_nm, aircraft_list_t *out)
{
    /* Persistent singleton: allocated on the first (early) fetch and kept.
     * Allocating 256 KB per cycle used to fail outright once the big tile
     * framebuffers pinned the 7B's PSRAM - no buffer, no aircraft. One
     * flight_task, so no concurrency concerns. */
    static char *buf;
    if (buf == NULL) {
        buf = heap_caps_malloc(FETCH_BUF_SIZE, MALLOC_CAP_SPIRAM);
    }
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* LAN receiver first, when configured and enabled (the switch lets
     * owners flip to the internet view without wiping the URL, #17).
     * With local_merge on, the internet aggregators fill in what the
     * antenna cannot hear; the receiver's own records win on overlap. */
    const settings_t *cfg = settings_get();
    if (cfg->local_adsb[0] != '\0' && cfg->local_adsb_use) {
        esp_err_t lerr = fetch_local(cfg->local_adsb, lat, lon, out, buf);
        if (lerr == ESP_OK) {
            if (cfg->local_merge) {
                aircraft_list_t *net = heap_caps_malloc(sizeof(*net), MALLOC_CAP_SPIRAM);
                if (net != NULL && fetch_internet(lat, lon, radius_nm, net, buf) == ESP_OK) {
                    int added = union_into(out, net);
                    if (added > 0) {
                        ESP_LOGI(TAG, "local + internet: %d aircraft from the internet", added);
                    }
                }
                free(net);
            }
            localize_and_filter(out, lat, lon, radius_nm);
            qsort(out->ac, out->count, sizeof(aircraft_t), cmp_by_dist);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "local receiver failed (%s), falling back to internet",
                 esp_err_to_name(lerr));
    }

    esp_err_t err = fetch_internet(lat, lon, radius_nm, out, buf);
    if (err == ESP_OK) {
        localize_and_filter(out, lat, lon, radius_nm);
        qsort(out->ac, out->count, sizeof(aircraft_t), cmp_by_dist);
    }
    return err;
}

static bool track_from_json(const char *json, double lat, double lon, aircraft_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return false;
    }
    bool found = false;
    const cJSON *jlist = cJSON_GetObjectItem(root, "ac");
    if (!cJSON_IsArray(jlist)) {
        jlist = cJSON_GetObjectItem(root, "aircraft");
    }
    const cJSON *jac;
    cJSON_ArrayForEach(jac, jlist) {
        aircraft_t tmp;
        parse_aircraft(jac, &tmp);
        if (!tmp.has_pos) {
            continue;
        }
        tmp.dist_nm = (float)(geo_haversine_km(lat, lon, tmp.lat, tmp.lon) / 1.852);
        tmp.dir_deg = (float)geo_bearing_deg(lat, lon, tmp.lat, tmp.lon);
        *out = tmp;
        found = true;
        break;
    }
    cJSON_Delete(root);
    return found;
}

esp_err_t flight_fetch_track(const char *id, double lat, double lon, aircraft_t *out)
{
    /* normalize: upper case, no spaces; keep the dash of registrations */
    char q[12];
    size_t n = 0;
    for (const char *p = id; *p && n < sizeof(q) - 1; p++) {
        if (*p == ' ') {
            continue;
        }
        q[n++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
    }
    q[n] = '\0';
    if (n < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    bool is_hex = n == 6;
    for (size_t i = 0; i < n && is_hex; i++) {
        is_hex = (q[i] >= '0' && q[i] <= '9') || (q[i] >= 'A' && q[i] <= 'F');
    }
    /* Try in order of likelihood: a dash means a registration; otherwise
     * a callsign first (LOT3AB, RYR4238), then registration (N12345 has
     * no dash), then hex. ABC123 can be both a callsign and a hex. */
    const char *kinds[3];
    int nk = 0;
    if (strchr(q, '-') != NULL) {
        kinds[nk++] = "reg";
    } else {
        kinds[nk++] = "callsign";
        kinds[nk++] = "reg";
        if (is_hex) {
            kinds[nk++] = "hex";
        }
    }
    static char *buf;
    if (buf == NULL) {
        buf = heap_caps_malloc(32 * 1024, MALLOC_CAP_SPIRAM);
    }
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (int k = 0; k < nk; k++) {
        char url[96];
        snprintf(url, sizeof(url), "https://api.adsb.lol/v2/%s/%s", kinds[k], q);
        esp_err_t err = http_get_to_buffer_t(url, buf, 32 * 1024, NULL, 6000);
        if (err != ESP_OK) {
            /* adsb.fi names the same lookups slightly differently */
            snprintf(url, sizeof(url), "https://opendata.adsb.fi/api/v2/%s/%s",
                     strcmp(kinds[k], "reg") == 0 ? "registration" : kinds[k], q);
            err = http_get_to_buffer_t(url, buf, 32 * 1024, NULL, 6000);
        }
        if (err == ESP_OK && track_from_json(buf, lat, lon, out)) {
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

flight_sprite_t flight_sprite(const aircraft_t *ac)
{
    switch (flight_class(ac)) {
    case FCLS_SMALL: return FSPR_SMALL;
    case FCLS_HELI:  return FSPR_HELI;
    case FCLS_MIL:   return FSPR_MIL;
    case FCLS_OTHER:
        if (ac->category[0] == 'B') {
            if (ac->category[1] == '2') {
                return FSPR_BALLOON;   /* lighter-than-air */
            }
            if (ac->category[1] == '6') {
                return FSPR_DRONE;     /* UAV */
            }
        }
        return FSPR_GLIDER;
    default: return FSPR_PLANE;
    }
}
