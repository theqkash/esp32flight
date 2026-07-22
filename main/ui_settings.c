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
/* theme.h is pulled in above for the COL_* macros; THEME_COUNT comes with it */

#include "theme.h"

#define COL_BG     (app_theme()->bg)
#define COL_PANEL  (app_theme()->panel)
#define COL_ACCENT (app_theme()->accent)
#define COL_TEXT   (app_theme()->text)
#define COL_DIM    (app_theme()->dim)

#define MAX_SCAN_APS   15
#define MAX_GEO_RESULTS 6

static lv_obj_t *s_overlay;
static lv_obj_t *s_kb;
static lv_obj_t *s_ta_ssid, *s_ta_pass, *s_ta_city, *s_ta_lat, *s_ta_lon;
static lv_obj_t *s_dd_networks, *s_dd_cities;
static lv_obj_t *s_sw_auto, *s_sw_ground, *s_sw_private;
static lv_obj_t *s_slider_radius, *s_radius_label;
static lv_obj_t *s_dd_theme, *s_dd_lang;

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
    bool numeric = (ta == s_ta_lat || ta == s_ta_lon);
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
                        continue;   /* skip hidden and duplicate SSIDs */
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
    /* Picking a city implies fixed location. */
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

static void ota_unlock_cb(lv_event_t *e)
{
    /* applies immediately, never persisted: OTA re-locks on every restart */
    settings_get()->ota_enabled =
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
}

static void radius_cb(lv_event_t *e)
{
    int nm = lv_slider_get_value(s_slider_radius);
    char buf[48];
    snprintf(buf, sizeof(buf), "%d nm  (%d km)", nm, (int)(nm * 1.852));
    lv_label_set_text(s_radius_label, buf);
}

static void save_cb(lv_event_t *e)
{
    settings_t *cfg = settings_get();
    strlcpy(cfg->wifi_ssid, lv_textarea_get_text(s_ta_ssid), sizeof(cfg->wifi_ssid));
    strlcpy(cfg->wifi_pass, lv_textarea_get_text(s_ta_pass), sizeof(cfg->wifi_pass));
    cfg->use_fixed_loc = !lv_obj_has_state(s_sw_auto, LV_STATE_CHECKED);
    cfg->lat = atof(lv_textarea_get_text(s_ta_lat));
    cfg->lon = atof(lv_textarea_get_text(s_ta_lon));
    cfg->radius_nm = lv_slider_get_value(s_slider_radius);
    cfg->hide_ground = lv_obj_has_state(s_sw_ground, LV_STATE_CHECKED);
    cfg->hide_private = lv_obj_has_state(s_sw_private, LV_STATE_CHECKED);
    cfg->theme = lv_dropdown_get_selected(s_dd_theme);
    cfg->lang = lv_dropdown_get_selected(s_dd_lang);
    settings_save();

    lv_obj_t *msg = lv_label_create(s_overlay);
    lv_obj_set_style_text_color(msg, COL_ACCENT, 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_24, 0);
    lv_label_set_text(msg, L()->saved_restarting);
    lv_obj_center(msg);
    lv_refr_now(NULL);
    esp_restart();
}

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

