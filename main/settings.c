#include "settings.h"

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
    s_settings.theme = 0;
    s_settings.lang = 1;
    s_settings.ota_enabled = false;   /* never persisted, armed per session */
    s_settings.cpa_alerts = true;
    s_settings.night_enabled = false;
    s_settings.night_start_min = 23 * 60;
    s_settings.night_end_min = 6 * 60 + 30;
    s_settings.ambient_idle_min = 10;

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
    if (nvs_get_i32(h, "radius_nm", &radius) == ESP_OK && radius >= 5 && radius <= 250) {
        s_settings.radius_nm = radius;
    }
    uint8_t hide = 0;
    if (nvs_get_u8(h, "hide_gnd", &hide) == ESP_OK) {
        s_settings.hide_ground = hide != 0;
    }
    if (nvs_get_u8(h, "hide_priv", &hide) == ESP_OK) {
        s_settings.hide_private = hide != 0;
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
    get_str(h, "watch", s_settings.watch_regs, sizeof(s_settings.watch_regs));
    get_str(h, "webhook", s_settings.webhook_url, sizeof(s_settings.webhook_url));
    get_str(h, "ladsb", s_settings.local_adsb, sizeof(s_settings.local_adsb));
    uint8_t b8 = 0;
    if (nvs_get_u8(h, "cpa", &b8) == ESP_OK) {
        s_settings.cpa_alerts = b8 != 0;
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
    nvs_close(h);
    ESP_LOGI(TAG, "loaded: ssid=\"%s\" fixed_loc=%d radius=%d nm",
             s_settings.wifi_ssid, s_settings.use_fixed_loc, s_settings.radius_nm);
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
    snprintf(coord, sizeof(coord), "%.6f", s_settings.lat);
    nvs_set_str(h, "lat", coord);
    snprintf(coord, sizeof(coord), "%.6f", s_settings.lon);
    nvs_set_str(h, "lon", coord);
    nvs_set_i32(h, "radius_nm", s_settings.radius_nm);
    nvs_set_u8(h, "hide_gnd", s_settings.hide_ground ? 1 : 0);
    nvs_set_u8(h, "hide_priv", s_settings.hide_private ? 1 : 0);
    nvs_set_u8(h, "theme", (uint8_t)s_settings.theme);
    nvs_set_u8(h, "lang", (uint8_t)s_settings.lang);
    nvs_set_str(h, "ntfy", s_settings.ntfy_topic);
    nvs_set_str(h, "mqtt", s_settings.mqtt_uri);
    nvs_set_str(h, "fakey", s_settings.fa_key);
    nvs_set_str(h, "watch", s_settings.watch_regs);
    nvs_set_str(h, "webhook", s_settings.webhook_url);
    nvs_set_str(h, "ladsb", s_settings.local_adsb);
    nvs_set_u8(h, "cpa", s_settings.cpa_alerts ? 1 : 0);
    nvs_set_u8(h, "night", s_settings.night_enabled ? 1 : 0);
    nvs_set_i32(h, "night_s", s_settings.night_start_min);
    nvs_set_i32(h, "night_e", s_settings.night_end_min);
    nvs_set_i32(h, "amb_idle", s_settings.ambient_idle_min);

    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved");
    return err;
}
