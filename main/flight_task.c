#include "flight_task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "sdkconfig.h"

#include "flight_data.h"
#include "geolocate.h"
#include "airlines.h"
#include "lvgl_port.h"
#include "routes.h"
#include "settings.h"
#include "geo_math.h"
#include "lang.h"
#include "tz.h"
#include "ui.h"
#include "weather.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "flight_task";

/* New adsbdb lookups per poll cycle - keep the free service happy. */
#define MAX_ROUTE_LOOKUPS_PER_CYCLE 4
/* Only resolve routes for the closest N aircraft. */
#define ROUTE_LOOKUP_TOP_N 10

static void set_status(const char *text)
{
    if (lvgl_port_lock(1000)) {
        ui_set_status(text);
        lvgl_port_unlock();
    }
}

/* Session stats (also exposed on the web panel) */
static struct {
    uint32_t hexes[1024];
    int      unique;
    int      max_alt_ft;
    float    max_gs_kt;
    float    max_dist_km;
    char     max_dist_cs[CALLSIGN_LEN];
} s_stats;

static void stats_update(const aircraft_list_t *list)
{
    for (int i = 0; i < list->count; i++) {
        const aircraft_t *ac = &list->ac[i];
        uint32_t h = (uint32_t)strtoul(ac->hex[0] == '~' ? ac->hex + 1 : ac->hex, NULL, 16);
        bool known = false;
        for (int k = 0; k < s_stats.unique; k++) {
            if (s_stats.hexes[k] == h) {
                known = true;
                break;
            }
        }
        if (!known && s_stats.unique < 1024) {
            s_stats.hexes[s_stats.unique++] = h;
        }
        if (ac->alt_baro_ft > s_stats.max_alt_ft) {
            s_stats.max_alt_ft = ac->alt_baro_ft;
        }
        if (ac->gs_kts > s_stats.max_gs_kt) {
            s_stats.max_gs_kt = ac->gs_kts;
        }
        float dkm = ac->dist_nm >= 0 ? ac->dist_nm * 1.852f : 0;
        if (dkm > s_stats.max_dist_km) {
            s_stats.max_dist_km = dkm;
            strlcpy(s_stats.max_dist_cs, ac->callsign[0] ? ac->callsign : ac->hex,
                    sizeof(s_stats.max_dist_cs));
        }
    }
}

/* NULL if no emergency squawk on the list */
static const aircraft_t *find_emergency(const aircraft_list_t *list)
{
    static const char *codes[] = { "7700", "7500", "7600" };
    for (size_t c = 0; c < 3; c++) {
        for (int i = 0; i < list->count; i++) {
            if (strcmp(list->ac[i].squawk, codes[c]) == 0) {
                return &list->ac[i];
            }
        }
    }
    return NULL;
}

