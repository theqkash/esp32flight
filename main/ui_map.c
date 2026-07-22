#include "ui_map.h"

#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "esp_heap_caps.h"

#include "fonts.h"
#include "geo_math.h"
#include "lang.h"

#include "theme.h"

#define COL_BG     (app_theme()->bg)
#define COL_PANEL  (app_theme()->panel)
#define COL_ACCENT (app_theme()->accent)
#define COL_TEXT   (app_theme()->text)
#define COL_DIM    (app_theme()->dim)
#define COL_ORIG   lv_color_hex(0x39d98a)
#define COL_DEST   lv_color_hex(0xff6b6b)
#define COL_PLANE  lv_color_hex(0xffd166)

#define MAP_W      800
#define MAP_H      400
#define MAP_Y      56
#define PATH_PTS   33

static lv_obj_t *s_overlay;
static lv_point_t s_path[PATH_PTS];
static lv_img_dsc_t s_map_dsc, s_map_small_dsc;
static uint8_t *s_map_data, *s_map_small_data;

static void close_cb(lv_event_t *e)
{
    if (s_overlay != NULL) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
    }
}

static const lv_img_dsc_t *load_png(const char *path, uint8_t **data, lv_img_dsc_t *dsc)
{
    if (*data != NULL) {
        return dsc;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return NULL;
    }
    *data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (*data == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(*data, 1, size, f) != (size_t)size) {
        free(*data);
        *data = NULL;
        fclose(f);
        return NULL;
    }
    fclose(f);
    dsc->header.always_zero = 0;
    /* The PNG decoder outputs true-color+alpha; declaring RAW here makes the
     * renderer misread the decoded buffer (2 vs 3 bytes/px) and garble colors. */
    dsc->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    dsc->data = *data;
    dsc->data_size = size;
    return dsc;
}

const lv_img_dsc_t *ui_map_get_image(void)
{
    return load_png("/assets/map/world.png", &s_map_data, &s_map_dsc);
}

const lv_img_dsc_t *ui_map_get_image_small(void)
{
    return load_png("/assets/map/world_small.png", &s_map_small_data, &s_map_small_dsc);
}

static void project(double lat, double lon, lv_coord_t *x, lv_coord_t *y)
{
    *x = (lv_coord_t)((lon + 180.0) / 360.0 * MAP_W);
    *y = (lv_coord_t)(MAP_Y + (90.0 - lat) / 180.0 * MAP_H);
}

static lv_obj_t *marker(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, int d, lv_color_t color)
{
    lv_obj_t *m = lv_obj_create(parent);
    lv_obj_set_size(m, d, d);
    lv_obj_set_pos(m, x - d / 2, y - d / 2);
    lv_obj_set_style_radius(m, d / 2, 0);
    lv_obj_set_style_bg_color(m, color, 0);
    lv_obj_set_style_border_width(m, 2, 0);
    lv_obj_set_style_border_color(m, COL_BG, 0);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return m;
}

