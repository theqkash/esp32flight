#include "ui.h"

#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include <ctype.h>
#include <math.h>
#include <time.h>
#include "airlines.h"
#include "faflight.h"
#include "fonts.h"
#include "lang.h"
#include "settings.h"
#include "ui_photo.h"
#include "geo_math.h"
#include "logos.h"
#include "flight_task.h"
#include "regcountry.h"
#include "routes.h"
#include "metar.h"
#include "dailystats.h"
#include "tilemap.h"
#include "trails.h"
#include "tz.h"
#include "ui_map.h"
#include "ui_settings.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "lvgl_port.h"
#include "esp_log.h"
#include "waveshare_rgb_lcd_port.h"

#define LIST_W        310
#define HEADER_H      48
#define MAX_SHOWN     20

#include "theme.h"

static const char *TAG = "ui";

LV_IMG_DECLARE(img_plane);

#define COL_BG        (app_theme()->bg)
#define COL_PANEL     (app_theme()->panel)
#define COL_ROW       (app_theme()->row)
#define COL_ROW_SEL   (app_theme()->row_sel)
#define COL_ACCENT    (app_theme()->accent)
#define COL_TEXT      (app_theme()->text)
#define COL_DIM       (app_theme()->dim)

typedef struct {
    aircraft_t   ac;
    route_info_t route;      /* route.callsign[0] == 0 -> no route snapshot */
    char         airline[NAME_LEN];
    char         iata[10];   /* commercial flight number, when known */
} shown_flight_t;

static shown_flight_t s_shown[MAX_SHOWN];
static int  s_shown_count;
static int  s_selected = -1;
static char s_selected_hex[ICAO_HEX_LEN];

static lv_obj_t *s_status_label;
static lv_obj_t *s_weather_label;
static lv_obj_t *s_list_panel;
static lv_obj_t *s_list_rows[MAX_SHOWN];

/* Right-panel view modes */
#define VIEW_DETAIL 0
#define VIEW_MAP    1
#define VIEW_RADAR  2
#define VIEW_STATS  3
#define VIEW_COUNT  4
#define EMB_MAP_W   490
#define EMB_MAP_H   245
#define CYCLE_MS    6000
static int        s_view_mode;
static lv_timer_t *s_cycle_timer;
static lv_obj_t *s_detail_panel;
static lv_obj_t *s_map_panel;
static lv_obj_t *s_emb_img;
static lv_obj_t *s_emb_line, *s_emb_orig, *s_emb_dest, *s_emb_plane, *s_emb_trail;
static lv_point_t s_emb_pts[33];
static lv_point_t s_emb_trail_pts[TRAIL_LEN];

/* Ambient map tile view (rendered by a worker task, CARTO tiles) */
static uint16_t     *s_emb_tiles;
static lv_img_dsc_t  s_emb_tiles_dsc;
static tile_view_t   s_emb_view;
static bool          s_emb_view_ok;
static char          s_emb_key[24];       /* key of the rendered view */
static char          s_emb_want_key[24];  /* key currently being rendered */
static volatile bool s_emb_busy;
static double        s_emb_bbox[4];       /* latmin, latmax, lonmin, lonmax */

/* Stats view */
/* Full-screen ambient screensaver (map + planes + clock + weather) */
static lv_obj_t *s_amb;
static lv_obj_t *s_amb_img;
static lv_obj_t *s_amb_planes[MAX_SHOWN];
static lv_obj_t *s_amb_lbls[MAX_SHOWN];
static lv_obj_t *s_amb_clock, *s_amb_wx;
static lv_obj_t *s_amb_ring, *s_amb_home;
static uint16_t *s_amb_tiles;
static lv_img_dsc_t s_amb_tiles_dsc;
static tile_view_t s_amb_view;
static bool s_amb_view_ok;
static volatile bool s_amb_busy;
static char s_amb_key[48];
static double s_amb_bbox[4];
static char s_weather_txt[96];
static bool s_bl_off;

static lv_obj_t *s_stats_panel;
static lv_obj_t *s_sv_vals[4];
static lv_obj_t *s_sv_chart;
static lv_chart_series_t *s_sv_series;
static lv_obj_t *s_sv_top[8];
static lv_obj_t *s_sv_metar;
static lv_obj_t *s_sv_days;
static app_stats_t s_stats_snap;
static lv_obj_t *s_mb_logo, *s_mb_callsign, *s_mb_type, *s_mb_route, *s_mb_stats, *s_mb_bar;
static lv_obj_t *s_mode_btn_label;
static lv_obj_t *s_clock_label;
static lv_obj_t *s_gear_label;

/* Radar view */
static lv_obj_t *s_radar_panel;
static lv_obj_t *s_radar_dots[MAX_SHOWN];
static lv_obj_t *s_radar_info;
static lv_obj_t *s_radar_range;
static lv_obj_t *s_radar_img;
static lv_obj_t *s_radar_rings[3];
static lv_obj_t *s_radar_home;
#define RADAR_CX 245
#define RADAR_CY 210
#define RADAR_R  185
#define RADAR_W  (800 - LIST_W)
#define RADAR_H  (480 - HEADER_H)

/* Radar map background (home-area tiles) */
static uint16_t     *s_radar_tiles;
static lv_img_dsc_t  s_radar_tiles_dsc;
static tile_view_t   s_radar_view;
static bool          s_radar_view_ok;
static volatile bool s_radar_busy;
static char          s_radar_key[48];
static char          s_radar_want[48];
static double        s_radar_bbox[4];
static double        s_home_lat, s_home_lon;
static bool          s_home_ok;

void ui_set_home(double lat, double lon)
{
    s_home_lat = lat;
    s_home_lon = lon;
    s_home_ok = true;
    ESP_LOGI("ui", "home set: %.4f, %.4f", lat, lon);
}

/* Detail widgets */
static lv_obj_t *s_logo_img;
static lv_obj_t *s_logo_fallback;
static lv_obj_t *s_callsign_label;
static lv_obj_t *s_airline_label;
static lv_obj_t *s_type_label;
static lv_obj_t *s_orig_code, *s_orig_city;
static lv_obj_t *s_dest_code, *s_dest_city;
static lv_obj_t *s_progress_bar;
static lv_obj_t *s_progress_label;
static lv_obj_t *s_look_label;
static lv_obj_t *s_stat_vals[6];
static lv_obj_t *s_detail_empty;
static lv_obj_t *s_detail_content;

static void render_detail(void);
static void render_list_selection(void);
static void render_map_panel(void);
static void render_radar_panel(void);
static void render_stats_panel(void);

static void render_right(void)
{
    switch (s_view_mode) {
    case VIEW_MAP:
        render_map_panel();
        break;
    case VIEW_RADAR:
        render_radar_panel();
        break;
    case VIEW_STATS:
        render_stats_panel();
        break;
    default:
        render_detail();
        break;
    }
}

/* Wall-clock time at the device's location: Open-Meteo home offset when
 * known, TZ env (Europe/Warsaw) otherwise */
static void home_localtime(time_t t, struct tm *tm)
{
    if (tz_home_known()) {
        time_t local = t + tz_home_offset();
        gmtime_r(&local, tm);
    } else {
        localtime_r(&t, tm);
    }
}

