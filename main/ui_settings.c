#include "ui.h"
#include "ui_settings.h"
#include "esp_log.h"
#include "esp_app_desc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "flight_model.h"
#include "fonts.h"
#include "geocode.h"
#include "lang.h"
#include "lvgl_port.h"
#include "settings.h"
#ifndef APKFLIGHT
#include "waveshare_rgb_lcd_port.h"
#endif
#include "wifi_mgr.h"

#include "theme.h"

#define COL_BG     (app_theme()->bg)
#define COL_PANEL  (app_theme()->panel)
#define COL_ACCENT (app_theme()->accent)
#define COL_TEXT   (app_theme()->text)
#define COL_DIM    (app_theme()->dim)

#define MAX_SCAN_APS    15
#define MAX_GEO_RESULTS 6

static lv_obj_t *s_overlay;
static lv_obj_t *s_kb;
static lv_obj_t *s_ta_ssid, *s_ta_pass, *s_ta_city, *s_ta_lat, *s_ta_lon;
static lv_obj_t *s_ta_track, *s_sw_track;
static lv_obj_t *s_ta_watch, *s_ta_ntfy, *s_ta_mqtt, *s_ta_fa, *s_ta_webhook, *s_ta_ladsb;
static lv_obj_t *s_ta_webpass;
static lv_obj_t *s_ta_fltapt, *s_ta_altmin, *s_ta_altmax;
static lv_obj_t *s_dd_cpa_scope, *s_dd_aptmode;
static lv_obj_t *s_ta_night_from, *s_ta_night_to, *s_ta_amb_idle;
static lv_obj_t *s_ta_amb_from, *s_ta_amb_to, *s_sw_amb_sched, *s_sw_pin_near, *s_sw_lmerge;
static lv_obj_t *s_dd_networks, *s_dd_cities, *s_dd_theme, *s_dd_lang;
static lv_obj_t *s_sw_auto, *s_sw_ground, *s_sw_cpa, *s_sw_night;
static lv_obj_t *s_sw_cls[FCLS_COUNT];
static lv_obj_t *s_sw_rain;
static lv_obj_t *s_sw_airsp, *s_sw_iss, *s_sw_sonde, *s_sw_ships, *s_sw_taf;
static lv_obj_t *s_ta_oaip, *s_ta_ais;
static lv_obj_t *s_ta_carto, *s_ta_tileurl;
static lv_obj_t *s_sw_route;
static lv_obj_t *s_slider_bright, *s_sw_clk12, *s_sw_brightctl;
static lv_obj_t *s_dd_units, *s_dd_metar, *s_sw_cycle, *s_sw_nauto;
static lv_obj_t *s_dd_amb_style;
static lv_obj_t *s_sw_map_light;
static lv_obj_t *s_sw_ladsb;
static lv_obj_t *s_sw_retro_map;
static lv_obj_t *s_slider_radius, *s_radius_label;

static bool s_scan_busy;
static bool s_geo_busy;
static geocode_result_t s_geo_results[MAX_GEO_RESULTS];
static int s_geo_count;

static void close_overlay(void)
{
    if (s_kb != NULL) {
        lv_obj_del(s_kb);
        s_kb = NULL;
    }
    if (s_overlay != NULL) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
    }
}

static void close_cb(lv_event_t *e)
{
    close_overlay();
}

static void kb_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY || lv_event_get_code(e) == LV_EVENT_CANCEL) {
        lv_keyboard_set_textarea(s_kb, NULL);
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    bool numeric = (ta == s_ta_lat || ta == s_ta_lon || ta == s_ta_amb_idle ||
                    ta == s_ta_night_from || ta == s_ta_night_to ||
                    ta == s_ta_amb_from || ta == s_ta_amb_to);
    lv_keyboard_set_mode(s_kb, numeric ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_kb, ta);
    lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kb);
}