static void code_label(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                       const char *text, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &font_pl_14, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_bg_color(l, COL_BG, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(l, 4, 0);
    lv_label_set_text(l, text);
    /* keep on screen */
    if (x > MAP_W - 70) {
        x = MAP_W - 70;
    }
    if (x < 0) {
        x = 0;
    }
    if (y > MAP_Y + MAP_H - 22) {
        y = MAP_Y + MAP_H - 22;
    }
    lv_obj_set_pos(l, x, y);
}

void ui_map_open(const aircraft_t *ac, const route_info_t *rt)
{
    if (s_overlay != NULL) {
        return;
    }
    bool have_route = rt != NULL && rt->valid;

    s_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_overlay, 800, 480);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, COL_BG, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* Header */
    char title[96];
    if (have_route) {
        snprintf(title, sizeof(title), "%s   %s " LV_SYMBOL_RIGHT " %s",
                 ac->callsign[0] ? ac->callsign : ac->hex,
                 rt->origin.iata[0] ? rt->origin.iata : rt->origin.icao,
                 rt->destination.iata[0] ? rt->destination.iata : rt->destination.icao);
    } else {
        snprintf(title, sizeof(title), "%s   (%s)",
                 ac->callsign[0] ? ac->callsign : ac->hex, L()->route_unknown);
    }
    lv_obj_t *tl = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(tl, COL_TEXT, 0);
    lv_label_set_text(tl, title);
    lv_obj_set_pos(tl, 16, 12);

    lv_obj_t *btn_close = lv_btn_create(s_overlay);
    lv_obj_set_size(btn_close, 52, 40);
    lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_obj_set_style_bg_color(btn_close, COL_PANEL, 0);
    lv_obj_add_event_cb(btn_close, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(btn_close);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_center(xl);

    /* World map */
    const lv_img_dsc_t *map = ui_map_get_image();
    if (map != NULL) {
        lv_obj_t *img = lv_img_create(s_overlay);
        lv_img_set_src(img, map);
        lv_obj_set_pos(img, 0, MAP_Y);
    }

    lv_coord_t x, y;
    if (have_route) {
        /* Great-circle path */
        for (int i = 0; i < PATH_PTS; i++) {
            double lat, lon;
            geo_gc_point(rt->origin.lat, rt->origin.lon,
                         rt->destination.lat, rt->destination.lon,
                         (double)i / (PATH_PTS - 1), &lat, &lon);
            project(lat, lon, &x, &y);
            s_path[i].x = x;
            s_path[i].y = y;
        }
        lv_obj_t *line = lv_line_create(s_overlay);
        lv_line_set_points(line, s_path, PATH_PTS);
        lv_obj_set_style_line_width(line, 3, 0);
        lv_obj_set_style_line_color(line, COL_ACCENT, 0);
        lv_obj_set_style_line_rounded(line, true, 0);

        /* Markers + labels */
        project(rt->origin.lat, rt->origin.lon, &x, &y);
        marker(s_overlay, x, y, 12, COL_ORIG);
        code_label(s_overlay, x + 8, y - 24,
                   rt->origin.iata[0] ? rt->origin.iata : rt->origin.icao, COL_ORIG);

        project(rt->destination.lat, rt->destination.lon, &x, &y);
        marker(s_overlay, x, y, 12, COL_DEST);
        code_label(s_overlay, x + 8, y - 24,
                   rt->destination.iata[0] ? rt->destination.iata : rt->destination.icao, COL_DEST);
    }

    if (ac->has_pos) {
        project(ac->lat, ac->lon, &x, &y);
        marker(s_overlay, x, y, 14, COL_PLANE);
        if (!have_route) {
            code_label(s_overlay, x + 8, y - 24,
                       ac->callsign[0] ? ac->callsign : ac->hex, COL_PLANE);
        }
    }

    /* Footer */
    char foot[200];
    if (have_route) {
        double prog = geo_progress(rt->origin.lat, rt->origin.lon,
                                   rt->destination.lat, rt->destination.lon,
                                   ac->lat, ac->lon);
        double remaining = geo_haversine_km(ac->lat, ac->lon,
                                            rt->destination.lat, rt->destination.lon);
        char tail[64];
        snprintf(tail, sizeof(tail), L()->km_to_go_fmt, (int)(prog * 100.0), remaining, "", "");
        snprintf(foot, sizeof(foot), "%.28s " LV_SYMBOL_RIGHT " %.28s   -   %.60s",
                 rt->origin.city[0] ? rt->origin.city : rt->origin.name,
                 rt->destination.city[0] ? rt->destination.city : rt->destination.name,
                 tail);
    } else {
        snprintf(foot, sizeof(foot), "%d ft   %.0f kt   %s",
                 ac->alt_baro_ft, (double)ac->gs_kts, L()->route_unknown);
    }
    lv_obj_t *fl = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(fl, &font_pl_16, 0);
    lv_obj_set_style_text_color(fl, COL_DIM, 0);
    lv_label_set_text(fl, foot);
    lv_obj_align(fl, LV_ALIGN_BOTTOM_LEFT, 16, -6);
}