static void publish_web_state(const aircraft_list_t *list, const weather_t *wx,
                              double lat, double lon, const char *city, int radius_nm)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "city", city);
    cJSON_AddNumberToObject(root, "lat", lat);
    cJSON_AddNumberToObject(root, "lon", lon);
    cJSON_AddNumberToObject(root, "radius_km", (int)(radius_nm * 1.852));
    if (wx->valid) {
        cJSON *jw = cJSON_AddObjectToObject(root, "weather");
        cJSON_AddNumberToObject(jw, "temp_c", (int)(wx->temp_c + 0.5f));
        cJSON_AddStringToObject(jw, "desc", weather_code_str(wx->code));
        cJSON_AddNumberToObject(jw, "wind_kmh", (int)wx->wind_kmh);
    }
    /* Network info for the panel */
    cJSON *jn = cJSON_AddObjectToObject(root, "net");
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));
        cJSON_AddStringToObject(jn, "ip", ip);
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.gw));
        cJSON_AddStringToObject(jn, "gateway", ip);
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddStringToObject(jn, "ssid", (const char *)ap.ssid);
        cJSON_AddNumberToObject(jn, "rssi", ap.rssi);
        cJSON_AddNumberToObject(jn, "channel", ap.primary);
    }
    cJSON_AddStringToObject(jn, "mdns", "esp32flight.local");

    cJSON *js = cJSON_AddObjectToObject(root, "stats");
    cJSON_AddNumberToObject(js, "unique_aircraft", s_stats.unique);
    cJSON_AddNumberToObject(js, "max_alt_ft", s_stats.max_alt_ft);
    cJSON_AddNumberToObject(js, "max_gs_kt", (int)s_stats.max_gs_kt);
    cJSON_AddNumberToObject(js, "max_dist_km", (int)s_stats.max_dist_km);
    cJSON_AddStringToObject(js, "max_dist_callsign", s_stats.max_dist_cs);
    cJSON_AddNumberToObject(js, "uptime_min", (int)(esp_timer_get_time() / 60000000LL));

    cJSON *arr = cJSON_AddArrayToObject(root, "flights");
    for (int i = 0; i < list->count; i++) {
        const aircraft_t *ac = &list->ac[i];
        cJSON *jf = cJSON_CreateObject();
        cJSON_AddStringToObject(jf, "callsign", ac->callsign[0] ? ac->callsign : ac->hex);
        cJSON_AddStringToObject(jf, "hex", ac->hex);
        cJSON_AddStringToObject(jf, "type", ac->type_desc[0] ? ac->type_desc : ac->type_icao);
        cJSON_AddNumberToObject(jf, "alt_ft", ac->on_ground ? 0 : ac->alt_baro_ft);
        cJSON_AddNumberToObject(jf, "gs_kt", (int)ac->gs_kts);
        cJSON_AddNumberToObject(jf, "dist_km", (double)(int)(ac->dist_nm * 1.852f * 10) / 10);
        cJSON_AddStringToObject(jf, "squawk", ac->squawk);
        if (ac->has_pos) {
            cJSON_AddNumberToObject(jf, "lat", ac->lat);
            cJSON_AddNumberToObject(jf, "lon", ac->lon);
            cJSON_AddNumberToObject(jf, "track", (int)ac->track_deg);
        }

        const route_info_t *rt = routes_get_cached(ac->callsign);
        bool rt_ok = rt != NULL && rt->valid &&
                     (!ac->has_pos ||
                      geo_route_plausible(rt->origin.lat, rt->origin.lon,
                                          rt->destination.lat, rt->destination.lon,
                                          ac->lat, ac->lon));
        if (rt_ok) {
            cJSON *jr = cJSON_AddObjectToObject(jf, "route");
            cJSON_AddStringToObject(jr, "from", rt->origin.iata[0] ? rt->origin.iata : rt->origin.icao);
            cJSON_AddStringToObject(jr, "to", rt->destination.iata[0] ? rt->destination.iata : rt->destination.icao);
            cJSON_AddStringToObject(jr, "from_city", rt->origin.city);
            cJSON_AddStringToObject(jr, "to_city", rt->destination.city);
            cJSON_AddStringToObject(jr, "from_cc", rt->origin.country);
            cJSON_AddStringToObject(jr, "to_cc", rt->destination.country);
            time_t now = time(NULL);
            if (now > 1600000000) {
                char lt[8];
                struct tm tm;
                if (rt->origin.tz_known) {
                    time_t l = now + rt->origin.tz_offset_s;
                    gmtime_r(&l, &tm);
                    snprintf(lt, sizeof(lt), "%02d:%02d", tm.tm_hour, tm.tm_min);
                    cJSON_AddStringToObject(jr, "from_time", lt);
                }
                if (rt->destination.tz_known) {
                    time_t l = now + rt->destination.tz_offset_s;
                    gmtime_r(&l, &tm);
                    snprintf(lt, sizeof(lt), "%02d:%02d", tm.tm_hour, tm.tm_min);
                    cJSON_AddStringToObject(jr, "to_time", lt);
                }
            }
            cJSON_AddNumberToObject(jr, "from_lat", rt->origin.lat);
            cJSON_AddNumberToObject(jr, "from_lon", rt->origin.lon);
            cJSON_AddNumberToObject(jr, "to_lat", rt->destination.lat);
            cJSON_AddNumberToObject(jr, "to_lon", rt->destination.lon);
            cJSON_AddNumberToObject(jr, "progress",
                                    (int)(geo_progress(rt->origin.lat, rt->origin.lon,
                                                       rt->destination.lat, rt->destination.lon,
                                                       ac->lat, ac->lon) * 100));
            if (rt->airline_name[0]) {
                cJSON_AddStringToObject(jf, "airline", rt->airline_name);
            }
        }
        if (!cJSON_GetObjectItem(jf, "airline") && flight_is_airline(ac)) {
            char code[4] = { ac->callsign[0], ac->callsign[1], ac->callsign[2], '\0' };
            const char *name = airlines_get_cached(code);
            if (name != NULL && name[0]) {
                cJSON_AddStringToObject(jf, "airline", name);
            }
        }
        cJSON_AddItemToArray(arr, jf);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json != NULL) {
        web_state_publish(json);
        free(json);
    }
}