static void ta_defocus_cb(lv_event_t *e)
{
    if (s_kb != NULL && lv_keyboard_get_textarea(s_kb) == lv_event_get_target(e)) {
        lv_keyboard_set_textarea(s_kb, NULL);
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- Wi-Fi scan ---- */

static void scan_task(void *arg)
{
    static wifi_ap_record_t recs[MAX_SCAN_APS];
    uint16_t n = MAX_SCAN_APS;
    esp_err_t err = wifi_mgr_scan(recs, &n);

    if (lvgl_port_lock(-1)) {
        if (s_overlay != NULL) {
            if (err != ESP_OK || n == 0) {
                lv_dropdown_set_options(s_dd_networks, L()->no_networks);
            } else {
                char opts[MAX_SCAN_APS * 34] = "";
                for (int i = 0; i < n; i++) {
                    const char *ssid = (const char *)recs[i].ssid;
                    if (ssid[0] == '\0' || strstr(opts, ssid) != NULL) {
                        continue;
                    }
                    strlcat(opts, ssid, sizeof(opts));
                    strlcat(opts, "\n", sizeof(opts));
                }
                size_t len = strlen(opts);
                if (len > 0) {
                    opts[len - 1] = '\0';
                }
                lv_dropdown_set_options(s_dd_networks, opts[0] ? opts : L()->no_networks);
                lv_dropdown_open(s_dd_networks);
            }
        }
        lvgl_port_unlock();
    }
    s_scan_busy = false;
    vTaskDelete(NULL);
}

static void scan_click_cb(lv_event_t *e)
{
    if (s_scan_busy) {
        return;
    }
    s_scan_busy = true;
    lv_dropdown_set_options(s_dd_networks, L()->scanning);
    xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 3, NULL);
}

static void network_pick_cb(lv_event_t *e)
{
    char ssid[33];
    lv_dropdown_get_selected_str(s_dd_networks, ssid, sizeof(ssid));
    if (ssid[0] != '(') {
        lv_textarea_set_text(s_ta_ssid, ssid);
    }
}

/* ---- City search ---- */

static void geo_task(void *arg)
{
    char *query = arg;
    esp_err_t err = geocode_search(query, s_geo_results, MAX_GEO_RESULTS, &s_geo_count);
    free(query);

    if (lvgl_port_lock(-1)) {
        if (s_overlay != NULL) {
            if (err != ESP_OK || s_geo_count == 0) {
                lv_dropdown_set_options(s_dd_cities, L()->not_found);
            } else {
                char opts[MAX_GEO_RESULTS * 96] = "";
                for (int i = 0; i < s_geo_count; i++) {
                    char line[96];
                    snprintf(line, sizeof(line), "%.39s, %.7s (%.31s)\n",
                             s_geo_results[i].name, s_geo_results[i].country,
                             s_geo_results[i].region);
                    strlcat(opts, line, sizeof(opts));
                }
                opts[strlen(opts) - 1] = '\0';
                lv_dropdown_set_options(s_dd_cities, opts);
                lv_dropdown_open(s_dd_cities);
            }
        }
        lvgl_port_unlock();
    }
    s_geo_busy = false;
    vTaskDelete(NULL);
}

static void city_search_cb(lv_event_t *e)
{
    if (s_geo_busy) {
        return;
    }
    const char *q = lv_textarea_get_text(s_ta_city);
    if (q[0] == '\0') {
        return;
    }
    s_geo_busy = true;
    lv_dropdown_set_options(s_dd_cities, L()->searching);
    xTaskCreate(geo_task, "geocode", 8192, strdup(q), 3, NULL);
}

static void city_pick_cb(lv_event_t *e)
{
    int sel = lv_dropdown_get_selected(s_dd_cities);
    if (sel < 0 || sel >= s_geo_count) {
        return;
    }
    char coord[24];
    snprintf(coord, sizeof(coord), "%.4f", s_geo_results[sel].lat);
    lv_textarea_set_text(s_ta_lat, coord);
    snprintf(coord, sizeof(coord), "%.4f", s_geo_results[sel].lon);
    lv_textarea_set_text(s_ta_lon, coord);
    lv_obj_clear_state(s_sw_auto, LV_STATE_CHECKED);
    lv_obj_clear_state(s_ta_lat, LV_STATE_DISABLED);
    lv_obj_clear_state(s_ta_lon, LV_STATE_DISABLED);
}

/* ---- misc ---- */

static void auto_loc_cb(lv_event_t *e)
{
    bool automatic = lv_obj_has_state(s_sw_auto, LV_STATE_CHECKED);
    if (automatic) {
        lv_obj_add_state(s_ta_lat, LV_STATE_DISABLED);
        lv_obj_add_state(s_ta_lon, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(s_ta_lat, LV_STATE_DISABLED);
        lv_obj_clear_state(s_ta_lon, LV_STATE_DISABLED);
    }
}

#ifndef APKFLIGHT
static void brightness_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    /* live preview only when the master switch is armed (either already
     * saved, or toggled on right now on this screen) */
    if (settings_get()->brightness_ctl ||
        (s_sw_brightctl != NULL && lv_obj_has_state(s_sw_brightctl, LV_STATE_CHECKED))) {
        waveshare_rgb_lcd_bl_pct(lv_slider_get_value(sl));
    }
}
#endif

static void radius_cb(lv_event_t *e)
{
    int nm = lv_slider_get_value(s_slider_radius);
    char buf[48];
    snprintf(buf, sizeof(buf), "%d nm  (%d km)", nm, (int)(nm * 1.852));
    lv_label_set_text(s_radius_label, buf);
}

static void ota_unlock_cb(lv_event_t *e)
{
    settings_get()->ota_enabled =
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
}

static int parse_hhmm(const char *txt, int fallback)
{
    int h = 0, m = 0;
    /* the on-screen numeric keypad has no colon, so 21:00, 21.00,
       2100, 830 and a bare 21 all have to parse */
    if (sscanf(txt, "%d:%d", &h, &m) != 2 && sscanf(txt, "%d.%d", &h, &m) != 2) {
        char d[6];
        int n = 0;
        for (const char *c = txt; *c; c++) {
            if (*c >= '0' && *c <= '9') {
                if (n >= (int)sizeof(d) - 1) return fallback;
                d[n++] = *c;
            } else if (*c != ' ') {
                return fallback;
            }
        }
        if (n == 0) return fallback;
        d[n] = 0;
        if (n <= 2) {
            h = atoi(d);
            m = 0;
        } else {
            m = atoi(d + n - 2);
            d[n - 2] = 0;
            h = atoi(d);
        }
    }
    if (h >= 0 && h < 24 && m >= 0 && m < 60) {
        return h * 60 + m;
    }
    return fallback;
}

static void save_cb(lv_event_t *e)
{
    settings_t *cfg = settings_get();
#ifndef APKFLIGHT_NO_WIFI
    strlcpy(cfg->wifi_ssid, lv_textarea_get_text(s_ta_ssid), sizeof(cfg->wifi_ssid));
    const char *pw = lv_textarea_get_text(s_ta_pass);
    if (pw[0] != '\0') {
        strlcpy(cfg->wifi_pass, pw, sizeof(cfg->wifi_pass));
    }
#endif
    cfg->use_fixed_loc = !lv_obj_has_state(s_sw_auto, LV_STATE_CHECKED);
    cfg->lat = atof(lv_textarea_get_text(s_ta_lat));
    cfg->lon = atof(lv_textarea_get_text(s_ta_lon));
    cfg->radius_nm = lv_slider_get_value(s_slider_radius);
    strlcpy(cfg->track_id, lv_textarea_get_text(s_ta_track), sizeof(cfg->track_id));
    cfg->track_on = lv_obj_has_state(s_sw_track, LV_STATE_CHECKED) && cfg->track_id[0] != '\0';
    cfg->hide_ground = lv_obj_has_state(s_sw_ground, LV_STATE_CHECKED);
    cfg->hide_private = false;   /* legacy switch, superseded by the class mask */
    cfg->show_classes = 0;
    for (int i = 0; i < FCLS_COUNT; i++) {
        if (lv_obj_has_state(s_sw_cls[i], LV_STATE_CHECKED)) {
            cfg->show_classes |= (uint8_t)(1u << i);
        }
    }
    if (cfg->show_classes == 0) {   /* nothing checked = show everything */
        cfg->show_classes = FCLS_ALL_MASK;
    }
    cfg->cpa_alerts = lv_obj_has_state(s_sw_cpa, LV_STATE_CHECKED);
    cfg->night_enabled = lv_obj_has_state(s_sw_night, LV_STATE_CHECKED);
    cfg->night_start_min = parse_hhmm(lv_textarea_get_text(s_ta_night_from),
                                      cfg->night_start_min);
    cfg->night_end_min = parse_hhmm(lv_textarea_get_text(s_ta_night_to),
                                    cfg->night_end_min);
    char hhmm[16];
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d",
             cfg->night_start_min / 60, cfg->night_start_min % 60);
    lv_textarea_set_text(s_ta_night_from, hhmm);
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d",
             cfg->night_end_min / 60, cfg->night_end_min % 60);
    lv_textarea_set_text(s_ta_night_to, hhmm);
    cfg->ambient_idle_min = atoi(lv_textarea_get_text(s_ta_amb_idle));
    cfg->amb_sched = lv_obj_has_state(s_sw_amb_sched, LV_STATE_CHECKED);
    cfg->amb_from_min = parse_hhmm(lv_textarea_get_text(s_ta_amb_from), cfg->amb_from_min);
    cfg->amb_to_min = parse_hhmm(lv_textarea_get_text(s_ta_amb_to), cfg->amb_to_min);
    cfg->pin_nearest = lv_obj_has_state(s_sw_pin_near, LV_STATE_CHECKED);
    cfg->rain_overlay = lv_obj_has_state(s_sw_rain, LV_STATE_CHECKED);
    cfg->show_route = lv_obj_has_state(s_sw_route, LV_STATE_CHECKED);
    cfg->clock_12h = lv_obj_has_state(s_sw_clk12, LV_STATE_CHECKED);
