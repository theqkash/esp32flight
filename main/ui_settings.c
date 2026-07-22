#include "ui_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fonts.h"
#include "geocode.h"
#include "lang.h"
#include "lvgl_port.h"
#include "settings.h"
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
static lv_obj_t *s_ta_watch, *s_ta_ntfy, *s_ta_mqtt, *s_ta_fa, *s_ta_webhook, *s_ta_ladsb;
static lv_obj_t *s_ta_night_from, *s_ta_night_to, *s_ta_amb_idle;
static lv_obj_t *s_dd_networks, *s_dd_cities, *s_dd_theme, *s_dd_lang;
static lv_obj_t *s_sw_auto, *s_sw_ground, *s_sw_private, *s_sw_cpa, *s_sw_night;
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
                    ta == s_ta_night_from || ta == s_ta_night_to);
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
    if (sscanf(txt, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
        return h * 60 + m;
    }
    return fallback;
}

static void save_cb(lv_event_t *e)
{
    settings_t *cfg = settings_get();
    strlcpy(cfg->wifi_ssid, lv_textarea_get_text(s_ta_ssid), sizeof(cfg->wifi_ssid));
    const char *pw = lv_textarea_get_text(s_ta_pass);
    if (pw[0] != '\0') {
        strlcpy(cfg->wifi_pass, pw, sizeof(cfg->wifi_pass));
    }
    cfg->use_fixed_loc = !lv_obj_has_state(s_sw_auto, LV_STATE_CHECKED);
    cfg->lat = atof(lv_textarea_get_text(s_ta_lat));
    cfg->lon = atof(lv_textarea_get_text(s_ta_lon));
    cfg->radius_nm = lv_slider_get_value(s_slider_radius);
    cfg->hide_ground = lv_obj_has_state(s_sw_ground, LV_STATE_CHECKED);
    cfg->hide_private = lv_obj_has_state(s_sw_private, LV_STATE_CHECKED);
    cfg->cpa_alerts = lv_obj_has_state(s_sw_cpa, LV_STATE_CHECKED);
    cfg->night_enabled = lv_obj_has_state(s_sw_night, LV_STATE_CHECKED);
    cfg->night_start_min = parse_hhmm(lv_textarea_get_text(s_ta_night_from),
                                      cfg->night_start_min);
    cfg->night_end_min = parse_hhmm(lv_textarea_get_text(s_ta_night_to),
                                    cfg->night_end_min);
    cfg->ambient_idle_min = atoi(lv_textarea_get_text(s_ta_amb_idle));
    strlcpy(cfg->watch_regs, lv_textarea_get_text(s_ta_watch), sizeof(cfg->watch_regs));
    strlcpy(cfg->ntfy_topic, lv_textarea_get_text(s_ta_ntfy), sizeof(cfg->ntfy_topic));
    strlcpy(cfg->mqtt_uri, lv_textarea_get_text(s_ta_mqtt), sizeof(cfg->mqtt_uri));
    strlcpy(cfg->fa_key, lv_textarea_get_text(s_ta_fa), sizeof(cfg->fa_key));
    strlcpy(cfg->webhook_url, lv_textarea_get_text(s_ta_webhook), sizeof(cfg->webhook_url));
    strlcpy(cfg->local_adsb, lv_textarea_get_text(s_ta_ladsb), sizeof(cfg->local_adsb));
    cfg->theme = lv_dropdown_get_selected(s_dd_theme);
    cfg->lang = lv_dropdown_get_selected(s_dd_lang);
    settings_save();

    lv_obj_t *msg = lv_label_create(s_overlay);
    lv_obj_set_style_text_color(msg, COL_ACCENT, 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_24, 0);
    lv_obj_set_style_bg_color(msg, COL_BG, 0);
    lv_obj_set_style_bg_opa(msg, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(msg, 20, 0);
    lv_label_set_text(msg, L()->saved_restarting);
    lv_obj_center(msg);
    lv_refr_now(NULL);
    esp_restart();
}

/* ---- widget helpers ---- */

static lv_obj_t *add_label(lv_obj_t *parent, const char *text, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &font_pl_16, 0);
    lv_obj_set_style_text_color(l, COL_DIM, 0);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *add_textarea(lv_obj_t *parent, int x, int y, int w, const char *value, bool password)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_pos(ta, x, y);
    lv_obj_set_size(ta, w, 44);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, password);
    lv_textarea_set_text(ta, value);
    lv_obj_set_style_bg_color(ta, COL_PANEL, 0);
    lv_obj_set_style_text_color(ta, COL_TEXT, 0);
    lv_obj_set_style_text_font(ta, &font_pl_16, 0);
    lv_obj_set_style_border_color(ta, COL_DIM, 0);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, ta_defocus_cb, LV_EVENT_DEFOCUSED, NULL);
    return ta;
}