void ui_settings_open(void)
{
    if (s_overlay != NULL) {
        return;
    }
    settings_t *cfg = settings_get();

    s_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_overlay, 800, 480);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, COL_BG, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 20, 0);
    lv_obj_set_scroll_dir(s_overlay, LV_DIR_VER);

    lv_obj_t *title = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, COL_ACCENT, 0);
    lv_label_set_text_fmt(title, LV_SYMBOL_SETTINGS " %s", L()->settings_title);
    lv_obj_set_pos(title, 0, 0);

    lv_obj_t *btn_close = add_button(s_overlay, 0, 0, 52, 40, LV_SYMBOL_CLOSE, close_cb, COL_PANEL);
    lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, 0, -4);

    /* Wi-Fi */
    add_label(s_overlay, L()->wifi_ssid, 0, 52);
    s_ta_ssid = add_textarea(s_overlay, 0, 74, 290, cfg->wifi_ssid, false);
    add_button(s_overlay, 300, 74, 56, 44, LV_SYMBOL_REFRESH, scan_click_cb, COL_PANEL);
    s_dd_networks = add_dropdown(s_overlay, 366, 74, 250, network_pick_cb);
    lv_dropdown_set_text(s_dd_networks, L()->dd_networks);

    add_label(s_overlay, L()->password, 0, 128);
    s_ta_pass = add_textarea(s_overlay, 0, 150, 380, cfg->wifi_pass, true);

    /* Location */
    add_label(s_overlay, L()->auto_location, 0, 208);
    s_sw_auto = lv_switch_create(s_overlay);
    lv_obj_set_pos(s_sw_auto, 170, 202);
    if (!cfg->use_fixed_loc) {
        lv_obj_add_state(s_sw_auto, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_sw_auto, auto_loc_cb, LV_EVENT_VALUE_CHANGED, NULL);

    add_label(s_overlay, L()->hide_ground, 280, 208);
    s_sw_ground = lv_switch_create(s_overlay);
    lv_obj_set_pos(s_sw_ground, 420, 202);
    if (cfg->hide_ground) {
        lv_obj_add_state(s_sw_ground, LV_STATE_CHECKED);
    }

    add_label(s_overlay, L()->airline_only, 545, 208);
    s_sw_private = lv_switch_create(s_overlay);
    lv_obj_set_pos(s_sw_private, 690, 202);
    if (cfg->hide_private) {
        lv_obj_add_state(s_sw_private, LV_STATE_CHECKED);
    }

    add_label(s_overlay, L()->city_search, 0, 248);
    s_ta_city = add_textarea(s_overlay, 0, 270, 290, "", false);
    add_button(s_overlay, 300, 270, 56, 44, LV_SYMBOL_GPS, city_search_cb, COL_PANEL);
    s_dd_cities = add_dropdown(s_overlay, 366, 270, 380, city_pick_cb);
    lv_dropdown_set_text(s_dd_cities, L()->dd_results);

    char coord[24];
    add_label(s_overlay, L()->latitude, 0, 324);
    snprintf(coord, sizeof(coord), "%.4f", cfg->lat);
    s_ta_lat = add_textarea(s_overlay, 0, 346, 200, coord, false);
    add_label(s_overlay, L()->longitude, 220, 324);
    snprintf(coord, sizeof(coord), "%.4f", cfg->lon);
    s_ta_lon = add_textarea(s_overlay, 220, 346, 200, coord, false);
    if (!cfg->use_fixed_loc) {
        lv_obj_add_state(s_ta_lat, LV_STATE_DISABLED);
        lv_obj_add_state(s_ta_lon, LV_STATE_DISABLED);
    }

    /* Radius */
    add_label(s_overlay, L()->search_radius, 0, 404);
    s_slider_radius = lv_slider_create(s_overlay);
    lv_obj_set_size(s_slider_radius, 420, 16);
    lv_obj_set_pos(s_slider_radius, 0, 436);
    lv_slider_set_range(s_slider_radius, 10, 250);
    lv_slider_set_value(s_slider_radius, cfg->radius_nm, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_slider_radius, radius_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_radius_label = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_radius_label, &font_pl_16, 0);
    lv_obj_set_style_text_color(s_radius_label, COL_TEXT, 0);
    lv_obj_set_pos(s_radius_label, 440, 432);
    char buf[48];
    snprintf(buf, sizeof(buf), "%d nm  (%d km)", cfg->radius_nm, (int)(cfg->radius_nm * 1.852));
    lv_label_set_text(s_radius_label, buf);

    /* Theme + language */
    add_label(s_overlay, L()->theme_lbl, 0, 482);
    s_dd_theme = add_dropdown(s_overlay, 90, 474, 150, NULL);
    lv_dropdown_set_options(s_dd_theme, theme_names_option_string());
    lv_dropdown_set_selected(s_dd_theme, cfg->theme < THEME_COUNT ? cfg->theme : 0);

    add_label(s_overlay, L()->language_lbl, 260, 482);
    s_dd_lang = add_dropdown(s_overlay, 340, 474, 150, NULL);
    lv_dropdown_set_options(s_dd_lang, "English\nPolski");
    lv_dropdown_set_selected(s_dd_lang, cfg->lang == 1 ? 1 : 0);

    /* Save */
    char save_txt[32];
    snprintf(save_txt, sizeof(save_txt), LV_SYMBOL_SAVE "  %s", L()->save);
    lv_obj_t *btn_save = add_button(s_overlay, 560, 470, 200, 52, save_txt, save_cb, COL_ACCENT);
    (void)btn_save;

    /* OTA unlock: instant, session-only, no save needed */
    add_label(s_overlay, L()->ota_unlock, 0, 548);
    lv_obj_t *sw_ota = lv_switch_create(s_overlay);
    lv_obj_set_pos(sw_ota, 420, 542);
    if (cfg->ota_enabled) {
        lv_obj_add_state(sw_ota, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw_ota, ota_unlock_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *hint = add_label(s_overlay, L()->ota_hint, 0, 584);
    lv_obj_set_style_text_font(hint, &font_pl_14, 0);

    /* Network info footer */
    char netbuf[80] = "";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        snprintf(netbuf, sizeof(netbuf), "IP: " IPSTR "    http://esp32flight.local", IP2STR(&ip_info.ip));
    } else {
        snprintf(netbuf, sizeof(netbuf), "IP: -");
    }
    lv_obj_t *netl = add_label(s_overlay, netbuf, 0, 646);
    lv_obj_set_style_text_color(netl, COL_DIM, 0);

    /* Keyboard on the top layer so it stays put while the form scrolls */
    s_kb = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(s_kb, 800, 210);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_ALL, NULL);
}