#ifndef APKFLIGHT
    if (s_slider_bright != NULL) {
        cfg->brightness = (uint8_t)lv_slider_get_value(s_slider_bright);
    }
    if (s_sw_brightctl != NULL) {
        cfg->brightness_ctl = lv_obj_has_state(s_sw_brightctl, LV_STATE_CHECKED);
    }
#endif
    cfg->airspace_enabled = lv_obj_has_state(s_sw_airsp, LV_STATE_CHECKED);
    cfg->iss_enabled = lv_obj_has_state(s_sw_iss, LV_STATE_CHECKED);
    cfg->sonde_enabled = lv_obj_has_state(s_sw_sonde, LV_STATE_CHECKED);
    cfg->ships_enabled = lv_obj_has_state(s_sw_ships, LV_STATE_CHECKED);
    strlcpy(cfg->ais_key, lv_textarea_get_text(s_ta_ais), sizeof(cfg->ais_key));
    cfg->taf_enabled = lv_obj_has_state(s_sw_taf, LV_STATE_CHECKED);
    strlcpy(cfg->openaip_key, lv_textarea_get_text(s_ta_oaip), sizeof(cfg->openaip_key));
    strlcpy(cfg->carto_key, lv_textarea_get_text(s_ta_carto), sizeof(cfg->carto_key));
    strlcpy(cfg->tile_url, lv_textarea_get_text(s_ta_tileurl), sizeof(cfg->tile_url));
    cfg->amb_style = (uint8_t)lv_dropdown_get_selected(s_dd_amb_style);
    cfg->map_light = lv_obj_has_state(s_sw_map_light, LV_STATE_CHECKED);
    cfg->retro_map = lv_obj_has_state(s_sw_retro_map, LV_STATE_CHECKED);
    {
        int u = lv_dropdown_get_selected(s_dd_units);
        cfg->metric_units = u == 2;
        cfg->temp_f = u == 1;
    }
    cfg->metar_decoded = lv_dropdown_get_selected(s_dd_metar) == 1;
    cfg->follow_mode = !lv_obj_has_state(s_sw_cycle, LV_STATE_CHECKED);
    cfg->night_auto = lv_obj_has_state(s_sw_nauto, LV_STATE_CHECKED);
    strlcpy(cfg->watch_regs, lv_textarea_get_text(s_ta_watch), sizeof(cfg->watch_regs));
    strlcpy(cfg->ntfy_topic, lv_textarea_get_text(s_ta_ntfy), sizeof(cfg->ntfy_topic));
    strlcpy(cfg->mqtt_uri, lv_textarea_get_text(s_ta_mqtt), sizeof(cfg->mqtt_uri));
    strlcpy(cfg->fa_key, lv_textarea_get_text(s_ta_fa), sizeof(cfg->fa_key));
    strlcpy(cfg->webhook_url, lv_textarea_get_text(s_ta_webhook), sizeof(cfg->webhook_url));
    strlcpy(cfg->local_adsb, lv_textarea_get_text(s_ta_ladsb), sizeof(cfg->local_adsb));
    cfg->local_adsb_use = lv_obj_has_state(s_sw_ladsb, LV_STATE_CHECKED);
    cfg->local_merge = lv_obj_has_state(s_sw_lmerge, LV_STATE_CHECKED);