static lv_obj_t *add_button(lv_obj_t *parent, int x, int y, int w, int h,
                            const char *text, lv_event_cb_t cb, lv_color_t bg)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_style_text_font(l, &font_pl_16, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return btn;
}

static lv_obj_t *add_dropdown(lv_obj_t *parent, int x, int y, int w, lv_event_cb_t cb)
{
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_obj_set_pos(dd, x, y);
    lv_obj_set_size(dd, w, 44);
    lv_dropdown_set_options(dd, "");
    lv_obj_set_style_bg_color(dd, COL_PANEL, 0);
    lv_obj_set_style_text_color(dd, COL_TEXT, 0);
    lv_obj_set_style_text_font(dd, &font_pl_16, 0);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_set_style_text_font(list, &font_pl_16, 0);
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
    lv_obj_set_pos(sw, x + 290, y);
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    return sw;
}

static lv_obj_t *tab_page(lv_obj_t *tv, const char *name)
{
    lv_obj_t *page = lv_tabview_add_tab(tv, name);
    lv_obj_set_style_pad_all(page, 14, 0);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    return page;
}

void ui_settings_open(void)
{
    if (s_overlay != NULL) {
        return;
    }
    settings_t *cfg = settings_get();
    char buf[48];

    s_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_overlay, 800, 480);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, COL_BG, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* header row: title + save + close */
    lv_obj_t *title = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(title, &font_pl_20, 0);
    lv_obj_set_style_text_color(title, COL_ACCENT, 0);
    lv_label_set_text_fmt(title, LV_SYMBOL_SETTINGS " %s", L()->settings_title);
    lv_obj_set_pos(title, 14, 12);

    snprintf(buf, sizeof(buf), LV_SYMBOL_SAVE "  %s", L()->save);
    add_button(s_overlay, 570, 6, 150, 38, buf, save_cb, COL_ACCENT);
    add_button(s_overlay, 734, 6, 54, 38, LV_SYMBOL_CLOSE, close_cb, COL_PANEL);

    lv_obj_t *tv = lv_tabview_create(s_overlay, LV_DIR_TOP, 44);
    lv_obj_set_size(tv, 800, 480 - 50);
    lv_obj_set_pos(tv, 0, 50);
    lv_obj_set_style_bg_color(tv, COL_BG, 0);
    lv_obj_t *bar = lv_tabview_get_tab_btns(tv);
    lv_obj_set_style_bg_color(bar, COL_PANEL, 0);
    lv_obj_set_style_text_color(bar, COL_TEXT, 0);
    lv_obj_set_style_text_font(bar, &font_pl_16, 0);

    /* --- Network --- */
    lv_obj_t *p = tab_page(tv, L()->tab_net);
    add_label(p, L()->wifi_ssid, 0, 0);
    s_ta_ssid = add_textarea(p, 0, 24, 290, cfg->wifi_ssid, false);
    add_button(p, 300, 24, 56, 44, LV_SYMBOL_REFRESH, scan_click_cb, COL_PANEL);
    s_dd_networks = add_dropdown(p, 366, 24, 250, network_pick_cb);
    lv_dropdown_set_text(s_dd_networks, L()->dd_networks);
    add_label(p, L()->password, 0, 84);
    s_ta_pass = add_textarea(p, 0, 108, 380, "", true);

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
    lv_obj_set_size(s_slider_radius, 420, 16);
    lv_obj_set_pos(s_slider_radius, 0, 258);
    lv_slider_set_range(s_slider_radius, 10, 250);
    lv_slider_set_value(s_slider_radius, cfg->radius_nm, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_slider_radius, radius_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_radius_label = add_label(p, "", 440, 254);
    lv_obj_set_style_text_color(s_radius_label, COL_TEXT, 0);
    snprintf(buf, sizeof(buf), "%d nm  (%d km)", cfg->radius_nm, (int)(cfg->radius_nm * 1.852));
    lv_label_set_text(s_radius_label, buf);

    /* --- Filters + alerts --- */
    p = tab_page(tv, L()->tab_filters);
    s_sw_ground = add_switch(p, L()->hide_ground, 0, 0, cfg->hide_ground);
    s_sw_private = add_switch(p, L()->airline_only, 380, 0, cfg->hide_private);
    s_sw_cpa = add_switch(p, L()->cpa_lbl, 0, 52, cfg->cpa_alerts);
    add_label(p, L()->lbl_watch, 0, 108);
    s_ta_watch = add_textarea(p, 0, 132, 740, cfg->watch_regs, false);
    s_sw_night = add_switch(p, L()->night_lbl, 0, 192, cfg->night_enabled);
    add_label(p, L()->night_from, 380, 186);
    snprintf(buf, sizeof(buf), "%02d:%02d", cfg->night_start_min / 60, cfg->night_start_min % 60);
    s_ta_night_from = add_textarea(p, 380, 210, 110, buf, false);
    add_label(p, L()->night_to, 510, 186);
    snprintf(buf, sizeof(buf), "%02d:%02d", cfg->night_end_min / 60, cfg->night_end_min % 60);
    s_ta_night_to = add_textarea(p, 510, 210, 110, buf, false);
    add_label(p, L()->amb_idle_lbl, 0, 250);
    snprintf(buf, sizeof(buf), "%d", cfg->ambient_idle_min);
    s_ta_amb_idle = add_textarea(p, 380, 244, 110, buf, false);

    /* --- Integrations --- */
    p = tab_page(tv, L()->tab_integr);
    add_label(p, L()->lbl_ntfy, 0, 0);
    s_ta_ntfy = add_textarea(p, 0, 24, 360, cfg->ntfy_topic, false);
    add_label(p, L()->lbl_mqtt, 380, 0);
    s_ta_mqtt = add_textarea(p, 380, 24, 360, cfg->mqtt_uri, false);
    add_label(p, L()->lbl_fa, 0, 84);
    s_ta_fa = add_textarea(p, 0, 108, 360, cfg->fa_key, false);
    add_label(p, L()->lbl_webhook, 380, 84);
    s_ta_webhook = add_textarea(p, 380, 108, 360, cfg->webhook_url, false);
    add_label(p, L()->lbl_ladsb, 0, 168);
    s_ta_ladsb = add_textarea(p, 0, 192, 740, cfg->local_adsb, false);

    /* --- System --- */
    p = tab_page(tv, L()->tab_system);
    add_label(p, L()->theme_lbl, 0, 0);
    s_dd_theme = add_dropdown(p, 0, 24, 180, NULL);
    lv_dropdown_set_options(s_dd_theme, theme_names_option_string());
    lv_dropdown_set_selected(s_dd_theme, cfg->theme < THEME_COUNT ? cfg->theme : 0);
    add_label(p, L()->language_lbl, 220, 0);
    s_dd_lang = add_dropdown(p, 220, 24, 180, NULL);
    lv_dropdown_set_options(s_dd_lang, "English\nPolski");
    lv_dropdown_set_selected(s_dd_lang, cfg->lang == 1 ? 1 : 0);

    lv_obj_t *sw_ota = add_switch(p, L()->ota_unlock, 0, 92, cfg->ota_enabled);
    lv_obj_add_event_cb(sw_ota, ota_unlock_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *hint = add_label(p, L()->ota_hint, 0, 138);
    lv_obj_set_style_text_font(hint, &font_pl_14, 0);

    char netbuf[80] = "";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        snprintf(netbuf, sizeof(netbuf), "IP: " IPSTR "    http://esp32flight.local", IP2STR(&ip_info.ip));
    } else {
        snprintf(netbuf, sizeof(netbuf), "IP: -");
    }
    add_label(p, netbuf, 0, 200);

    /* keyboard on the top layer */
    s_kb = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(s_kb, 800, 210);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_ALL, NULL);
}
