#include "settings.h"

#include "flight_model.h"
#include "faflight.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "settings";
static const char *NVS_NS = "canflight";

static settings_t s_settings;

settings_t *settings_get(void)
{
    return &s_settings;
}

static void get_str(nvs_handle_t h, const char *key, char *dst, size_t dst_size)
{
    size_t len = dst_size;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        /* keep default */
    }
}

void settings_load(void)
{
    strlcpy(s_settings.wifi_ssid, CONFIG_CANFLIGHT_WIFI_SSID, sizeof(s_settings.wifi_ssid));
    strlcpy(s_settings.wifi_pass, CONFIG_CANFLIGHT_WIFI_PASSWORD, sizeof(s_settings.wifi_pass));
    s_settings.use_fixed_loc = false;
    s_settings.lat = 0;
    s_settings.lon = 0;
    s_settings.radius_nm = CONFIG_CANFLIGHT_RADIUS_NM;
    s_settings.hide_ground = true;
    s_settings.hide_private = false;
    s_settings.show_classes = FCLS_ALL_MASK;
    s_settings.rain_overlay = false;
    s_settings.show_route = false;
    s_settings.brightness_ctl = false;
    s_settings.brightness = 100;
    s_settings.clock_12h = false;
    s_settings.input_map[0] = '\0';
    s_settings.map_light = false;
    s_settings.local_adsb_use = true;
    s_settings.temp_f = false;
    s_settings.retro_map = false;
    s_settings.amb_style = 0;
    s_settings.theme = 0;
    s_settings.lang = 0;   /* English out of the box; 1 = Polish */
    s_settings.ota_enabled = false;   /* never persisted, armed per session */
    s_settings.cpa_alerts = true;
    s_settings.cpa_all = false;
    s_settings.night_enabled = false;
    s_settings.night_start_min = 23 * 60;
    s_settings.night_end_min = 6 * 60 + 30;
    s_settings.ambient_idle_min = 10;
    s_settings.filter_airport[0] = '\0';
    s_settings.filter_apt_exclude = false;
    s_settings.alt_min_ft = 0;
    s_settings.alt_max_ft = 0;
    s_settings.taf_enabled = false;
    s_settings.iss_enabled = false;
    s_settings.sonde_enabled = false;
    s_settings.ships_enabled = false;
    s_settings.airspace_enabled = false;
    s_settings.openaip_key[0] = '\0';
    s_settings.ais_key[0] = '\0';
    s_settings.carto_key[0] = '\0';
    s_settings.tile_url[0] = '\0';
    s_settings.metric_units = false;
    s_settings.metar_decoded = false;
    s_settings.follow_mode = false;
    s_settings.night_auto = false;
    s_settings.fa_limit = FA_DEFAULT_MONTHLY;
    s_settings.fa_used = 0;
    s_settings.fa_month = 0;
    s_settings.pin_nearest = false;
    s_settings.amb_sched = false;
    s_settings.amb_from_min = 22 * 60;
    s_settings.amb_to_min = 7 * 60;
    s_settings.local_merge = false;
    s_settings.track_id[0] = '\0';
    s_settings.track_on = false;
    memset(s_settings.fav_name, 0, sizeof(s_settings.fav_name));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no stored settings, using defaults");
        return;
    }

    get_str(h, "ssid", s_settings.wifi_ssid, sizeof(s_settings.wifi_ssid));
    get_str(h, "pass", s_settings.wifi_pass, sizeof(s_settings.wifi_pass));

    uint8_t fixed = 0;
    if (nvs_get_u8(h, "fixed_loc", &fixed) == ESP_OK) {
        s_settings.use_fixed_loc = fixed != 0;
    }
    char coord[24] = "";
    get_str(h, "lat", coord, sizeof(coord));
    if (coord[0]) {
        s_settings.lat = atof(coord);
    }
    coord[0] = '\0';
    get_str(h, "lon", coord, sizeof(coord));
    if (coord[0]) {
        s_settings.lon = atof(coord);
    }

    int32_t radius = 0;
    if (nvs_get_i32(h, "radius_nm", &radius) == ESP_OK && radius >= 1 && radius <= 250) {
        s_settings.radius_nm = radius;
    }
    uint8_t hide = 0;
    if (nvs_get_u8(h, "hide_gnd", &hide) == ESP_OK) {
        s_settings.hide_ground = hide != 0;
    }
    if (nvs_get_u8(h, "hide_priv", &hide) == ESP_OK) {
        s_settings.hide_private = hide != 0;
    }
    uint8_t cls = 0;
    if (nvs_get_u8(h, "show_cls", &cls) == ESP_OK && (cls & FCLS_ALL_MASK)) {
        s_settings.show_classes = cls & FCLS_ALL_MASK;
    }
    if (nvs_get_u8(h, "map_light", &hide) == ESP_OK) {
        s_settings.map_light = hide != 0;
    }
    if (nvs_get_u8(h, "retro_map", &hide) == ESP_OK) {
        s_settings.retro_map = hide != 0;
    }
    if (nvs_get_u8(h, "rain", &hide) == ESP_OK) {
        s_settings.rain_overlay = hide != 0;
    }
    if (nvs_get_u8(h, "showroute", &hide) == ESP_OK) {
        s_settings.show_route = hide != 0;
    }
    if (nvs_get_u8(h, "brightctl", &hide) == ESP_OK) {
        s_settings.brightness_ctl = hide != 0;
    }
    if (nvs_get_u8(h, "bright", &hide) == ESP_OK && hide >= 5 && hide <= 100) {
        s_settings.brightness = hide;
    }
    if (nvs_get_u8(h, "clk12", &hide) == ESP_OK) {
        s_settings.clock_12h = hide != 0;
    }
    get_str(h, "inputmap", s_settings.input_map, sizeof(s_settings.input_map));
    if (nvs_get_u8(h, "amb_style", &hide) == ESP_OK) {
        s_settings.amb_style = hide == 1 ? 1 : 0;
    }
    uint8_t theme = 0;
    if (nvs_get_u8(h, "theme", &theme) == ESP_OK) {
        s_settings.theme = theme;
    }
    uint8_t lang = 0;
    if (nvs_get_u8(h, "lang", &lang) == ESP_OK) {
        s_settings.lang = lang;
    }
    get_str(h, "ntfy", s_settings.ntfy_topic, sizeof(s_settings.ntfy_topic));
    get_str(h, "mqtt", s_settings.mqtt_uri, sizeof(s_settings.mqtt_uri));
    get_str(h, "fakey", s_settings.fa_key, sizeof(s_settings.fa_key));
    uint16_t u16 = 0;
    if (nvs_get_u16(h, "falimit", &u16) == ESP_OK && u16 > 0) {
        s_settings.fa_limit = u16;
    }
    if (nvs_get_u16(h, "faused", &u16) == ESP_OK) {
        s_settings.fa_used = u16;
    }
    if (nvs_get_u16(h, "famonth", &u16) == ESP_OK) {
        s_settings.fa_month = u16;
    }
    get_str(h, "watch", s_settings.watch_regs, sizeof(s_settings.watch_regs));
    get_str(h, "webhook", s_settings.webhook_url, sizeof(s_settings.webhook_url));
    get_str(h, "ladsb", s_settings.local_adsb, sizeof(s_settings.local_adsb));
    if (nvs_get_u8(h, "ladsb_use", &hide) == ESP_OK) {
        s_settings.local_adsb_use = hide != 0;
    }
    if (nvs_get_u8(h, "temp_f", &hide) == ESP_OK) {
        s_settings.temp_f = hide != 0;
    }
    get_str(h, "webpass", s_settings.web_pass, sizeof(s_settings.web_pass));
    get_str(h, "fltapt", s_settings.filter_airport, sizeof(s_settings.filter_airport));
    int32_t alt = 0;
    if (nvs_get_i32(h, "altmin", &alt) == ESP_OK && alt >= 0) {
        s_settings.alt_min_ft = alt;
    }
    if (nvs_get_i32(h, "altmax", &alt) == ESP_OK && alt >= 0) {
        s_settings.alt_max_ft = alt;
    }
    uint8_t b8 = 0;
    if (nvs_get_u8(h, "cpa", &b8) == ESP_OK) {
        s_settings.cpa_alerts = b8 != 0;
    }
    if (nvs_get_u8(h, "cpa_all", &b8) == ESP_OK) {
        s_settings.cpa_all = b8 != 0;
    }
    if (nvs_get_u8(h, "fltexcl", &b8) == ESP_OK) {
        s_settings.filter_apt_exclude = b8 != 0;
    }
    if (nvs_get_u8(h, "night", &b8) == ESP_OK) {
        s_settings.night_enabled = b8 != 0;
    }
    int32_t m = 0;
    if (nvs_get_i32(h, "night_s", &m) == ESP_OK && m >= 0 && m < 1440) {
        s_settings.night_start_min = m;
    }
    if (nvs_get_i32(h, "night_e", &m) == ESP_OK && m >= 0 && m < 1440) {
        s_settings.night_end_min = m;
    }
    if (nvs_get_i32(h, "amb_idle", &m) == ESP_OK && m >= 0 && m <= 240) {
        s_settings.ambient_idle_min = m;
    }
    if (nvs_get_u8(h, "ambsched", &b8) == ESP_OK) {
        s_settings.amb_sched = b8 != 0;
    }
    if (nvs_get_i32(h, "amb_from", &m) == ESP_OK && m >= 0 && m < 1440) {
        s_settings.amb_from_min = m;
    }
    if (nvs_get_i32(h, "amb_to", &m) == ESP_OK && m >= 0 && m < 1440) {
        s_settings.amb_to_min = m;
    }
    if (nvs_get_u8(h, "pinnear", &b8) == ESP_OK) {
        s_settings.pin_nearest = b8 != 0;
    }
    if (nvs_get_u8(h, "lmerge", &b8) == ESP_OK) {
        s_settings.local_merge = b8 != 0;
    }
    get_str(h, "trackid", s_settings.track_id, sizeof(s_settings.track_id));
    if (nvs_get_u8(h, "trackon", &b8) == ESP_OK) {
        s_settings.track_on = b8 != 0;
    }
    if (nvs_get_u8(h, "taf", &b8) == ESP_OK) {
        s_settings.taf_enabled = b8 != 0;
    }
    if (nvs_get_u8(h, "iss", &b8) == ESP_OK) {
        s_settings.iss_enabled = b8 != 0;
    }
    if (nvs_get_u8(h, "sonde", &b8) == ESP_OK) {
        s_settings.sonde_enabled = b8 != 0;
    }
    if (nvs_get_u8(h, "ships", &b8) == ESP_OK) {
        s_settings.ships_enabled = b8 != 0;
    }
    if (nvs_get_u8(h, "airsp", &b8) == ESP_OK) {
        s_settings.airspace_enabled = b8 != 0;
    }
    get_str(h, "oaipkey", s_settings.openaip_key, sizeof(s_settings.openaip_key));
    get_str(h, "aiskey", s_settings.ais_key, sizeof(s_settings.ais_key));
    get_str(h, "cartokey", s_settings.carto_key, sizeof(s_settings.carto_key));
    get_str(h, "tileurl", s_settings.tile_url, sizeof(s_settings.tile_url));
    if (nvs_get_u8(h, "metric", &b8) == ESP_OK) {
        s_settings.metric_units = b8 != 0;
    }
    if (nvs_get_u8(h, "mdec", &b8) == ESP_OK) {
        s_settings.metar_decoded = b8 != 0;
    }
    if (nvs_get_u8(h, "follow", &b8) == ESP_OK) {
        s_settings.follow_mode = b8 != 0;
    }
    if (nvs_get_u8(h, "nauto", &b8) == ESP_OK) {
        s_settings.night_auto = b8 != 0;
    }
    for (int f = 0; f < 3; f++) {
        char key[12], val[64] = "";
        snprintf(key, sizeof(key), "fav%d", f);
        get_str(h, key, val, sizeof(val));
        if (val[0]) {
            /* "lat|lon|name" */
            double la, lo;
            char nm[24] = "";
            if (sscanf(val, "%lf|%lf|%23[^\n]", &la, &lo, nm) >= 2) {
                s_settings.fav_lat[f] = la;
                s_settings.fav_lon[f] = lo;
                strlcpy(s_settings.fav_name[f], nm, sizeof(s_settings.fav_name[f]));
            }
        }
    }
    nvs_close(h);
    ESP_LOGI(TAG, "loaded: ssid=\"%s\" fixed_loc=%d radius=%d nm lang=%d theme=%d",
             s_settings.wifi_ssid, s_settings.use_fixed_loc, s_settings.radius_nm,
             s_settings.lang, s_settings.theme);
}