/* "14:32" local time at an airport, "" if timezone or clock unknown */
static void airport_local_time(const airport_t *ap, char *dst, size_t n)
{
    dst[0] = '\0';
    time_t now = time(NULL);
    if (!ap->tz_known || now < 1600000000) {
        return;
    }
    time_t local = now + ap->tz_offset_s;
    struct tm tm;
    gmtime_r(&local, &tm);
    snprintf(dst, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

/* "~24 min (14:52)" when speed and (optionally) wall clock are available */
static void format_eta(char *dst, size_t n, double remaining_km, float gs_kts)
{
    dst[0] = '\0';
    if (gs_kts < 50 || remaining_km <= 0) {
        return;
    }
    int mins = (int)(remaining_km / (gs_kts * 1.852) * 60.0 + 0.5);
    time_t now = time(NULL);
    if (now > 1600000000) {
        struct tm tm;
        home_localtime(now + (time_t)mins * 60, &tm);
        snprintf(dst, n, "~%d min (%02d:%02d)", mins, tm.tm_hour, tm.tm_min);
    } else {
        snprintf(dst, n, "~%d min", mins);
    }
}

static void settings_click_cb(lv_event_t *e)
{
    ui_settings_open();
}

/* Airline ICAO: prefer the route's, else derive from the callsign prefix so
 * logos show up before the route lookup completes. */
static const char *airline_code(const aircraft_t *ac, const route_info_t *rt)
{
    if (rt != NULL && rt->callsign[0] && rt->valid && rt->airline_icao[0]) {
        return rt->airline_icao;
    }
    static char prefix[4];
    if (isalpha((unsigned char)ac->callsign[0]) &&
        isalpha((unsigned char)ac->callsign[1]) &&
        isalpha((unsigned char)ac->callsign[2])) {
        prefix[0] = toupper((unsigned char)ac->callsign[0]);
        prefix[1] = toupper((unsigned char)ac->callsign[1]);
        prefix[2] = toupper((unsigned char)ac->callsign[2]);
        prefix[3] = '\0';
        return prefix;
    }
    return NULL;
}

static void map_click_cb(lv_event_t *e)
{
    if (s_selected >= 0 && s_selected < s_shown_count) {
        const route_info_t *rt = s_shown[s_selected].route.callsign[0]
                                     ? &s_shown[s_selected].route : NULL;
        ui_map_open(&s_shown[s_selected].ac, rt);
    }
}

static void photo_click_cb(lv_event_t *e)
{
    if (s_selected >= 0 && s_selected < s_shown_count) {
        const aircraft_t *ac = &s_shown[s_selected].ac;
        ui_photo_open(ac->hex, ac->callsign[0] ? ac->callsign : ac->hex);
    }
}

static void clock_timer_cb(lv_timer_t *t)
{
    time_t now = time(NULL);
    if (now < 1600000000) {
        return;
    }
    struct tm tm;
    home_localtime(now, &tm);
    lv_label_set_text_fmt(s_clock_label, "%02d:%02d", tm.tm_hour, tm.tm_min);
    if (s_amb != NULL && s_amb_clock != NULL) {
        lv_label_set_text_fmt(s_amb_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    }
}

static void row_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_shown_count) {
        s_selected = idx;
        strlcpy(s_selected_hex, s_shown[idx].ac.hex, sizeof(s_selected_hex));
        render_list_selection();
        if (s_view_mode != VIEW_DETAIL) {
            lv_timer_reset(s_cycle_timer);
        }
        render_right();
    }
}

static void cycle_timer_cb(lv_timer_t *t)
{
    if (s_shown_count == 0) {
        return;
    }
    s_selected = (s_selected + 1) % s_shown_count;
    strlcpy(s_selected_hex, s_shown[s_selected].ac.hex, sizeof(s_selected_hex));
    render_list_selection();
    render_right();
    /* keep the cycled aircraft visible in the list */
    lv_obj_scroll_to_view(s_list_rows[s_selected], LV_ANIM_ON);
}

static void mode_click_cb(lv_event_t *e)
{
    s_view_mode = (s_view_mode + 1) % VIEW_COUNT;

    lv_obj_add_flag(s_detail_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_map_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_radar_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_stats_panel, LV_OBJ_FLAG_HIDDEN);

    switch (s_view_mode) {
    case VIEW_MAP:
        lv_obj_clear_flag(s_map_panel, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_mode_btn_label, LV_SYMBOL_GPS);   /* next: radar */
        lv_timer_resume(s_cycle_timer);
        lv_timer_reset(s_cycle_timer);
        break;
    case VIEW_RADAR:
        lv_obj_clear_flag(s_radar_panel, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_mode_btn_label, LV_SYMBOL_BARS);  /* next: stats */
        lv_timer_resume(s_cycle_timer);
        lv_timer_reset(s_cycle_timer);
        break;
    case VIEW_STATS:
        lv_obj_clear_flag(s_stats_panel, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_mode_btn_label, LV_SYMBOL_LIST);  /* next: detail */
        lv_timer_pause(s_cycle_timer);
        break;
    default:
        lv_obj_clear_flag(s_detail_panel, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_mode_btn_label, LV_SYMBOL_LOOP);  /* next: auto map */
        lv_timer_pause(s_cycle_timer);
        break;
    }
    render_right();
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *make_panel(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_style_bg_color(p, COL_PANEL, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static void build_header(lv_obj_t *scr)
{
    lv_obj_t *hdr = make_panel(scr);
    lv_obj_set_size(hdr, 800, HEADER_H);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x0e1424), 0);

    s_weather_label = make_label(hdr, &font_pl_20, COL_ACCENT);
    lv_obj_set_width(s_weather_label, 340);
    lv_label_set_long_mode(s_weather_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_weather_label, LV_SYMBOL_GPS " esp32flight");
    lv_obj_align(s_weather_label, LV_ALIGN_LEFT_MID, 14, 0);

    s_clock_label = make_label(hdr, &font_pl_20, COL_TEXT);
    lv_obj_align(s_clock_label, LV_ALIGN_CENTER, 60, 0);
    lv_label_set_text(s_clock_label, "");

    s_status_label = make_label(hdr, &font_pl_14, COL_DIM);
    lv_obj_set_width(s_status_label, 205);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_status_label, LV_ALIGN_RIGHT_MID, -122, 0);
    lv_label_set_text(s_status_label, "...");

    lv_obj_t *gear = lv_btn_create(hdr);
    lv_obj_set_size(gear, 46, 36);
    lv_obj_align(gear, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_bg_color(gear, COL_ROW, 0);
    lv_obj_add_event_cb(gear, settings_click_cb, LV_EVENT_CLICKED, NULL);
    s_gear_label = make_label(gear, &lv_font_montserrat_16, COL_TEXT);
    lv_label_set_text(s_gear_label, LV_SYMBOL_SETTINGS);
    lv_obj_center(s_gear_label);

    lv_obj_t *mode = lv_btn_create(hdr);
    lv_obj_set_size(mode, 46, 36);
    lv_obj_align(mode, LV_ALIGN_RIGHT_MID, -62, 0);
    lv_obj_set_style_bg_color(mode, COL_ROW, 0);
    lv_obj_add_event_cb(mode, mode_click_cb, LV_EVENT_CLICKED, NULL);
    s_mode_btn_label = make_label(mode, &lv_font_montserrat_16, COL_TEXT);
    lv_label_set_text(s_mode_btn_label, LV_SYMBOL_LOOP);
    lv_obj_center(s_mode_btn_label);
}

static void build_list(lv_obj_t *scr)
{
    s_list_panel = make_panel(scr);
    lv_obj_set_size(s_list_panel, LIST_W, 480 - HEADER_H);
    lv_obj_set_pos(s_list_panel, 0, HEADER_H);
    lv_obj_set_style_bg_color(s_list_panel, COL_BG, 0);
    lv_obj_set_style_pad_all(s_list_panel, 8, 0);
    lv_obj_set_style_pad_row(s_list_panel, 6, 0);
    lv_obj_set_flex_flow(s_list_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_list_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list_panel, LV_DIR_VER);

    for (int i = 0; i < MAX_SHOWN; i++) {
        lv_obj_t *row = lv_obj_create(s_list_panel);
        lv_obj_set_size(row, LIST_W - 16 - 8, 64);
        lv_obj_set_style_bg_color(row, COL_ROW, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_all(row, 8, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *cs = make_label(row, &lv_font_montserrat_20, COL_TEXT);
        lv_obj_align(cs, LV_ALIGN_TOP_LEFT, 48, -2);
        lv_obj_t *type = make_label(row, &lv_font_montserrat_14, COL_ACCENT);
        lv_obj_align(type, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_t *info = make_label(row, &lv_font_montserrat_12, COL_DIM);
        lv_obj_align(info, LV_ALIGN_BOTTOM_LEFT, 48, 2);

        /* chip border and rounded corners are baked into the PNG assets */
        lv_obj_t *logo = lv_img_create(row);
        lv_img_set_pivot(logo, 0, 0);
        lv_img_set_zoom(logo, 256 * 40 / 90);
        lv_img_set_size_mode(logo, LV_IMG_SIZE_MODE_REAL);
        lv_obj_align(logo, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_flag(logo, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        s_list_rows[i] = row;
    }
}

static lv_obj_t *make_stat(lv_obj_t *parent, int col, int row, const char *name, lv_obj_t **val_out)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, 146, 56);
    lv_obj_set_pos(box, col * 156, row * 64);
    lv_obj_set_style_bg_color(box, COL_ROW, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_style_pad_all(box, 8, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *n = make_label(box, &font_pl_14, COL_DIM);
    lv_label_set_text(n, name);
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, -4);

    *val_out = make_label(box, &lv_font_montserrat_20, COL_TEXT);
    lv_obj_align(*val_out, LV_ALIGN_BOTTOM_LEFT, 0, 2);
    return box;
}

static void project_emb(double lat, double lon, lv_coord_t *x, lv_coord_t *y)
{
    if (s_emb_view_ok) {
        int xx, yy;
        tilemap_project(&s_emb_view, lat, lon, &xx, &yy);
        *x = (lv_coord_t)xx;
        *y = (lv_coord_t)yy;
        return;
    }
    *x = (lv_coord_t)((lon + 180.0) / 360.0 * EMB_MAP_W);
    *y = (lv_coord_t)((90.0 - lat) / 180.0 * EMB_MAP_H);
}

static void emb_tiles_task(void *arg)
{
    char key[24];
    double b[4];
    strlcpy(key, s_emb_want_key, sizeof(key));
    memcpy(b, s_emb_bbox, sizeof(b));

    if (s_emb_tiles == NULL) {
        s_emb_tiles = heap_caps_malloc(EMB_MAP_W * EMB_MAP_H * 2, MALLOC_CAP_SPIRAM);
    }
    tile_view_t view;
    bool ok = s_emb_tiles != NULL &&
              tilemap_render(s_emb_tiles, EMB_MAP_W, EMB_MAP_H,
                             b[0], b[1], b[2], b[3], &view);

    if (lvgl_port_lock(-1)) {
        if (ok && strcmp(key, s_emb_want_key) == 0) {
            s_emb_view = view;
            s_emb_view_ok = true;
            strlcpy(s_emb_key, key, sizeof(s_emb_key));
            s_emb_tiles_dsc.header.always_zero = 0;
            s_emb_tiles_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
            s_emb_tiles_dsc.header.w = EMB_MAP_W;
            s_emb_tiles_dsc.header.h = EMB_MAP_H;
            s_emb_tiles_dsc.data = (const uint8_t *)s_emb_tiles;
            s_emb_tiles_dsc.data_size = EMB_MAP_W * EMB_MAP_H * 2;
            lv_img_set_src(s_emb_img, &s_emb_tiles_dsc);
            lv_obj_invalidate(s_emb_img);
            if (s_view_mode == VIEW_MAP) {
                render_map_panel();
            }
        }
        lvgl_port_unlock();
    }
    s_emb_busy = false;
    vTaskDelete(NULL);
}

/* Kick off (or keep) the tile view for the currently selected flight. */
static void emb_tiles_want(const aircraft_t *ac, const route_info_t *rt)
{
    char key[24];
    if (rt != NULL) {
        snprintf(key, sizeof(key), "%s-%s", rt->origin.icao, rt->destination.icao);
    } else {
        snprintf(key, sizeof(key), "@%s", ac->hex);
    }
    if (strcmp(key, s_emb_key) == 0 || s_emb_busy) {
        return;
    }

    /* fall back to the bundled map while tiles load */
    s_emb_view_ok = false;
    const lv_img_dsc_t *fallback = ui_map_get_image_small();
    if (fallback != NULL) {
        lv_img_set_src(s_emb_img, fallback);
    }

    double latmin, latmax, lonmin, lonmax;
    if (rt != NULL) {
        latmin = latmax = rt->origin.lat;
        lonmin = lonmax = rt->origin.lon;
        double lats[2] = { rt->destination.lat, ac->has_pos ? ac->lat : rt->destination.lat };
        double lons[2] = { rt->destination.lon, ac->has_pos ? ac->lon : rt->destination.lon };
        for (int i = 0; i < 2; i++) {
            if (lats[i] < latmin) latmin = lats[i];
            if (lats[i] > latmax) latmax = lats[i];
            if (lons[i] < lonmin) lonmin = lons[i];
            if (lons[i] > lonmax) lonmax = lons[i];
        }
    } else if (ac->has_pos) {
        latmin = ac->lat - 1.0;
        latmax = ac->lat + 1.0;
        lonmin = ac->lon - 2.0;
        lonmax = ac->lon + 2.0;
    } else {
        return;
    }
    double mlat = (latmax - latmin) * 0.2 + 0.4;
    double mlon = (lonmax - lonmin) * 0.2 + 0.8;
    s_emb_bbox[0] = latmin - mlat;
    s_emb_bbox[1] = latmax + mlat;
    s_emb_bbox[2] = lonmin - mlon;
    s_emb_bbox[3] = lonmax + mlon;

    strlcpy(s_emb_want_key, key, sizeof(s_emb_want_key));
    s_emb_busy = true;
    if (xTaskCreatePinnedToCore(emb_tiles_task, "emb_tiles", 10240,
                                NULL, 3, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "emb tiles: task spawn failed (low memory)");
        s_emb_busy = false;
    }
}

/* Rotatable plane sprite, tinted by altitude at render time */
static lv_obj_t *plane_img(lv_obj_t *parent)
{
    lv_obj_t *im = lv_img_create(parent);
    lv_img_set_src(im, &img_plane);
    lv_obj_set_style_img_recolor_opa(im, LV_OPA_COVER, 0);
    lv_obj_clear_flag(im, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(im, LV_OBJ_FLAG_HIDDEN);
    return im;
}

static lv_obj_t *emb_marker(lv_obj_t *parent, int d, lv_color_t color)
{
    lv_obj_t *m = lv_obj_create(parent);
    lv_obj_set_size(m, d, d);
    lv_obj_set_style_radius(m, d / 2, 0);
    lv_obj_set_style_bg_color(m, color, 0);
    lv_obj_set_style_border_width(m, 1, 0);
    lv_obj_set_style_border_color(m, COL_BG, 0);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);
    return m;
}

static void build_map_panel(lv_obj_t *scr)
{
    s_map_panel = make_panel(scr);
    lv_obj_set_size(s_map_panel, 800 - LIST_W, 480 - HEADER_H);
    lv_obj_set_pos(s_map_panel, LIST_W, HEADER_H);
    lv_obj_add_flag(s_map_panel, LV_OBJ_FLAG_HIDDEN);

    /* Bundled 490x245 world map as the instant/offline base; a worker task
     * upgrades it to a CARTO tile view zoomed to the route. */
    s_emb_img = lv_img_create(s_map_panel);
    const lv_img_dsc_t *map = ui_map_get_image_small();
    if (map != NULL) {
        lv_img_set_src(s_emb_img, map);
    }
    lv_obj_set_pos(s_emb_img, 0, 0);

    s_emb_trail = lv_line_create(s_map_panel);
    lv_obj_set_style_line_width(s_emb_trail, 2, 0);
    lv_obj_set_style_line_color(s_emb_trail, lv_color_hex(0xffd166), 0);
    lv_obj_set_style_line_opa(s_emb_trail, LV_OPA_60, 0);
    lv_obj_add_flag(s_emb_trail, LV_OBJ_FLAG_HIDDEN);

    s_emb_line = lv_line_create(s_map_panel);
    lv_obj_set_style_line_width(s_emb_line, 2, 0);
    lv_obj_set_style_line_color(s_emb_line, COL_ACCENT, 0);
    lv_obj_set_style_line_rounded(s_emb_line, true, 0);
    lv_obj_add_flag(s_emb_line, LV_OBJ_FLAG_HIDDEN);

    s_emb_orig = emb_marker(s_map_panel, 10, lv_color_hex(0x39d98a));
    s_emb_dest = emb_marker(s_map_panel, 10, lv_color_hex(0xff6b6b));
    s_emb_plane = plane_img(s_map_panel);

    /* tap the map to open the full-screen route map */
    lv_obj_t *tap = lv_obj_create(s_map_panel);
    lv_obj_set_size(tap, EMB_MAP_W, EMB_MAP_H);
    lv_obj_set_pos(tap, 0, 0);
    lv_obj_set_style_bg_opa(tap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tap, 0, 0);
    lv_obj_clear_flag(tap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tap, map_click_cb, LV_EVENT_CLICKED, NULL);

    /* Info bubble under the map */
    lv_obj_t *bubble = lv_obj_create(s_map_panel);
    lv_obj_set_size(bubble, 800 - LIST_W - 20, 480 - HEADER_H - EMB_MAP_H - 18);
    lv_obj_set_pos(bubble, 10, EMB_MAP_H + 8);
    lv_obj_set_style_bg_color(bubble, COL_ROW, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_radius(bubble, 12, 0);
    lv_obj_set_style_pad_all(bubble, 12, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    s_mb_logo = lv_img_create(bubble);
    lv_img_set_pivot(s_mb_logo, 0, 0);
    lv_img_set_zoom(s_mb_logo, 256 * 48 / 90);
    lv_img_set_size_mode(s_mb_logo, LV_IMG_SIZE_MODE_REAL);
    lv_obj_set_pos(s_mb_logo, 0, 0);
    lv_obj_add_flag(s_mb_logo, LV_OBJ_FLAG_HIDDEN);

    s_mb_callsign = make_label(bubble, &lv_font_montserrat_24, COL_TEXT);
    lv_obj_set_pos(s_mb_callsign, 60, 0);
    s_mb_type = make_label(bubble, &font_pl_14, COL_ACCENT);
    lv_obj_set_pos(s_mb_type, 60, 30);

    s_mb_route = make_label(bubble, &font_pl_16, COL_TEXT);
    lv_obj_set_pos(s_mb_route, 0, 58);
    lv_obj_set_width(s_mb_route, 800 - LIST_W - 20 - 24);
    lv_label_set_long_mode(s_mb_route, LV_LABEL_LONG_DOT);

    s_mb_bar = lv_bar_create(bubble);
    lv_obj_set_size(s_mb_bar, 800 - LIST_W - 20 - 24, 8);
    lv_obj_set_pos(s_mb_bar, 0, 90);
    lv_bar_set_range(s_mb_bar, 0, 100);
    lv_obj_set_style_bg_color(s_mb_bar, COL_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_mb_bar, COL_ACCENT, LV_PART_INDICATOR);

    s_mb_stats = make_label(bubble, &font_pl_14, COL_DIM);
    lv_obj_set_pos(s_mb_stats, 0, 106);
}

static void render_map_panel(void)
{
    if (s_selected < 0 || s_selected >= s_shown_count) {
        lv_obj_add_flag(s_emb_line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_emb_orig, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_emb_dest, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_emb_plane, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_mb_logo, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_mb_callsign, "");
        lv_label_set_text(s_mb_type, "");
        lv_label_set_text(s_mb_route, L()->waiting_aircraft);
        lv_label_set_text(s_mb_stats, "");
        lv_bar_set_value(s_mb_bar, 0, LV_ANIM_OFF);
        return;
    }

    const aircraft_t *ac = &s_shown[s_selected].ac;
    const route_info_t *rt = s_shown[s_selected].route.callsign[0] && s_shown[s_selected].route.valid
                                 ? &s_shown[s_selected].route : NULL;
    lv_coord_t x, y;

    emb_tiles_want(ac, rt);

    /* breadcrumb trail of the selected aircraft */
    float tlat[TRAIL_LEN], tlon[TRAIL_LEN];
    int tn = trails_get(ac->hex, tlat, tlon, TRAIL_LEN);
    if (tn >= 2) {
        for (int k = 0; k < tn; k++) {
            project_emb(tlat[k], tlon[k], &x, &y);
            s_emb_trail_pts[k].x = x;
            s_emb_trail_pts[k].y = y;
        }
        lv_line_set_points(s_emb_trail, s_emb_trail_pts, tn);
        lv_obj_clear_flag(s_emb_trail, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_emb_trail, LV_OBJ_FLAG_HIDDEN);
    }

    if (rt != NULL) {
        for (int i = 0; i < 33; i++) {
            double lat, lon;
            geo_gc_point(rt->origin.lat, rt->origin.lon,
                         rt->destination.lat, rt->destination.lon,
                         (double)i / 32.0, &lat, &lon);
            project_emb(lat, lon, &x, &y);
            s_emb_pts[i].x = x;
            s_emb_pts[i].y = y;
        }
        lv_line_set_points(s_emb_line, s_emb_pts, 33);
        lv_obj_clear_flag(s_emb_line, LV_OBJ_FLAG_HIDDEN);

        project_emb(rt->origin.lat, rt->origin.lon, &x, &y);
        lv_obj_set_pos(s_emb_orig, x - 5, y - 5);
        lv_obj_clear_flag(s_emb_orig, LV_OBJ_FLAG_HIDDEN);
        project_emb(rt->destination.lat, rt->destination.lon, &x, &y);
        lv_obj_set_pos(s_emb_dest, x - 5, y - 5);
        lv_obj_clear_flag(s_emb_dest, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_emb_line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_emb_orig, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_emb_dest, LV_OBJ_FLAG_HIDDEN);
    }

    if (ac->has_pos) {
        project_emb(ac->lat, ac->lon, &x, &y);
        lv_obj_set_pos(s_emb_plane, x - 14, y - 14);
        lv_img_set_angle(s_emb_plane, (int)(ac->track_deg * 10));
        lv_obj_set_style_img_recolor(s_emb_plane,
                                     alt_color(ac->alt_baro_ft, ac->on_ground), 0);
        lv_obj_clear_flag(s_emb_plane, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_emb_plane, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(s_mb_callsign, ac->callsign[0] ? ac->callsign : ac->hex);
    const char *airline = s_shown[s_selected].airline;
    if (airline[0]) {
        lv_label_set_text_fmt(s_mb_type, "%s  -  %s", airline,
                              ac->type_desc[0] ? ac->type_desc : ac->type_icao);
    } else {
        lv_label_set_text(s_mb_type, ac->type_desc[0] ? ac->type_desc : ac->type_icao);
    }

    const char *code = airline_code(ac, rt);
    const lv_img_dsc_t *logo = code ? logos_get(code) : NULL;
    if (logo != NULL) {
        lv_img_set_src(s_mb_logo, logo);
        lv_obj_clear_flag(s_mb_logo, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_mb_logo, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[128];
    if (rt != NULL) {
        double prog = geo_progress(rt->origin.lat, rt->origin.lon,
                                   rt->destination.lat, rt->destination.lon,
                                   ac->lat, ac->lon);
        snprintf(buf, sizeof(buf), "%s (%s) " LV_SYMBOL_RIGHT " %s (%s)",
                 rt->origin.city[0] ? rt->origin.city : rt->origin.icao,
                 rt->origin.iata[0] ? rt->origin.iata : rt->origin.icao,
                 rt->destination.city[0] ? rt->destination.city : rt->destination.icao,
                 rt->destination.iata[0] ? rt->destination.iata : rt->destination.icao);
        lv_label_set_text(s_mb_route, buf);
        lv_bar_set_value(s_mb_bar, (int)(prog * 100.0), LV_ANIM_OFF);
        double remaining = geo_haversine_km(ac->lat, ac->lon,
                                            rt->destination.lat, rt->destination.lon);
        char eta[40];
        format_eta(eta, sizeof(eta), remaining, ac->gs_kts);
        snprintf(buf, sizeof(buf), "%d ft   %.0f kt   %d%%, %.0f km to go   %s",
                 ac->alt_baro_ft, (double)ac->gs_kts,
                 (int)(prog * 100.0), remaining, eta);
        lv_label_set_text(s_mb_stats, buf);
    } else {
        lv_label_set_text(s_mb_route, L()->route_unknown);
        lv_bar_set_value(s_mb_bar, 0, LV_ANIM_OFF);
        snprintf(buf, sizeof(buf), "%d ft   %.0f kt   %.1f km away",
                 ac->alt_baro_ft, (double)ac->gs_kts,
                 ac->dist_nm >= 0 ? ac->dist_nm * 1.852 : 0.0);
        lv_label_set_text(s_mb_stats, buf);
    }
}

static void radar_tiles_task(void *arg)
{
    char key[48];
    double b[4];
    strlcpy(key, s_radar_want, sizeof(key));
    memcpy(b, s_radar_bbox, sizeof(b));

    if (s_radar_tiles == NULL) {
        s_radar_tiles = heap_caps_malloc(RADAR_W * RADAR_H * 2, MALLOC_CAP_SPIRAM);
    }
    tile_view_t view;
    bool ok = s_radar_tiles != NULL &&
              tilemap_render(s_radar_tiles, RADAR_W, RADAR_H,
                             b[0], b[1], b[2], b[3], &view);

    if (lvgl_port_lock(-1)) {
        if (ok && strcmp(key, s_radar_want) == 0) {
            s_radar_view = view;
            s_radar_view_ok = true;
            strlcpy(s_radar_key, key, sizeof(s_radar_key));
            s_radar_tiles_dsc.header.always_zero = 0;
            s_radar_tiles_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
            s_radar_tiles_dsc.header.w = RADAR_W;
            s_radar_tiles_dsc.header.h = RADAR_H;
            s_radar_tiles_dsc.data = (const uint8_t *)s_radar_tiles;
            s_radar_tiles_dsc.data_size = RADAR_W * RADAR_H * 2;
            lv_img_set_src(s_radar_img, &s_radar_tiles_dsc);
            lv_obj_invalidate(s_radar_img);
            if (s_view_mode == VIEW_RADAR) {
                render_radar_panel();
            }
        }
        lvgl_port_unlock();
    }
    s_radar_busy = false;
    vTaskDelete(NULL);
}

static void radar_tiles_want(void)
{
    if (!s_home_ok || s_radar_busy) {
        return;
    }
    int radius_nm = settings_get()->radius_nm;
    char key[48];
    snprintf(key, sizeof(key), "%.3f,%.3f,%d", s_home_lat, s_home_lon, radius_nm);
    if (strcmp(key, s_radar_key) == 0) {
        return;
    }
    double rkm = radius_nm * 1.852;
    double dlat = rkm / 111.0;
    double dlon = rkm / (111.0 * cos(s_home_lat * M_PI / 180.0));
    s_radar_bbox[0] = s_home_lat - dlat;
    s_radar_bbox[1] = s_home_lat + dlat;
    s_radar_bbox[2] = s_home_lon - dlon;
    s_radar_bbox[3] = s_home_lon + dlon;
    LV_LOG_USER("radar bbox %.3f..%.3f / %.3f..%.3f (home %.3f,%.3f r=%d)",
                s_radar_bbox[0], s_radar_bbox[1], s_radar_bbox[2], s_radar_bbox[3],
                s_home_lat, s_home_lon, radius_nm);
    strlcpy(s_radar_want, key, sizeof(s_radar_want));
    s_radar_busy = true;
    ESP_LOGI(TAG, "radar tiles: spawn for %s", key);
    if (xTaskCreatePinnedToCore(radar_tiles_task, "radar_tiles", 10240,
                                NULL, 3, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "radar tiles: task spawn failed (low memory)");
        s_radar_busy = false;
    }
}

static void build_radar_panel(lv_obj_t *scr)
{
    s_radar_panel = make_panel(scr);
    lv_obj_set_size(s_radar_panel, 800 - LIST_W, 480 - HEADER_H);
    lv_obj_set_pos(s_radar_panel, LIST_W, HEADER_H);
    lv_obj_set_style_bg_color(s_radar_panel, COL_BG, 0);
    lv_obj_add_flag(s_radar_panel, LV_OBJ_FLAG_HIDDEN);

    /* home-area tile map in the background (falls back to plain rings) */
    s_radar_img = lv_img_create(s_radar_panel);
    lv_obj_set_pos(s_radar_img, 0, 0);
    lv_obj_add_flag(s_radar_img, LV_OBJ_FLAG_HIDDEN);

    for (int i = 1; i <= 3; i++) {
        int r = RADAR_R * i / 3;
        lv_obj_t *ring = lv_obj_create(s_radar_panel);
        lv_obj_set_size(ring, r * 2, r * 2);
        lv_obj_set_pos(ring, RADAR_CX - r, RADAR_CY - r);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_set_style_border_color(ring, COL_ROW_SEL, 0);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        s_radar_rings[i - 1] = ring;
    }

    lv_obj_t *home = lv_obj_create(s_radar_panel);
    lv_obj_set_size(home, 10, 10);
    lv_obj_set_pos(home, RADAR_CX - 5, RADAR_CY - 5);
    lv_obj_set_style_radius(home, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(home, COL_ACCENT, 0);
    lv_obj_set_style_border_width(home, 0, 0);
    lv_obj_clear_flag(home, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_radar_home = home;

    for (int i = 0; i < MAX_SHOWN; i++) {
        s_radar_dots[i] = plane_img(s_radar_panel);
    }

    s_radar_info = make_label(s_radar_panel, &font_pl_14, COL_TEXT);
    lv_obj_set_style_bg_color(s_radar_info, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(s_radar_info, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(s_radar_info, 6, 0);
    lv_obj_set_style_radius(s_radar_info, 6, 0);
    lv_label_set_text(s_radar_info, "");

    s_radar_range = make_label(s_radar_panel, &font_pl_14, COL_DIM);
    lv_obj_align(s_radar_range, LV_ALIGN_BOTTOM_RIGHT, -10, -6);
    lv_label_set_text(s_radar_range, "");
}

static void render_radar_panel(void)
{
    radar_tiles_want();

    int radius_nm = settings_get()->radius_nm;
    bool map_mode = s_radar_view_ok;
    int hx = RADAR_CX, hy = RADAR_CY;
    int rpx = RADAR_R;

    if (map_mode) {
        tilemap_project(&s_radar_view, s_home_lat, s_home_lon, &hx, &hy);
        int ex, ey;
        tilemap_project(&s_radar_view, s_home_lat, s_radar_bbox[3], &ex, &ey);
        rpx = ex - hx;
        if (rpx < 20) {
            rpx = 20;
        }
        lv_obj_clear_flag(s_radar_img, LV_OBJ_FLAG_HIDDEN);
        /* single range ring on the map */
        lv_obj_clear_flag(s_radar_rings[2], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(s_radar_rings[2], rpx * 2, rpx * 2);
        lv_obj_set_pos(s_radar_rings[2], hx - rpx, hy - rpx);
        lv_obj_add_flag(s_radar_rings[0], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_radar_rings[1], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_radar_home, hx - 5, hy - 5);
        lv_label_set_text_fmt(s_radar_range, "%d km", (int)(radius_nm * 1.852));
    } else {
        lv_obj_add_flag(s_radar_img, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 3; i++) {
            int r = RADAR_R * (i + 1) / 3;
            lv_obj_clear_flag(s_radar_rings[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(s_radar_rings[i], r * 2, r * 2);
            lv_obj_set_pos(s_radar_rings[i], RADAR_CX - r, RADAR_CY - r);
        }
        lv_obj_set_pos(s_radar_home, RADAR_CX - 5, RADAR_CY - 5);
        lv_label_set_text_fmt(s_radar_range, L()->ring_fmt, (int)(radius_nm * 1.852 / 3));
    }

    for (int i = 0; i < MAX_SHOWN; i++) {
        if (i >= s_shown_count || s_shown[i].ac.dist_nm < 0) {
            lv_obj_add_flag(s_radar_dots[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const aircraft_t *ac = &s_shown[i].ac;
        int x, y;
        if (map_mode && ac->has_pos) {
            tilemap_project(&s_radar_view, ac->lat, ac->lon, &x, &y);
            if (x < -14 || x > RADAR_W + 14 || y < -14 || y > RADAR_H + 14) {
                lv_obj_add_flag(s_radar_dots[i], LV_OBJ_FLAG_HIDDEN);
                continue;
            }
        } else {
            float frac = ac->dist_nm / (float)radius_nm;
            if (frac > 1.0f) {
                frac = 1.0f;
            }
            float rad = ac->dir_deg * (float)M_PI / 180.0f;
            x = RADAR_CX + (int)(sinf(rad) * frac * RADAR_R);
            y = RADAR_CY - (int)(cosf(rad) * frac * RADAR_R);
        }
        lv_obj_set_pos(s_radar_dots[i], x - 14, y - 14);
        lv_img_set_angle(s_radar_dots[i], (int)(ac->track_deg * 10));
        lv_img_set_zoom(s_radar_dots[i], i == s_selected ? 384 : 232);
        lv_obj_set_style_img_recolor(s_radar_dots[i],
                                     alt_color(ac->alt_baro_ft, ac->on_ground), 0);
        lv_obj_clear_flag(s_radar_dots[i], LV_OBJ_FLAG_HIDDEN);

        if (i == s_selected) {
            char info[96];
            if (ac->on_ground) {
                snprintf(info, sizeof(info), "%s\n%s  %.1f km",
                         ac->callsign[0] ? ac->callsign : ac->hex,
                         L()->ground, ac->dist_nm * 1.852);
            } else {
                snprintf(info, sizeof(info), "%s\n%d ft  %.0f kt  %.1f km",
                         ac->callsign[0] ? ac->callsign : ac->hex,
                         ac->alt_baro_ft, (double)ac->gs_kts, ac->dist_nm * 1.852);
            }
            lv_label_set_text(s_radar_info, info);
            int lx = x + 12, ly = y - 10;
            if (lx > 800 - LIST_W - 150) {
                lx = x - 150;
            }
            if (ly < 0) {
                ly = 0;
            }
            if (ly > 480 - HEADER_H - 60) {
                ly = 480 - HEADER_H - 60;
            }
            lv_obj_set_pos(s_radar_info, lx, ly);
        }
    }
    if (s_selected < 0 || s_selected >= s_shown_count) {
        lv_label_set_text(s_radar_info, "");
    }
}

/* ---------- full-screen ambient screensaver ---------- */

static void render_ambient(void);

static void amb_tiles_task(void *arg)
{
    char key[48];
    double b[4];
    strlcpy(key, s_amb_key, sizeof(key));
    memcpy(b, s_amb_bbox, sizeof(b));

    if (s_amb_tiles == NULL) {
        s_amb_tiles = heap_caps_malloc(800 * 480 * 2, MALLOC_CAP_SPIRAM);
    }
    tile_view_t view;
    bool ok = s_amb_tiles != NULL &&
              tilemap_render(s_amb_tiles, 800, 480, b[0], b[1], b[2], b[3], &view);

    if (lvgl_port_lock(-1)) {
        if (ok && s_amb != NULL) {
            s_amb_view = view;
            s_amb_view_ok = true;
            s_amb_tiles_dsc.header.always_zero = 0;
            s_amb_tiles_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
            s_amb_tiles_dsc.header.w = 800;
            s_amb_tiles_dsc.header.h = 480;
            s_amb_tiles_dsc.data = (const uint8_t *)s_amb_tiles;
            s_amb_tiles_dsc.data_size = 800 * 480 * 2;
            lv_img_set_src(s_amb_img, &s_amb_tiles_dsc);
            render_ambient();
        }
        lvgl_port_unlock();
    }
    s_amb_busy = false;
    vTaskDelete(NULL);
}

static void amb_proj(double lat, double lon, lv_coord_t *x, lv_coord_t *y)
{
    if (s_amb_view_ok) {
        int xx, yy;
        tilemap_project(&s_amb_view, lat, lon, &xx, &yy);
        *x = (lv_coord_t)xx;
        *y = (lv_coord_t)yy;
        return;
    }
    *x = (lv_coord_t)((lon + 180.0) / 360.0 * 800);
    *y = (lv_coord_t)(40 + (90.0 - lat) / 180.0 * 400);
}

static void render_ambient(void)
{
    if (s_amb == NULL) {
        return;
    }
    /* observation circle: everything outside it is simply not queried */
    if (s_amb_ring != NULL && s_amb_view_ok && s_home_ok) {
        lv_coord_t hx, hy, ex, ey;
        amb_proj(s_home_lat, s_home_lon, &hx, &hy);
        double rkm = settings_get()->radius_nm * 1.852;
        amb_proj(s_home_lat + rkm / 111.0, s_home_lon, &ex, &ey);
        lv_coord_t r = hy - ey;
        if (r > 10) {
            lv_obj_set_size(s_amb_ring, r * 2, r * 2);
            lv_obj_set_pos(s_amb_ring, hx - r, hy - r);
            lv_obj_clear_flag(s_amb_ring, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_amb_home, hx - 4, hy - 4);
            lv_obj_clear_flag(s_amb_home, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < MAX_SHOWN; i++) {
        if (i >= s_shown_count || !s_shown[i].ac.has_pos) {
            lv_obj_add_flag(s_amb_planes[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_amb_lbls[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const aircraft_t *ac = &s_shown[i].ac;
        lv_coord_t x, y;
        amb_proj(ac->lat, ac->lon, &x, &y);
        if (x < -14 || x > 814 || y < -14 || y > 494) {
            lv_obj_add_flag(s_amb_planes[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_amb_lbls[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_set_pos(s_amb_planes[i], x - 14, y - 14);
        lv_img_set_angle(s_amb_planes[i], (int)(ac->track_deg * 10));
        lv_obj_set_style_img_recolor(s_amb_planes[i],
                                     alt_color(ac->alt_baro_ft, ac->on_ground), 0);
        lv_obj_clear_flag(s_amb_planes[i], LV_OBJ_FLAG_HIDDEN);

        const route_info_t *rt = s_shown[i].route.callsign[0] && s_shown[i].route.valid
                                     ? &s_shown[i].route : NULL;
        char txt[64];
        if (rt != NULL) {
            snprintf(txt, sizeof(txt), "%s\n%s " LV_SYMBOL_RIGHT " %s",
                     ac->callsign[0] ? ac->callsign : ac->hex,
                     rt->origin.iata[0] ? rt->origin.iata : rt->origin.icao,
                     rt->destination.iata[0] ? rt->destination.iata : rt->destination.icao);
        } else {
            snprintf(txt, sizeof(txt), "%s",
                     ac->callsign[0] ? ac->callsign : ac->hex);
        }
        lv_label_set_text(s_amb_lbls[i], txt);
        int lx = x + 16, ly = y - 8;
        if (lx > 690) {
            lx = x - 110;
        }
        if (ly < 0) {
            ly = 0;
        }
        if (ly > 440) {
            ly = 440;
        }
        lv_obj_set_pos(s_amb_lbls[i], lx, ly);
        lv_obj_clear_flag(s_amb_lbls[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void amb_close(void)
{
    if (s_amb != NULL) {
        lv_obj_del(s_amb);
        s_amb = NULL;
        s_amb_view_ok = false;
        s_amb_key[0] = '\0';
    }
}

static void amb_click_cb(lv_event_t *e)
{
    if (s_bl_off) {
        waveshare_rgb_lcd_bl_on();
        s_bl_off = false;
        return;     /* first tap only wakes the screen */
    }
    amb_close();
}

static void amb_show(void)
{
    if (s_amb != NULL) {
        return;
    }
    s_amb = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_amb, 800, 480);
    lv_obj_set_pos(s_amb, 0, 0);
    lv_obj_set_style_bg_color(s_amb, lv_color_hex(0x06090f), 0);
    lv_obj_set_style_border_width(s_amb, 0, 0);
    lv_obj_set_style_radius(s_amb, 0, 0);
    lv_obj_set_style_pad_all(s_amb, 0, 0);
    lv_obj_clear_flag(s_amb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_amb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_amb, amb_click_cb, LV_EVENT_CLICKED, NULL);

    s_amb_img = lv_img_create(s_amb);
    lv_obj_set_pos(s_amb_img, 0, 0);
    const lv_img_dsc_t *fallback = ui_map_get_image();
    if (fallback != NULL && !s_amb_view_ok) {
        lv_img_set_src(s_amb_img, fallback);
        lv_obj_set_pos(s_amb_img, 0, 40);
    }

    s_amb_ring = lv_obj_create(s_amb);
    lv_obj_set_style_bg_opa(s_amb_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_amb_ring, 2, 0);
    lv_obj_set_style_border_color(s_amb_ring, COL_ACCENT, 0);
    lv_obj_set_style_border_opa(s_amb_ring, LV_OPA_60, 0);
    lv_obj_set_style_radius(s_amb_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(s_amb_ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_amb_ring, LV_OBJ_FLAG_HIDDEN);

    s_amb_home = lv_obj_create(s_amb);
    lv_obj_set_size(s_amb_home, 8, 8);
    lv_obj_set_style_bg_color(s_amb_home, COL_ACCENT, 0);
    lv_obj_set_style_border_width(s_amb_home, 0, 0);
    lv_obj_set_style_radius(s_amb_home, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(s_amb_home, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_amb_home, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < MAX_SHOWN; i++) {
        s_amb_planes[i] = plane_img(s_amb);
        s_amb_lbls[i] = make_label(s_amb, &font_pl_14, COL_TEXT);
        lv_obj_set_style_bg_color(s_amb_lbls[i], lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_amb_lbls[i], LV_OPA_50, 0);
        lv_obj_set_style_pad_hor(s_amb_lbls[i], 4, 0);
        lv_obj_add_flag(s_amb_lbls[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_amb_clock = make_label(s_amb, &lv_font_montserrat_32, lv_color_hex(0xffffff));
    lv_obj_set_style_bg_color(s_amb_clock, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_amb_clock, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(s_amb_clock, 8, 0);
    lv_obj_align(s_amb_clock, LV_ALIGN_TOP_LEFT, 10, 8);
    lv_label_set_text(s_amb_clock, "");

    s_amb_wx = make_label(s_amb, &font_pl_16, lv_color_hex(0xdddddd));
    lv_obj_set_style_bg_color(s_amb_wx, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_amb_wx, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(s_amb_wx, 6, 0);
    lv_obj_align(s_amb_wx, LV_ALIGN_TOP_RIGHT, -10, 12);
    lv_label_set_text(s_amb_wx, s_weather_txt);

    /* home-area tiles for the whole screen */
    if (s_home_ok && !s_amb_busy) {
        int radius_nm = settings_get()->radius_nm;
        double rkm = radius_nm * 1.852;
        double dlat = rkm / 111.0;
        double dlon = rkm / (111.0 * cos(s_home_lat * M_PI / 180.0));
        s_amb_bbox[0] = s_home_lat - dlat;
        s_amb_bbox[1] = s_home_lat + dlat;
        s_amb_bbox[2] = s_home_lon - dlon;
        s_amb_bbox[3] = s_home_lon + dlon;
        snprintf(s_amb_key, sizeof(s_amb_key), "amb");
        s_amb_busy = true;
        if (xTaskCreatePinnedToCore(amb_tiles_task, "amb_tiles", 10240,
                                    NULL, 3, NULL, 0) != pdPASS) {
            s_amb_busy = false;
        }
    }
    render_ambient();
}

/* idle watcher: screensaver + night backlight */
static void idle_timer_cb(lv_timer_t *t)
{
    const settings_t *cfg = settings_get();
    uint32_t idle_ms = lv_disp_get_inactive_time(NULL);

    if (cfg->ambient_idle_min > 0 && s_amb == NULL &&
        idle_ms > (uint32_t)cfg->ambient_idle_min * 60000U) {
        amb_show();
    }

    if (cfg->night_enabled && s_amb != NULL) {
        time_t now = time(NULL);
        if (now > 1600000000) {
            time_t l = now + (tz_home_known() ? tz_home_offset() : 0);
            struct tm tm;
            gmtime_r(&l, &tm);
            int m = tm.tm_hour * 60 + tm.tm_min;
            int a = cfg->night_start_min, b = cfg->night_end_min;
            bool night = a <= b ? (m >= a && m < b) : (m >= a || m < b);
            if (night && !s_bl_off &&
                idle_ms > (uint32_t)(cfg->ambient_idle_min + 5) * 60000U) {
                waveshare_rgb_lcd_bl_off();
                s_bl_off = true;
            } else if (!night && s_bl_off) {
                waveshare_rgb_lcd_bl_on();
                s_bl_off = false;
            }
        }
    }
}

static void build_stats_panel(lv_obj_t *scr)
{
    s_stats_panel = make_panel(scr);
    lv_obj_set_size(s_stats_panel, 800 - LIST_W, 480 - HEADER_H);
    lv_obj_set_pos(s_stats_panel, LIST_W, HEADER_H);
    lv_obj_set_style_pad_all(s_stats_panel, 16, 0);
    lv_obj_add_flag(s_stats_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = make_label(s_stats_panel, &font_pl_20, COL_ACCENT);
    lv_label_set_text(title, L()->stats_title);
    lv_obj_set_pos(title, 0, 0);

    static const int tile_w = 110;
    const char *names[4] = { L()->st_unique, L()->st_highest, L()->st_fastest, L()->st_farthest };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *box = lv_obj_create(s_stats_panel);
        lv_obj_set_size(box, tile_w, 62);
        lv_obj_set_pos(box, i * (tile_w + 6), 36);
        lv_obj_set_style_bg_color(box, COL_ROW, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_radius(box, 8, 0);
        lv_obj_set_style_pad_all(box, 8, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *n = make_label(box, &font_pl_14, COL_DIM);
        lv_label_set_text(n, names[i]);
        lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
        lv_obj_set_width(n, tile_w - 16);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, -4);
        s_sv_vals[i] = make_label(box, &font_pl_16, COL_TEXT);
        lv_obj_align(s_sv_vals[i], LV_ALIGN_BOTTOM_LEFT, 0, 4);
    }

    lv_obj_t *ch = make_label(s_stats_panel, &font_pl_14, COL_DIM);
    lv_label_set_text(ch, L()->st_hourly);
    lv_obj_set_pos(ch, 0, 112);

    s_sv_chart = lv_chart_create(s_stats_panel);
    lv_obj_set_size(s_sv_chart, 458, 120);
    lv_obj_set_pos(s_sv_chart, 0, 134);
    lv_chart_set_type(s_sv_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(s_sv_chart, 24);
    lv_chart_set_div_line_count(s_sv_chart, 3, 0);
    lv_obj_set_style_bg_color(s_sv_chart, COL_ROW, 0);
    lv_obj_set_style_border_width(s_sv_chart, 0, 0);
    lv_obj_set_style_pad_column(s_sv_chart, 2, LV_PART_ITEMS);
    s_sv_series = lv_chart_add_series(s_sv_chart, COL_ACCENT, LV_CHART_AXIS_PRIMARY_Y);

    lv_obj_t *th = make_label(s_stats_panel, &font_pl_14, COL_DIM);
    lv_label_set_text(th, L()->st_top_airlines);
    lv_obj_set_pos(th, 0, 272);

    for (int i = 0; i < 8; i++) {
        s_sv_top[i] = make_label(s_stats_panel, &font_pl_16, COL_TEXT);
        lv_obj_set_pos(s_sv_top[i], (i % 2) * 236, 298 + (i / 2) * 26);
        lv_label_set_text(s_sv_top[i], "");
    }

    s_sv_days = make_label(s_stats_panel, &font_pl_14, COL_DIM);
    lv_obj_align(s_sv_days, LV_ALIGN_TOP_RIGHT, 0, 6);
    lv_label_set_text(s_sv_days, "");

    s_sv_metar = make_label(s_stats_panel, &font_pl_14, COL_DIM);
    lv_obj_set_pos(s_sv_metar, 0, 380);
    lv_obj_set_width(s_sv_metar, 458);
    lv_label_set_long_mode(s_sv_metar, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_sv_metar, "");
}

static void render_stats_panel(void)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%d", s_stats_snap.unique);
    lv_label_set_text(s_sv_vals[0], buf);
    snprintf(buf, sizeof(buf), "%d ft", s_stats_snap.max_alt_ft);
    lv_label_set_text(s_sv_vals[1], buf);
    snprintf(buf, sizeof(buf), "%.0f kt", (double)s_stats_snap.max_gs_kt);
    lv_label_set_text(s_sv_vals[2], buf);
    snprintf(buf, sizeof(buf), "%.0f km", (double)s_stats_snap.max_dist_km);
    lv_label_set_text(s_sv_vals[3], buf);

    uint16_t maxv = 1;
    for (int i = 0; i < 24; i++) {
        if (s_stats_snap.hours[i] > maxv) {
            maxv = s_stats_snap.hours[i];
        }
    }
    lv_chart_set_range(s_sv_chart, LV_CHART_AXIS_PRIMARY_Y, 0, maxv);
    for (int i = 0; i < 24; i++) {
        lv_chart_set_value_by_id(s_sv_chart, s_sv_series, i, s_stats_snap.hours[i]);
    }
    lv_chart_refresh(s_sv_chart);

    char daysum[64];
    dailystats_summary(daysum, sizeof(daysum), L()->avg_word, L()->best_word);
    lv_label_set_text(s_sv_days, daysum);
    lv_label_set_text(s_sv_metar, metar_get());

    for (int i = 0; i < 8; i++) {
        if (i < s_stats_snap.top_n) {
            const char *name = airlines_get_cached(s_stats_snap.top[i].code);
            char row[64];
            snprintf(row, sizeof(row), "%s  %.28s  \xC3\x97%d",
                     s_stats_snap.top[i].code,
                     name != NULL && name[0] ? name : "",
                     s_stats_snap.top[i].n);
            lv_label_set_text(s_sv_top[i], row);
        } else {
            lv_label_set_text(s_sv_top[i], "");
        }
    }
}

static void build_detail(lv_obj_t *scr)
{
    lv_obj_t *panel = make_panel(scr);
    lv_obj_set_size(panel, 800 - LIST_W, 480 - HEADER_H);
    lv_obj_set_pos(panel, LIST_W, HEADER_H);
    s_detail_panel = panel;

    s_detail_empty = make_label(panel, &font_pl_20, COL_DIM);
    lv_label_set_text(s_detail_empty, L()->waiting_aircraft);
    lv_obj_center(s_detail_empty);

    s_detail_content = lv_obj_create(panel);
    lv_obj_set_size(s_detail_content, 800 - LIST_W, 480 - HEADER_H);
    lv_obj_set_pos(s_detail_content, 0, 0);
    lv_obj_set_style_bg_opa(s_detail_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_detail_content, 0, 0);
    lv_obj_set_style_pad_all(s_detail_content, 16, 0);
    lv_obj_clear_flag(s_detail_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_detail_content, LV_OBJ_FLAG_HIDDEN);

    /* Top: logo + names */
    s_logo_fallback = lv_obj_create(s_detail_content);
    lv_obj_set_size(s_logo_fallback, 90, 90);
    lv_obj_set_pos(s_logo_fallback, 0, 0);
    lv_obj_set_style_bg_color(s_logo_fallback, COL_ROW_SEL, 0);
    lv_obj_set_style_radius(s_logo_fallback, 45, 0);
    lv_obj_set_style_border_width(s_logo_fallback, 0, 0);
    lv_obj_clear_flag(s_logo_fallback, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *fb_label = make_label(s_logo_fallback, &lv_font_montserrat_24, COL_TEXT);
    lv_obj_center(fb_label);

    s_logo_img = lv_img_create(s_detail_content);
    lv_obj_set_pos(s_logo_img, 0, 0);
    lv_obj_set_style_radius(s_logo_img, 12, 0);
    lv_obj_set_style_clip_corner(s_logo_img, true, 0);
    lv_obj_add_flag(s_logo_img, LV_OBJ_FLAG_HIDDEN);

    s_callsign_label = make_label(s_detail_content, &lv_font_montserrat_32, COL_TEXT);
    lv_obj_set_pos(s_callsign_label, 106, 0);
    s_airline_label = make_label(s_detail_content, &font_pl_16, COL_DIM);
    lv_obj_set_pos(s_airline_label, 106, 38);
    s_type_label = make_label(s_detail_content, &font_pl_16, COL_ACCENT);
    lv_obj_set_pos(s_type_label, 106, 62);

    lv_obj_t *btn_map = lv_btn_create(s_detail_content);
    lv_obj_set_size(btn_map, 96, 40);
    lv_obj_align(btn_map, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_map, COL_ROW, 0);
    lv_obj_add_event_cb(btn_map, map_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ml = make_label(btn_map, &font_pl_16, COL_TEXT);
    lv_label_set_text_fmt(ml, LV_SYMBOL_GPS " %s", L()->map_btn);
    lv_obj_center(ml);

    lv_obj_t *btn_photo = lv_btn_create(s_detail_content);
    lv_obj_set_size(btn_photo, 60, 40);
    lv_obj_align(btn_photo, LV_ALIGN_TOP_RIGHT, -104, 0);
    lv_obj_set_style_bg_color(btn_photo, COL_ROW, 0);
    lv_obj_add_event_cb(btn_photo, photo_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pl = make_label(btn_photo, &font_pl_16, COL_TEXT);
    lv_label_set_text(pl, LV_SYMBOL_IMAGE);
    lv_obj_center(pl);

    /* Route: ORIG -> DEST */
    s_orig_code = make_label(s_detail_content, &lv_font_montserrat_32, COL_TEXT);
    lv_obj_set_pos(s_orig_code, 0, 116);
    s_orig_city = make_label(s_detail_content, &font_pl_14, COL_DIM);
    lv_obj_set_pos(s_orig_city, 0, 152);

    lv_obj_t *arrow = make_label(s_detail_content, &lv_font_montserrat_24, COL_ACCENT);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_pos(arrow, 222, 122);

    s_dest_code = make_label(s_detail_content, &lv_font_montserrat_32, COL_TEXT);
    lv_obj_set_style_text_align(s_dest_code, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_dest_code, 320, 116);
    lv_obj_set_width(s_dest_code, 138);
    s_dest_city = make_label(s_detail_content, &font_pl_14, COL_DIM);
    lv_obj_set_style_text_align(s_dest_city, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_dest_city, 258, 152);
    lv_obj_set_width(s_dest_city, 200);

    s_progress_bar = lv_bar_create(s_detail_content);
    lv_obj_set_size(s_progress_bar, 458, 12);
    lv_obj_set_pos(s_progress_bar, 0, 180);
    lv_bar_set_range(s_progress_bar, 0, 100);
    lv_obj_set_style_bg_color(s_progress_bar, COL_ROW, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_progress_bar, COL_ACCENT, LV_PART_INDICATOR);

    s_progress_label = make_label(s_detail_content, &font_pl_14, COL_DIM);
    lv_obj_set_pos(s_progress_label, 0, 200);

    s_look_label = make_label(s_detail_content, &font_pl_14, COL_ACCENT);
    lv_obj_set_pos(s_look_label, 0, 219);
    lv_obj_set_width(s_look_label, 458);
    lv_label_set_long_mode(s_look_label, LV_LABEL_LONG_DOT);

    /* Stats grid */
    lv_obj_t *grid = lv_obj_create(s_detail_content);
    lv_obj_set_size(grid, 468, 130);
    lv_obj_set_pos(grid, 0, 238);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    make_stat(grid, 0, 0, L()->st_alt, &s_stat_vals[0]);
    make_stat(grid, 1, 0, L()->st_speed, &s_stat_vals[1]);
    make_stat(grid, 2, 0, L()->st_vrate, &s_stat_vals[2]);
    make_stat(grid, 0, 1, L()->st_dist, &s_stat_vals[3]);
    make_stat(grid, 1, 1, L()->st_track, &s_stat_vals[4]);
    make_stat(grid, 2, 1, L()->st_reg, &s_stat_vals[5]);
}

void ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);

    build_header(scr);
    build_list(scr);
    build_detail(scr);
    build_map_panel(scr);
    build_radar_panel(scr);
    build_stats_panel(scr);

    s_cycle_timer = lv_timer_create(cycle_timer_cb, CYCLE_MS, NULL);
    lv_timer_pause(s_cycle_timer);
    lv_timer_create(clock_timer_cb, 5000, NULL);
    lv_timer_create(idle_timer_cb, 10000, NULL);
}

void ui_set_update_available(bool available)
{
    if (s_gear_label != NULL) {
        lv_obj_set_style_text_color(s_gear_label,
                                    available ? lv_color_hex(0xffd166) : COL_TEXT, 0);
    }
}

void ui_set_status_alert(bool alert)
{
    if (s_status_label != NULL) {
        lv_obj_set_style_text_color(s_status_label,
                                    alert ? lv_color_hex(0xff5252) : COL_DIM, 0);
        lv_obj_set_style_text_font(s_status_label,
                                   alert ? &font_pl_20 : &font_pl_14, 0);
    }
}

void ui_set_status(const char *text)
{
    if (s_status_label != NULL) {
        lv_label_set_text(s_status_label, text);
    }
}

void ui_set_weather(const char *text)
{
    strlcpy(s_weather_txt, text, sizeof(s_weather_txt));
    if (s_weather_label != NULL) {
        lv_label_set_text(s_weather_label, text);
    }
    if (s_amb_wx != NULL) {
        lv_label_set_text(s_amb_wx, text);
    }
}

static void render_list_selection(void)
{
    for (int i = 0; i < s_shown_count; i++) {
        lv_obj_set_style_bg_color(s_list_rows[i], i == s_selected ? COL_ROW_SEL : COL_ROW, 0);
    }
}

static void render_detail(void)
{
    if (s_selected < 0 || s_selected >= s_shown_count) {
        lv_obj_add_flag(s_detail_content, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_detail_empty, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_detail_content, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_detail_empty, LV_OBJ_FLAG_HIDDEN);

    const aircraft_t *ac = &s_shown[s_selected].ac;
    const route_info_t *rt = s_shown[s_selected].route.callsign[0] ? &s_shown[s_selected].route : NULL;

    lv_label_set_text(s_callsign_label, ac->callsign[0] ? ac->callsign : ac->hex);

    if (s_shown[s_selected].iata[0]) {
        lv_label_set_text_fmt(s_airline_label, "%s%s%s",
                              s_shown[s_selected].airline,
                              s_shown[s_selected].airline[0] ? "  \xC2\xB7  " : "",
                              s_shown[s_selected].iata);
    } else {
        lv_label_set_text(s_airline_label, s_shown[s_selected].airline);
    }

    if (ac->type_desc[0]) {
        lv_label_set_text(s_type_label, ac->type_desc);
    } else {
        lv_label_set_text(s_type_label, ac->type_icao);
    }

    /* Logo or fallback badge with airline ICAO */
    const char *code = airline_code(ac, rt);
    const lv_img_dsc_t *logo = code ? logos_get(code) : NULL;
    if (logo != NULL) {
        lv_img_set_src(s_logo_img, logo);
        lv_obj_clear_flag(s_logo_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_logo_fallback, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_logo_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_logo_fallback, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *fb_label = lv_obj_get_child(s_logo_fallback, 0);
        char badge[8];
        if (code != NULL) {
            strlcpy(badge, code, sizeof(badge));
        } else if (ac->callsign[0]) {
            strlcpy(badge, ac->callsign, 4);
        } else {
            strlcpy(badge, "?", sizeof(badge));
        }
        lv_label_set_text(fb_label, badge);
    }

    char buf[96];
    if (rt != NULL && rt->valid) {
        char lt[8];
        lv_label_set_text(s_orig_code,
                          rt->origin.iata[0] ? rt->origin.iata : rt->origin.icao);
        airport_local_time(&rt->origin, lt, sizeof(lt));
        lv_label_set_text_fmt(s_orig_city, "%s%s%s%s%s",
                              rt->origin.city,
                              rt->origin.country[0] ? " (" : "",
                              rt->origin.country,
                              rt->origin.country[0] ? ")" : "",
                              lt[0] ? "" : "");
        if (lt[0]) {
            lv_label_ins_text(s_orig_city, LV_LABEL_POS_LAST, "  \xC2\xB7  ");
            lv_label_ins_text(s_orig_city, LV_LABEL_POS_LAST, lt);
        }
        lv_label_set_text(s_dest_code,
                          rt->destination.iata[0] ? rt->destination.iata : rt->destination.icao);
        airport_local_time(&rt->destination, lt, sizeof(lt));
        lv_label_set_text_fmt(s_dest_city, "%s%s%s%s",
                              rt->destination.city,
                              rt->destination.country[0] ? " (" : "",
                              rt->destination.country,
                              rt->destination.country[0] ? ")" : "");
        if (lt[0]) {
            lv_label_ins_text(s_dest_city, LV_LABEL_POS_LAST, "  \xC2\xB7  ");
            lv_label_ins_text(s_dest_city, LV_LABEL_POS_LAST, lt);
        }

        double prog = geo_progress(rt->origin.lat, rt->origin.lon,
                                   rt->destination.lat, rt->destination.lon,
                                   ac->lat, ac->lon);
        lv_bar_set_value(s_progress_bar, (int)(prog * 100.0), LV_ANIM_OFF);
        double remaining = geo_haversine_km(ac->lat, ac->lon,
                                            rt->destination.lat, rt->destination.lon);
        char eta[40];
        format_eta(eta, sizeof(eta), remaining, ac->gs_kts);
        snprintf(buf, sizeof(buf), L()->km_to_go_fmt,
                 (int)(prog * 100.0), remaining, eta[0] ? "  -  " : "", eta);
        lv_label_set_text(s_progress_label, buf);
    } else {
        lv_label_set_text(s_orig_code, "----");
        lv_label_set_text(s_orig_city, L()->route_unknown);
        lv_label_set_text(s_dest_code, "----");
        lv_label_set_text(s_dest_city, "");
        lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(s_progress_label, "");
    }

    if (ac->on_ground) {
        lv_label_set_text(s_stat_vals[0], L()->ground);
    } else {
        snprintf(buf, sizeof(buf), "%d ft", ac->alt_baro_ft);
        lv_label_set_text(s_stat_vals[0], buf);
    }
    snprintf(buf, sizeof(buf), "%.0f kt", (double)ac->gs_kts);
    lv_label_set_text(s_stat_vals[1], buf);
    snprintf(buf, sizeof(buf), "%+d fpm", ac->baro_rate_fpm);
    lv_label_set_text(s_stat_vals[2], buf);
    snprintf(buf, sizeof(buf), "%.1f km", ac->dist_nm >= 0 ? ac->dist_nm * 1.852 : 0.0);
    lv_label_set_text(s_stat_vals[3], buf);
    snprintf(buf, sizeof(buf), "%.0f\xC2\xB0", (double)ac->track_deg);
    lv_label_set_text(s_stat_vals[4], buf);
    const char *cc = reg_country(ac->reg);
    if (ac->reg[0] && cc != NULL) {
        /* ASCII only: this tile uses the built-in Montserrat font */
        snprintf(buf, sizeof(buf), "%s (%s)", ac->reg, cc);
        lv_label_set_text(s_stat_vals[5], buf);
    } else {
        lv_label_set_text(s_stat_vals[5], ac->reg[0] ? ac->reg : "-");
    }

    /* spotter line: where to look + flyover prediction */
    char look[128] = "";
    if (ac->has_pos && !ac->on_ground && ac->dist_nm >= 0 && s_home_ok) {
        double t_s, cpa_km;
        bool cpa_ok = geo_cpa(s_home_lat, s_home_lon, ac->lat, ac->lon,
                              ac->track_deg, ac->gs_kts, &t_s, &cpa_km) &&
                      cpa_km < 25.0 && t_s < 30 * 60;
        /* the compact variant leaves room for the flyover prediction */
        int n = snprintf(look, sizeof(look),
                         cpa_ok ? L()->look_short_fmt : L()->look_fmt,
                         lang_compass((int)ac->dir_deg),
                         (int)geo_elevation_deg(ac->dist_nm * 1.852, ac->alt_baro_ft));
        if (cpa_ok && n > 0 && (size_t)n < sizeof(look) - 8) {
            snprintf(look + n, sizeof(look) - n, "  \xC2\xB7  ");
            n = strlen(look);
            snprintf(look + n, sizeof(look) - n, L()->cpa_fmt, (int)(t_s / 60), cpa_km);
        }
    }
    lv_label_set_text(s_look_label, look);
}

void ui_update(const aircraft_list_t *list)
{
    /* Snapshot aircraft + routes so touch callbacks never race the fetcher. */
    flight_stats_get(&s_stats_snap);
    s_shown_count = list->count < MAX_SHOWN ? list->count : MAX_SHOWN;
    for (int i = 0; i < s_shown_count; i++) {
        s_shown[i].ac = list->ac[i];
        const route_info_t *rt = routes_get_cached(list->ac[i].callsign);
        if (rt != NULL && rt->valid && list->ac[i].has_pos &&
            !geo_route_plausible(rt->origin.lat, rt->origin.lon,
                                 rt->destination.lat, rt->destination.lon,
                                 list->ac[i].lat, list->ac[i].lon)) {
            /* Stale/reused callsign in the route DB - don't show nonsense. */
            rt = NULL;
        }
        if (rt != NULL) {
            s_shown[i].route = *rt;
        } else {
            memset(&s_shown[i].route, 0, sizeof(route_info_t));
        }

        s_shown[i].iata[0] = '\0';
        const char *fa = faflight_get_cached(list->ac[i].callsign);
        if (fa != NULL && fa[0]) {
            strlcpy(s_shown[i].iata, fa, sizeof(s_shown[i].iata));
        }

        /* Airline display name: route DB first, adsbdb airline lookup second */
        s_shown[i].airline[0] = '\0';
        if (rt != NULL && rt->valid && rt->airline_name[0]) {
            strlcpy(s_shown[i].airline, rt->airline_name, sizeof(s_shown[i].airline));
        } else {
            const char *code = airline_code(&s_shown[i].ac, rt);
            const char *name = code ? airlines_get_cached(code) : NULL;
            if (name != NULL) {
                strlcpy(s_shown[i].airline, name, sizeof(s_shown[i].airline));
            }
        }
    }

    /* Keep selection pinned to the same aircraft across refreshes. */
    s_selected = -1;
    for (int i = 0; i < s_shown_count; i++) {
        if (s_selected_hex[0] && strcmp(s_shown[i].ac.hex, s_selected_hex) == 0) {
            s_selected = i;
            break;
        }
    }
    if (s_selected < 0 && s_shown_count > 0) {
        s_selected = 0;
        strlcpy(s_selected_hex, s_shown[0].ac.hex, sizeof(s_selected_hex));
    }

    for (int i = 0; i < MAX_SHOWN; i++) {
        if (i < s_shown_count) {
            const aircraft_t *ac = &s_shown[i].ac;
            lv_obj_t *row = s_list_rows[i];
            lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);

            lv_obj_t *cs_label = lv_obj_get_child(row, 0);
            lv_label_set_text(cs_label, ac->callsign[0] ? ac->callsign : ac->hex);
            /* gold for military / heavies / watchlist hits */
            bool interesting = flight_is_interesting(ac, settings_get()->watch_regs);
            lv_obj_set_style_text_color(cs_label,
                                        interesting ? lv_color_hex(0xffd166) : COL_TEXT, 0);
            lv_label_set_text(lv_obj_get_child(row, 1), ac->type_icao[0] ? ac->type_icao : "?");

            char info[64];
            if (ac->on_ground) {
                snprintf(info, sizeof(info), "%s  -  %.1f km", L()->ground, ac->dist_nm * 1.852);
            } else {
                const char *trend = "";
                if (ac->baro_rate_fpm > 300) {
                    trend = LV_SYMBOL_UP " ";
                } else if (ac->baro_rate_fpm < -300) {
                    trend = LV_SYMBOL_DOWN " ";
                }
                snprintf(info, sizeof(info), "%s%d ft  %.0f kt  %.1f km",
                         trend, ac->alt_baro_ft, (double)ac->gs_kts, ac->dist_nm * 1.852);
            }
            lv_label_set_text(lv_obj_get_child(row, 2), info);

            lv_obj_t *logo_img = lv_obj_get_child(row, 3);
            const char *code = airline_code(ac, &s_shown[i].route);
            const lv_img_dsc_t *logo = code ? logos_get(code) : NULL;
            if (logo != NULL) {
                lv_img_set_src(logo_img, logo);
                lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_add_flag(s_list_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    render_list_selection();
    render_right();
    render_ambient();
}