static void flight_task(void *arg)
{
    if (settings_get()->wifi_ssid[0]) {
        set_status(L()->connecting);
    } else {
        char sbuf[64];
        snprintf(sbuf, sizeof(sbuf), L()->setup_wifi, LV_SYMBOL_SETTINGS);
        set_status(sbuf);
    }
    if (!wifi_mgr_wait_connected(-1)) {
        set_status("Wi-Fi failed");
        vTaskDelete(NULL);
        return;
    }

    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_cfg);

    set_status(L()->locating);
    double lat = 0, lon = 0;
    char city[32] = "";
    while (geolocate_get(&lat, &lon, city, sizeof(city)) != ESP_OK) {
        set_status(L()->geo_retry);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    /* Clock + ETA follow the device's location, wherever it is */
    int home_off;
    if (tz_offset_for("HOME", lat, lon, &home_off)) {
        tz_set_home_offset(home_off);
    }

    char status[96];
    aircraft_list_t *list = calloc(1, sizeof(aircraft_list_t));
    if (list == NULL) {
        set_status("out of memory");
        vTaskDelete(NULL);
        return;
    }

    int radius_nm = settings_get()->radius_nm;
    int consecutive_failures = 0;
    int64_t last_weather_ms = -1;
    static weather_t wx;
    while (true) {
        /* Weather for the header, refreshed every 15 min */
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (last_weather_ms < 0 || now_ms - last_weather_ms > 15 * 60 * 1000) {
            if (weather_fetch(lat, lon, &wx) == ESP_OK) {
                char wbuf[96];
                /* wind before the description so truncation trims words, not data */
                snprintf(wbuf, sizeof(wbuf), "%s %.0f\xC2\xB0""C  \xEF\x9C\xAE %s %.0f  %s",
                         weather_icon_str(wx.code), (double)wx.temp_c,
                         wx.wind_dir_deg >= 0 ? lang_compass(wx.wind_dir_deg) : "",
                         (double)wx.wind_kmh, lang_weather_desc(wx.code));
                if (lvgl_port_lock(1000)) {
                    ui_set_weather(wbuf);
                    lvgl_port_unlock();
                }
            }
            last_weather_ms = now_ms;
        }
        esp_err_t err = flight_fetch_nearby(lat, lon, radius_nm, list);
        if (err == ESP_OK) {
            consecutive_failures = 0;

            const settings_t *cfg = settings_get();
            if (cfg->hide_ground || cfg->hide_private) {
                int w = 0;
                for (int i = 0; i < list->count; i++) {
                    const aircraft_t *ac = &list->ac[i];
                    if (cfg->hide_ground && ac->on_ground) {
                        continue;
                    }
                    if (cfg->hide_private && !flight_is_airline(ac)) {
                        continue;
                    }
                    list->ac[w++] = *ac;
                }
                list->count = w;
            }

            int lookups = 0;
            int top = list->count < ROUTE_LOOKUP_TOP_N ? list->count : ROUTE_LOOKUP_TOP_N;
            for (int i = 0; i < top && lookups < MAX_ROUTE_LOOKUPS_PER_CYCLE; i++) {
                const char *cs = list->ac[i].callsign;
                if (cs[0] != '\0' && routes_get_cached(cs) == NULL) {
                    routes_fetch(cs, list->ac[i].lat, list->ac[i].lon, list->ac[i].has_pos);
                    lookups++;
                    vTaskDelay(pdMS_TO_TICKS(250));
                }
            }

            /* Airline full names for prefixes the route DBs didn't name */
            for (int i = 0; i < top; i++) {
                const char *cs = list->ac[i].callsign;
                if (!flight_is_airline(&list->ac[i])) {
                    continue;
                }
                const route_info_t *rt = routes_get_cached(cs);
                if (rt != NULL && rt->valid && rt->airline_name[0]) {
                    continue;
                }
                char code[4] = { cs[0], cs[1], cs[2], '\0' };
                if (airlines_get_cached(code) == NULL) {
                    airlines_fetch(code);
                    break;      /* one lookup per cycle */
                }
            }

            stats_update(list);

            const aircraft_t *emergency = find_emergency(list);
            if (emergency != NULL) {
                snprintf(status, sizeof(status), LV_SYMBOL_WARNING " %s squawk %s!",
                         emergency->callsign[0] ? emergency->callsign : emergency->hex,
                         emergency->squawk);
            } else {
                snprintf(status, sizeof(status), L()->status_fmt,
                         city[0] ? city : "home", list->count,
                         (int)(radius_nm * 1.852));
            }
            if (lvgl_port_lock(2000)) {
                ui_set_status_alert(emergency != NULL);
                ui_set_status(status);
                ui_update(list);
                lvgl_port_unlock();
            }
            publish_web_state(list, &wx, lat, lon, city, radius_nm);
        } else {
            consecutive_failures++;
            ESP_LOGW(TAG, "fetch failed (%s), %d in a row",
                     esp_err_to_name(err), consecutive_failures);
            if (consecutive_failures >= 3) {
                set_status(L()->no_data);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_CANFLIGHT_POLL_SECONDS * 1000));
    }
}

void flight_task_start(void)
{
    xTaskCreatePinnedToCore(flight_task, "flight_task", 8192, NULL, 4, NULL, 0);
}