#ifndef APKFLIGHT   /* no web panel in the app - the field doesn't exist */
    strlcpy(cfg->web_pass, lv_textarea_get_text(s_ta_webpass), sizeof(cfg->web_pass));
#endif
    strlcpy(cfg->filter_airport, lv_textarea_get_text(s_ta_fltapt), sizeof(cfg->filter_airport));
    cfg->alt_min_ft = atoi(lv_textarea_get_text(s_ta_altmin));
    cfg->alt_max_ft = atoi(lv_textarea_get_text(s_ta_altmax));
    cfg->filter_apt_exclude = lv_dropdown_get_selected(s_dd_aptmode) == 1;
    cfg->cpa_all = lv_dropdown_get_selected(s_dd_cpa_scope) == 1;
    cfg->theme = lv_dropdown_get_selected(s_dd_theme);
    cfg->lang = lv_dropdown_get_selected(s_dd_lang);
    ESP_LOGI("settings", "device save: lang=%d theme=%d fixed=%d",
             cfg->lang, cfg->theme, cfg->use_fixed_loc);
    settings_save();

#ifdef APKFLIGHT
    /* App build: no process restart. Signal the flight task to re-apply
     * location/radius live, then just close the settings overlay. */
    extern void apk_settings_touch(void);
    apk_settings_touch();
    close_overlay();
    return;
#else
    lv_obj_t *msg = lv_label_create(s_overlay);
    lv_obj_set_style_text_color(msg, COL_ACCENT, 0);
    lv_obj_set_style_text_font(msg, UIFONT(&lv_font_montserrat_24, &lv_font_montserrat_14), 0);
    lv_obj_set_style_bg_color(msg, COL_BG, 0);
    lv_obj_set_style_bg_opa(msg, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(msg, UISY(20), 0);
    lv_label_set_text(msg, L()->saved_restarting);
    lv_obj_center(msg);
    lv_refr_now(NULL);
    esp_restart();
#endif
}

/* ---- widget helpers ---- */

static lv_obj_t *add_label(lv_obj_t *parent, const char *text, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, UIFONT(&font_pl_16, &font_pl_10), 0);
    lv_obj_set_style_text_color(l, COL_DIM, 0);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, UISX(x), UISY(y));
    return l;
}

static void add_section(lv_obj_t *parent, const char *text, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, UIFONT(&font_pl_14, &font_pl_8), 0);
    lv_obj_set_style_text_color(l, COL_ACCENT, 0);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, 0, UISY(y));
    lv_obj_t *ln = lv_obj_create(parent);
    lv_obj_set_size(ln, UISX(740), 1);
    lv_obj_set_pos(ln, 0, UISY(y) + UISY(22));
    lv_obj_set_style_bg_color(ln, COL_DIM, 0);
    lv_obj_set_style_bg_opa(ln, LV_OPA_40, 0);
    lv_obj_set_style_border_width(ln, 0, 0);
    lv_obj_clear_flag(ln, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *add_hint(lv_obj_t *parent, const char *text, int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, UIFONT(&font_pl_14, &font_pl_8), 0);
    lv_obj_set_style_text_color(l, COL_DIM, 0);
    lv_obj_set_width(l, UISX(w));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(l, UISX(x), UISY(y));
    return l;
}

#ifdef APKFLIGHT
const char *apk_clipboard_text(void);

static void ta_paste_cb(lv_event_t *e)
{
    const char *clip = apk_clipboard_text();
    if (clip != NULL && clip[0] != '\0' && strlen(clip) < 128) {
        lv_textarea_add_text(lv_event_get_target(e), clip);
    }
}
#endif