esp_err_t settings_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    char coord[24];
    nvs_set_str(h, "ssid", s_settings.wifi_ssid);
    nvs_set_str(h, "pass", s_settings.wifi_pass);
    nvs_set_u8(h, "fixed_loc", s_settings.use_fixed_loc ? 1 : 0);
    nvs_set_u8(h, "map_light", s_settings.map_light ? 1 : 0);
    nvs_set_u8(h, "retro_map", s_settings.retro_map ? 1 : 0);
    snprintf(coord, sizeof(coord), "%.6f", s_settings.lat);
    nvs_set_str(h, "lat", coord);
    snprintf(coord, sizeof(coord), "%.6f", s_settings.lon);
    nvs_set_str(h, "lon", coord);
    nvs_set_i32(h, "radius_nm", s_settings.radius_nm);
    nvs_set_u8(h, "hide_gnd", s_settings.hide_ground ? 1 : 0);
    nvs_set_u8(h, "hide_priv", s_settings.hide_private ? 1 : 0);
    nvs_set_u8(h, "show_cls", s_settings.show_classes);
    nvs_set_u8(h, "rain", s_settings.rain_overlay ? 1 : 0);
    nvs_set_u8(h, "showroute", s_settings.show_route ? 1 : 0);
    nvs_set_u8(h, "brightctl", s_settings.brightness_ctl ? 1 : 0);
    nvs_set_u8(h, "bright", s_settings.brightness);
    nvs_set_u8(h, "clk12", s_settings.clock_12h ? 1 : 0);
    nvs_set_str(h, "inputmap", s_settings.input_map);
    nvs_set_u8(h, "amb_style", s_settings.amb_style);
    nvs_set_u8(h, "theme", (uint8_t)s_settings.theme);
    nvs_set_u8(h, "lang", (uint8_t)s_settings.lang);
    nvs_set_str(h, "ntfy", s_settings.ntfy_topic);
    nvs_set_str(h, "mqtt", s_settings.mqtt_uri);
    nvs_set_str(h, "fakey", s_settings.fa_key);
    nvs_set_u16(h, "falimit", s_settings.fa_limit);
    nvs_set_str(h, "watch", s_settings.watch_regs);
    nvs_set_str(h, "webhook", s_settings.webhook_url);
    nvs_set_str(h, "ladsb", s_settings.local_adsb);
    nvs_set_u8(h, "ladsb_use", s_settings.local_adsb_use ? 1 : 0);
    nvs_set_u8(h, "temp_f", s_settings.temp_f ? 1 : 0);
    nvs_set_str(h, "webpass", s_settings.web_pass);
    nvs_set_str(h, "fltapt", s_settings.filter_airport);
    nvs_set_i32(h, "altmin", s_settings.alt_min_ft);
    nvs_set_i32(h, "altmax", s_settings.alt_max_ft);
    nvs_set_u8(h, "cpa", s_settings.cpa_alerts ? 1 : 0);
    nvs_set_u8(h, "cpa_all", s_settings.cpa_all ? 1 : 0);
    nvs_set_u8(h, "fltexcl", s_settings.filter_apt_exclude ? 1 : 0);
    nvs_set_u8(h, "night", s_settings.night_enabled ? 1 : 0);
    nvs_set_i32(h, "night_s", s_settings.night_start_min);
    nvs_set_i32(h, "night_e", s_settings.night_end_min);
    nvs_set_i32(h, "amb_idle", s_settings.ambient_idle_min);
    nvs_set_u8(h, "ambsched", s_settings.amb_sched ? 1 : 0);
    nvs_set_i32(h, "amb_from", s_settings.amb_from_min);
    nvs_set_i32(h, "amb_to", s_settings.amb_to_min);
    nvs_set_u8(h, "pinnear", s_settings.pin_nearest ? 1 : 0);
    nvs_set_u8(h, "lmerge", s_settings.local_merge ? 1 : 0);
    nvs_set_str(h, "trackid", s_settings.track_id);
    nvs_set_u8(h, "trackon", s_settings.track_on ? 1 : 0);
    nvs_set_u8(h, "taf", s_settings.taf_enabled ? 1 : 0);
    nvs_set_u8(h, "iss", s_settings.iss_enabled ? 1 : 0);
    nvs_set_u8(h, "sonde", s_settings.sonde_enabled ? 1 : 0);
    nvs_set_u8(h, "ships", s_settings.ships_enabled ? 1 : 0);
    nvs_set_u8(h, "airsp", s_settings.airspace_enabled ? 1 : 0);
    nvs_set_str(h, "oaipkey", s_settings.openaip_key);
    nvs_set_str(h, "aiskey", s_settings.ais_key);
    nvs_set_str(h, "cartokey", s_settings.carto_key);
    nvs_set_str(h, "tileurl", s_settings.tile_url);
    nvs_set_u8(h, "metric", s_settings.metric_units ? 1 : 0);
    nvs_set_u8(h, "mdec", s_settings.metar_decoded ? 1 : 0);
    nvs_set_u8(h, "follow", s_settings.follow_mode ? 1 : 0);
    nvs_set_u8(h, "nauto", s_settings.night_auto ? 1 : 0);
    for (int f = 0; f < 3; f++) {
        char key[12], val[64];
        snprintf(key, sizeof(key), "fav%d", f);
        if (s_settings.fav_name[f][0]) {
            snprintf(val, sizeof(val), "%.6f|%.6f|%s",
                     s_settings.fav_lat[f], s_settings.fav_lon[f],
                     s_settings.fav_name[f]);
        } else {
            val[0] = '\0';
        }
        nvs_set_str(h, key, val);
    }

    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved");
    return err;
}

void settings_save_fa_usage(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u16(h, "faused", s_settings.fa_used);
    nvs_set_u16(h, "famonth", s_settings.fa_month);
    nvs_commit(h);
    nvs_close(h);
}