static lv_obj_t *add_textarea(lv_obj_t *parent, int x, int y, int w, const char *value, bool password)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_pos(ta, UISX(x), UISY(y));
    lv_obj_set_size(ta, UISX(w), UISY(44));
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, password);
    lv_textarea_set_text(ta, value);
    lv_obj_set_style_bg_color(ta, COL_PANEL, 0);
    lv_obj_set_style_text_color(ta, COL_TEXT, 0);
    lv_obj_set_style_text_font(ta, UIFONT(&font_pl_16, &font_pl_10), 0);
    lv_obj_set_style_border_color(ta, COL_DIM, 0);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, ta_defocus_cb, LV_EVENT_DEFOCUSED, NULL);
#ifdef APKFLIGHT
    /* long-press pastes the clipboard: 32-char API keys beg for it */
    lv_obj_add_event_cb(ta, ta_paste_cb, LV_EVENT_LONG_PRESSED, NULL);
#endif
    return ta;
}

static lv_obj_t *add_button(lv_obj_t *parent, int x, int y, int w, int h,
                            const char *text, lv_event_cb_t cb, lv_color_t bg)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, UISX(x), UISY(y));
    lv_obj_set_size(btn, UISX(w), UISY(h));
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_style_text_font(l, UIFONT(&font_pl_16, &font_pl_10), 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return btn;
}

static lv_obj_t *add_dropdown(lv_obj_t *parent, int x, int y, int w, lv_event_cb_t cb)
{
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_obj_set_pos(dd, UISX(x), UISY(y));
    lv_obj_set_size(dd, UISX(w), UISY(44));
    lv_dropdown_set_options(dd, "");
    lv_obj_set_style_bg_color(dd, COL_PANEL, 0);
    lv_obj_set_style_text_color(dd, COL_TEXT, 0);
    lv_obj_set_style_text_font(dd, UIFONT(&font_pl_16, &font_pl_10), 0);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_set_style_text_font(list, UIFONT(&font_pl_16, &font_pl_10), 0);
    lv_obj_set_style_bg_color(list, COL_PANEL, 0);
    lv_obj_set_style_text_color(list, COL_TEXT, 0);
    if (cb != NULL) {
        lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);
    }
    return dd;
}

static lv_obj_t *add_switch(lv_obj_t *parent, const char *label, int x, int y, bool on)
{
    add_label(parent, label, x, y + 6);
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_pos(sw, UISX(x) + UISX(290), UISY(y));
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    return sw;
}

static lv_obj_t *tab_page(lv_obj_t *tv, const char *name)
{
    lv_obj_t *page = lv_tabview_add_tab(tv, name);
    lv_obj_set_style_pad_all(page, UISY(14), 0);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    return page;
}

static lv_obj_t *s_tabview;

void ui_settings_show_tab(int idx)
{
    ui_settings_open();
    if (s_tabview != NULL && idx >= 0 && idx <= 4) {
        lv_tabview_set_act(s_tabview, (uint32_t)idx, LV_ANIM_OFF);
    }
}

void ui_settings_open(void)
{
    if (s_overlay != NULL) {
        return;
    }
    settings_t *cfg = settings_get();
    char buf[48];

    s_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, COL_BG, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* header row: title + save + close */
    lv_obj_t *title = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(title, UIFONT(&font_pl_20, &font_pl_12), 0);
    lv_obj_set_style_text_color(title, COL_ACCENT, 0);
    lv_label_set_text_fmt(title, LV_SYMBOL_SETTINGS " %s", L()->settings_title);
    lv_obj_set_pos(title, UISX(14), UISY(12));

    if (ui_update_available()) {
        lv_obj_t *up = lv_label_create(s_overlay);
        lv_obj_set_style_text_font(up, UIFONT(&font_pl_14, &font_pl_8), 0);
        lv_obj_set_style_text_color(up, lv_color_hex(0xffd166), 0);
        char upt[96];
        snprintf(upt, sizeof(upt), L()->update_banner, ui_update_tag());
        lv_label_set_text_fmt(up, LV_SYMBOL_DOWNLOAD " %s", upt);
        lv_obj_align(up, LV_ALIGN_TOP_MID, -UISX(20), UISY(16));
    }

    snprintf(buf, sizeof(buf), LV_SYMBOL_SAVE "  %s", L()->save);
    /* right-anchored: physical screen edge minus scaled design offset (the
     * helper would UISX() the whole mixed expression) */
    lv_obj_t *hbtn = add_button(s_overlay, 0, 6, 150, 38, buf, save_cb, COL_ACCENT);
    lv_obj_set_x(hbtn, LV_HOR_RES - UISX(230));
    hbtn = add_button(s_overlay, 0, 6, 54, 38, LV_SYMBOL_CLOSE, close_cb, COL_PANEL);
    lv_obj_set_x(hbtn, LV_HOR_RES - UISX(66));

    lv_obj_t *tv = lv_tabview_create(s_overlay, LV_DIR_TOP, UISY(44));
    s_tabview = tv;
    lv_obj_set_size(tv, LV_HOR_RES, LV_VER_RES - UISY(50));
    lv_obj_set_pos(tv, 0, UISY(50));
    lv_obj_set_style_bg_color(tv, COL_BG, 0);
    lv_obj_t *bar = lv_tabview_get_tab_btns(tv);
    lv_obj_set_style_bg_color(bar, COL_PANEL, 0);
    lv_obj_set_style_text_color(bar, COL_TEXT, 0);
    lv_obj_set_style_text_font(bar, UIFONT(&font_pl_16, &font_pl_10), 0);

#ifdef APKFLIGHT_NO_WIFI
    /* App build: Wi-Fi is the host OS's job, so the Network tab is dropped. */
    lv_obj_t *p;
#else
    /* --- Network --- */
    lv_obj_t *p = tab_page(tv, L()->tab_net);
    add_label(p, L()->wifi_ssid, 0, 0);
    s_ta_ssid = add_textarea(p, 0, 24, 290, cfg->wifi_ssid, false);
    add_button(p, 300, 24, 56, 44, LV_SYMBOL_REFRESH, scan_click_cb, COL_PANEL);
    s_dd_networks = add_dropdown(p, 366, 24, 250, network_pick_cb);
    lv_dropdown_set_text(s_dd_networks, L()->dd_networks);
    add_label(p, L()->password, 0, 84);
    s_ta_pass = add_textarea(p, 0, 108, 380, "", true);
#endif

    /* --- Location --- */
    p = tab_page(tv, L()->tab_place);
    s_sw_auto = add_switch(p, L()->auto_location, 0, 0, !cfg->use_fixed_loc);
    lv_obj_add_event_cb(s_sw_auto, auto_loc_cb, LV_EVENT_VALUE_CHANGED, NULL);
    add_label(p, L()->city_search, 0, 56);
    s_ta_city = add_textarea(p, 0, 80, 290, "", false);
    add_button(p, 300, 80, 56, 44, LV_SYMBOL_GPS, city_search_cb, COL_PANEL);
    s_dd_cities = add_dropdown(p, 366, 80, 380, city_pick_cb);
    lv_dropdown_set_text(s_dd_cities, L()->dd_results);
    add_label(p, L()->latitude, 0, 140);
    snprintf(buf, sizeof(buf), "%.4f", cfg->lat);
    s_ta_lat = add_textarea(p, 0, 164, 200, buf, false);
    add_label(p, L()->longitude, 220, 140);
    snprintf(buf, sizeof(buf), "%.4f", cfg->lon);
    s_ta_lon = add_textarea(p, 220, 164, 200, buf, false);
    if (!cfg->use_fixed_loc) {
        lv_obj_add_state(s_ta_lat, LV_STATE_DISABLED);
        lv_obj_add_state(s_ta_lon, LV_STATE_DISABLED);
    }
    add_label(p, L()->search_radius, 0, 226);
    s_slider_radius = lv_slider_create(p);
    lv_obj_set_size(s_slider_radius, UISX(420), UISY(16));
    lv_obj_set_pos(s_slider_radius, 0, UISY(258));
    lv_slider_set_range(s_slider_radius, 1, 250);
    lv_slider_set_value(s_slider_radius, cfg->radius_nm, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_slider_radius, radius_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_radius_label = add_label(p, "", 440, 254);
    lv_obj_set_style_text_color(s_radius_label, COL_TEXT, 0);
    snprintf(buf, sizeof(buf), "%d nm  (%d km)", cfg->radius_nm, (int)(cfg->radius_nm * 1.852));
    lv_label_set_text(s_radius_label, buf);

    /* track one flight anywhere (callsign, registration or hex) */
    add_label(p, L()->track_lbl, 0, 304);
    s_ta_track = add_textarea(p, 0, 328, 290, cfg->track_id, false);
    lv_textarea_set_max_length(s_ta_track, sizeof(cfg->track_id) - 1);
    s_sw_track = add_switch(p, L()->track_on_lbl, 380, 334, cfg->track_on);

    /* --- Filters + alerts --- */
    p = tab_page(tv, L()->tab_filters);
    add_section(p, L()->sec_traffic, 0);
    s_sw_ground = add_switch(p, L()->hide_ground, 0, 32, cfg->hide_ground);
    add_label(p, L()->lbl_altmin, 0, 88);
    snprintf(buf, sizeof(buf), "%d", cfg->alt_min_ft);
    s_ta_altmin = add_textarea(p, 0, 112, 140, buf, false);
    add_label(p, L()->lbl_altmax, 180, 88);
    snprintf(buf, sizeof(buf), "%d", cfg->alt_max_ft);
    s_ta_altmax = add_textarea(p, 180, 112, 140, buf, false);
    add_label(p, L()->lbl_fltapt, 380, 88);
    s_ta_fltapt = add_textarea(p, 380, 112, 140, cfg->filter_airport, false);
    s_dd_aptmode = add_dropdown(p, 540, 112, 200, NULL);
    lv_dropdown_set_options(s_dd_aptmode, L()->apt_mode_opts);
    lv_dropdown_set_selected(s_dd_aptmode, cfg->filter_apt_exclude ? 1 : 0);

    add_section(p, L()->sec_watch, 180);
    s_ta_watch = add_textarea(p, 0, 212, 740, cfg->watch_regs, false);
    add_hint(p, L()->lbl_watch, 0, 258, 740);

    add_section(p, L()->sec_alerts, 292);
    s_sw_cpa = add_switch(p, L()->cpa_lbl, 0, 324, cfg->cpa_alerts);
    s_dd_cpa_scope = add_dropdown(p, 500, 320, 240, NULL);
    lv_dropdown_set_options(s_dd_cpa_scope, L()->cpa_scope_opts);
    lv_dropdown_set_selected(s_dd_cpa_scope, cfg->cpa_all ? 1 : 0);

    add_section(p, L()->sec_classes, 380);
    for (int i = 0; i < FCLS_COUNT; i++) {
        s_sw_cls[i] = add_switch(p, L()->cls_names[i], (i % 2) * 380,
                                 412 + (i / 2) * 52,
                                 (cfg->show_classes >> i) & 1);
    }

    add_section(p, L()->sec_layers, 580);
    s_sw_rain = add_switch(p, L()->rain_lbl, 0, 612, cfg->rain_overlay);
    s_sw_airsp = add_switch(p, L()->airspace_lbl, 380, 612, cfg->airspace_enabled);
    s_sw_iss = add_switch(p, L()->iss_lbl, 0, 664, cfg->iss_enabled);
    s_sw_sonde = add_switch(p, L()->sonde_lbl, 380, 664, cfg->sonde_enabled);
    s_sw_ships = add_switch(p, L()->ships_lbl, 0, 716, cfg->ships_enabled);
    s_sw_taf = add_switch(p, L()->taf_lbl, 380, 716, cfg->taf_enabled);
    s_sw_route = add_switch(p, L()->route_lbl, 0, 768, cfg->show_route);

    /* --- Integrations --- */
    p = tab_page(tv, L()->tab_integr);
    add_section(p, L()->sec_notify, 0);
    add_label(p, L()->lbl_ntfy, 0, 32);
    s_ta_ntfy = add_textarea(p, 0, 56, 360, cfg->ntfy_topic, false);
    add_hint(p, L()->hint_ntfy, 0, 102, 360);
    add_label(p, L()->lbl_webhook, 380, 32);
    s_ta_webhook = add_textarea(p, 380, 56, 360, cfg->webhook_url, false);
    add_hint(p, L()->hint_webhook, 380, 102, 360);

    add_section(p, L()->sec_datasrc, 154);
    s_sw_ladsb = add_switch(p, L()->ladsb_use_lbl, 380, 148, cfg->local_adsb_use);
    add_label(p, L()->lbl_fa, 0, 186);
    s_ta_fa = add_textarea(p, 0, 210, 360, cfg->fa_key, false);
    add_hint(p, L()->hint_fa, 0, 256, 360);
    add_label(p, L()->lbl_ladsb, 380, 186);
    s_ta_ladsb = add_textarea(p, 380, 210, 360, cfg->local_adsb, false);
    add_hint(p, L()->hint_ladsb, 380, 256, 360);
    s_sw_lmerge = add_switch(p, L()->lmerge_lbl, 380, 300, cfg->local_merge);
    add_label(p, L()->lbl_oaip, 0, 356);
    s_ta_oaip = add_textarea(p, 0, 380, 360, cfg->openaip_key, false);
    add_hint(p, L()->hint_oaip, 0, 426, 360);
    add_label(p, L()->lbl_ais, 380, 356);
    s_ta_ais = add_textarea(p, 380, 380, 360, cfg->ais_key, false);
    add_hint(p, L()->hint_ais, 380, 426, 360);

    add_section(p, L()->sec_maptiles, 478);
    add_label(p, L()->lbl_carto, 0, 510);
    s_ta_carto = add_textarea(p, 0, 534, 360, cfg->carto_key, false);
    add_hint(p, L()->hint_carto, 0, 580, 360);
    add_label(p, L()->lbl_tileurl, 380, 510);
    s_ta_tileurl = add_textarea(p, 380, 534, 360, cfg->tile_url, false);
    add_hint(p, L()->hint_tileurl, 380, 580, 360);

    add_section(p, L()->sec_smart, 632);
    add_label(p, L()->lbl_mqtt, 0, 664);
    s_ta_mqtt = add_textarea(p, 0, 688, 360, cfg->mqtt_uri, false);
    add_hint(p, L()->hint_mqtt, 0, 734, 740);

    /* --- System --- */
    p = tab_page(tv, L()->tab_system);
    /* System tab on a strict grid: two columns (x 0 and 380), labels 24 px
     * above their control, 56 px row pitch, 66 px between sections. */
    add_section(p, L()->sec_look, 0);
    add_label(p, L()->theme_lbl, 0, 32);
    s_dd_theme = add_dropdown(p, 0, 56, 300, NULL);
    lv_dropdown_set_options(s_dd_theme, theme_names_option_string());
    lv_dropdown_set_selected(s_dd_theme, cfg->theme < THEME_COUNT ? cfg->theme : 0);
    add_label(p, L()->language_lbl, 380, 32);
    s_dd_lang = add_dropdown(p, 380, 56, 300, NULL);
    lv_dropdown_set_options(s_dd_lang, "English\nPolski\nMagyar");
    lv_dropdown_set_selected(s_dd_lang, cfg->lang >= 0 && cfg->lang <= 2 ? cfg->lang : 0);
    add_label(p, L()->units_lbl, 0, 112);
    s_dd_units = add_dropdown(p, 0, 136, 300, NULL);
    lv_dropdown_set_options(s_dd_units, L()->units_opts);
    lv_dropdown_set_selected(s_dd_units,
                             cfg->metric_units ? 2 : (cfg->temp_f ? 1 : 0));
    add_label(p, L()->metar_lbl, 380, 112);
    s_dd_metar = add_dropdown(p, 380, 136, 300, NULL);
    lv_dropdown_set_options(s_dd_metar, L()->metar_opts);
    lv_dropdown_set_selected(s_dd_metar, cfg->metar_decoded ? 1 : 0);

    add_section(p, L()->sec_screen, 202);
    s_sw_night = add_switch(p, L()->night_lbl, 0, 234, cfg->night_enabled);
    add_label(p, L()->night_from, 380, 228);
    snprintf(buf, sizeof(buf), "%02d:%02d", cfg->night_start_min / 60, cfg->night_start_min % 60);
    s_ta_night_from = add_textarea(p, 380, 252, 110, buf, false);
    add_label(p, L()->night_to, 510, 228);
    snprintf(buf, sizeof(buf), "%02d:%02d", cfg->night_end_min / 60, cfg->night_end_min % 60);
    s_ta_night_to = add_textarea(p, 510, 252, 110, buf, false);
    s_sw_nauto = add_switch(p, L()->night_auto_lbl, 0, 306, cfg->night_auto);
    s_sw_cycle = add_switch(p, L()->follow_lbl, 0, 358, !cfg->follow_mode);
    s_sw_pin_near = add_switch(p, L()->pin_near_lbl, 380, 358, cfg->pin_nearest);
    add_label(p, L()->amb_idle_lbl, 0, 416);
    snprintf(buf, sizeof(buf), "%d", cfg->ambient_idle_min);
    s_ta_amb_idle = add_textarea(p, 630, 410, 110, buf, false);
    add_label(p, L()->amb_style_lbl, 0, 472);
    s_dd_amb_style = add_dropdown(p, 520, 466, 220, NULL);
    lv_dropdown_set_options(s_dd_amb_style, L()->amb_style_opts);
    lv_dropdown_set_selected(s_dd_amb_style, cfg->amb_style == 1 ? 1 : 0);
    s_sw_amb_sched = add_switch(p, L()->amb_sched_lbl, 0, 530, cfg->amb_sched);
    add_label(p, L()->night_from, 380, 518);
    snprintf(buf, sizeof(buf), "%02d:%02d", cfg->amb_from_min / 60, cfg->amb_from_min % 60);
    s_ta_amb_from = add_textarea(p, 380, 542, 110, buf, false);
    add_label(p, L()->night_to, 510, 518);
    snprintf(buf, sizeof(buf), "%02d:%02d", cfg->amb_to_min / 60, cfg->amb_to_min % 60);
    s_ta_amb_to = add_textarea(p, 510, 542, 110, buf, false);
    s_sw_map_light = add_switch(p, L()->map_light_lbl, 0, 596, cfg->map_light);
    s_sw_retro_map = add_switch(p, L()->retro_map_lbl, 380, 596, cfg->retro_map);
    s_sw_clk12 = add_switch(p, L()->clk12_lbl, 0, 648, cfg->clock_12h);
    s_slider_bright = NULL;
    s_sw_brightctl = NULL;
#ifndef APKFLIGHT
    if (waveshare_rgb_lcd_bl_dimmable()) {
        s_sw_brightctl = add_switch(p, L()->brightctl_lbl, 380, 642, cfg->brightness_ctl);
        add_label(p, L()->bright_lbl, 380, 694);
        s_slider_bright = lv_slider_create(p);
        lv_obj_set_size(s_slider_bright, UISX(280), UISY(16));
        lv_obj_set_pos(s_slider_bright, UISX(460), UISY(706));
        lv_slider_set_range(s_slider_bright, 5, 100);
        lv_slider_set_value(s_slider_bright, cfg->brightness, LV_ANIM_OFF);
        lv_obj_add_event_cb(s_slider_bright, brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }
#endif

#ifdef APKFLIGHT
    /* No web panel and no OTA in the app - updates arrive as a new APK. */
    int nety = 760;
#else
    add_section(p, L()->sec_webpanel, 760);
    s_ta_webpass = add_textarea(p, 0, 792, 360, cfg->web_pass, false);
    add_hint(p, L()->lbl_webpass, 0, 838, 360);

    add_section(p, L()->sec_updates, 872);
    lv_obj_t *sw_ota = add_switch(p, L()->ota_unlock, 0, 904, cfg->ota_enabled);
    lv_obj_add_event_cb(sw_ota, ota_unlock_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *hint = add_label(p, L()->ota_hint, 0, 950);
    lv_obj_set_style_text_font(hint, UIFONT(&font_pl_14, &font_pl_8), 0);
    int nety = 998;
#endif

    char netbuf[120] = "";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
#ifdef APKFLIGHT
        /* the app has no web panel, so no .local URL to advertise */
        snprintf(netbuf, sizeof(netbuf),
                 "panel: http://" IPSTR ":8080    esp32flight v%s",
                 IP2STR(&ip_info.ip), esp_app_get_description()->version);
#else
        snprintf(netbuf, sizeof(netbuf), "IP: " IPSTR "    http://esp32flight.local    v%s",
                 IP2STR(&ip_info.ip), esp_app_get_description()->version);
#endif
    } else {
        snprintf(netbuf, sizeof(netbuf), "IP: -    v%s", esp_app_get_description()->version);
    }
    add_label(p, netbuf, 0, nety);

    /* keyboard on the top layer */
    s_kb = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(s_kb, LV_HOR_RES, UISY(210));
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_ALL, NULL);
}
