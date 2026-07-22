#include "ui_map.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fonts.h"
#include "geo_math.h"
#include "lang.h"
#include "lvgl_port.h"
#include "tilemap.h"
#include "trails.h"

#include "esp_heap_caps.h"
#include "extra/libs/png/lodepng.h"

#include "theme.h"

LV_IMG_DECLARE(img_plane);

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
static lv_point_t s_trail_pts[TRAIL_LEN];
static lv_img_dsc_t s_map_dsc, s_map_small_dsc;
static uint8_t *s_map_data, *s_map_small_data;

/* Snapshot of what the overlay shows (worker task rebuilds from these) */
static aircraft_t   s_ac;
static route_info_t s_rt;
static bool         s_have_route;

/* Tile view for the full-screen map */
static uint16_t     *s_tiles;
static lv_img_dsc_t  s_tiles_dsc;
static tile_view_t   s_view;
static bool          s_view_ok;
static volatile bool s_tiles_busy;
static int           s_generation;   /* bumped each open/close */

static void close_cb(lv_event_t *e)
{
    if (s_overlay != NULL) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
        s_generation++;
    }
}

/* Decode a bundled PNG once into a persistent RGB565 buffer. Draw-time PNG
 * decoding needs a 1.3 MB contiguous block per frame; under memory pressure
 * LVGL silently falls back to its built-in decoder, which renders the raw
 * file bytes as pixels (colorful noise). Pre-decoding removes that failure
 * mode entirely. */
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
    uint8_t *raw = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (raw == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(raw, 1, size, f) != (size_t)size) {
        free(raw);
        fclose(f);
        return NULL;
    }
    fclose(f);

    unsigned char *rgba = NULL;
    unsigned w = 0, h = 0;
    unsigned rc = lodepng_decode32(&rgba, &w, &h, raw, size);
    free(raw);
    if (rc != 0 || rgba == NULL) {
        return NULL;
    }
    uint16_t *px = heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
    if (px == NULL) {
        free(rgba);
        return NULL;
    }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const unsigned char *p = rgba + i * 4;
        px[i] = ((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3);
    }
    free(rgba);

    *data = (uint8_t *)px;
    dsc->header.always_zero = 0;
    dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->data = *data;
    dsc->data_size = (size_t)w * h * 2;
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
    if (s_view_ok) {
        int xx, yy;
        tilemap_project(&s_view, lat, lon, &xx, &yy);
        *x = (lv_coord_t)xx;
        *y = (lv_coord_t)(yy + MAP_Y);
        return;
    }
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

static void build_content(void)
{
    const aircraft_t *ac = &s_ac;
    const route_info_t *rt = &s_rt;
    bool have_route = s_have_route;

    /* Header */
    char title[200];
    if (have_route) {
        snprintf(title, sizeof(title), "%.8s   %.4s " LV_SYMBOL_RIGHT " %.4s",
                 ac->callsign[0] ? ac->callsign : ac->hex,
                 rt->origin.iata[0] ? rt->origin.iata : rt->origin.icao,
                 rt->destination.iata[0] ? rt->destination.iata : rt->destination.icao);
    } else {
        snprintf(title, sizeof(title), "%.8s   (%.60s)",
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

    /* Map: tile view when rendered, bundled world map otherwise */
    const lv_img_dsc_t *map = s_view_ok ? &s_tiles_dsc : ui_map_get_image();
    if (map != NULL) {
        lv_obj_t *img = lv_img_create(s_overlay);
        lv_img_set_src(img, map);
        lv_obj_set_pos(img, 0, MAP_Y);
    }

    lv_coord_t x, y;

    /* breadcrumb trail */
    float tlat[TRAIL_LEN], tlon[TRAIL_LEN];
    int tn = trails_get(ac->hex, tlat, tlon, TRAIL_LEN);
    if (tn >= 2) {
        for (int k = 0; k < tn; k++) {
            project(tlat[k], tlon[k], &x, &y);
            s_trail_pts[k].x = x;
            s_trail_pts[k].y = y;
        }
        lv_obj_t *trail = lv_line_create(s_overlay);
        lv_line_set_points(trail, s_trail_pts, tn);
        lv_obj_set_style_line_width(trail, 2, 0);
        lv_obj_set_style_line_color(trail, COL_PLANE, 0);
        lv_obj_set_style_line_opa(trail, LV_OPA_60, 0);
    }

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
        lv_obj_t *pl = lv_img_create(s_overlay);
        lv_img_set_src(pl, &img_plane);
        lv_obj_set_style_img_recolor(pl, alt_color(ac->alt_baro_ft, ac->on_ground), 0);
        lv_obj_set_style_img_recolor_opa(pl, LV_OPA_COVER, 0);
        lv_img_set_angle(pl, (int)(ac->track_deg * 10));
        lv_obj_set_pos(pl, x - 14, y - 14);
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

static void map_tiles_task(void *arg)
{
    int gen = (int)(intptr_t)arg;

    double latmin, latmax, lonmin, lonmax;
    if (s_have_route) {
        latmin = latmax = s_rt.origin.lat;
        lonmin = lonmax = s_rt.origin.lon;
        double lats[2] = { s_rt.destination.lat, s_ac.has_pos ? s_ac.lat : s_rt.destination.lat };
        double lons[2] = { s_rt.destination.lon, s_ac.has_pos ? s_ac.lon : s_rt.destination.lon };
        for (int i = 0; i < 2; i++) {
            if (lats[i] < latmin) latmin = lats[i];
            if (lats[i] > latmax) latmax = lats[i];
            if (lons[i] < lonmin) lonmin = lons[i];
            if (lons[i] > lonmax) lonmax = lons[i];
        }
    } else if (s_ac.has_pos) {
        latmin = s_ac.lat - 1.0;
        latmax = s_ac.lat + 1.0;
        lonmin = s_ac.lon - 2.0;
        lonmax = s_ac.lon + 2.0;
    } else {
        s_tiles_busy = false;
        vTaskDelete(NULL);
        return;
    }
    double mlat = (latmax - latmin) * 0.15 + 0.4;
    double mlon = (lonmax - lonmin) * 0.15 + 0.8;

    if (s_tiles == NULL) {
        s_tiles = heap_caps_malloc(MAP_W * MAP_H * 2, MALLOC_CAP_SPIRAM);
    }
    tile_view_t view;
    bool ok = s_tiles != NULL &&
              tilemap_render(s_tiles, MAP_W, MAP_H,
                             latmin - mlat, latmax + mlat,
                             lonmin - mlon, lonmax + mlon, &view);

    if (lvgl_port_lock(-1)) {
        if (ok && s_overlay != NULL && gen == s_generation) {
            s_view = view;
            s_view_ok = true;
            s_tiles_dsc.header.always_zero = 0;
            s_tiles_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
            s_tiles_dsc.header.w = MAP_W;
            s_tiles_dsc.header.h = MAP_H;
            s_tiles_dsc.data = (const uint8_t *)s_tiles;
            s_tiles_dsc.data_size = MAP_W * MAP_H * 2;
            lv_obj_clean(s_overlay);
            build_content();
        }
        lvgl_port_unlock();
    }
    /* if the overlay was reopened for another flight while we rendered,
     * render again for the current one */
    if (s_overlay != NULL && gen != s_generation) {
        if (xTaskCreatePinnedToCore(map_tiles_task, "map_tiles", 10240,
                                    (void *)(intptr_t)s_generation, 3, NULL, 0) != pdPASS) {
            s_tiles_busy = false;
        }
    } else {
        s_tiles_busy = false;
    }
    vTaskDelete(NULL);
}

void ui_map_open(const aircraft_t *ac, const route_info_t *rt)
{
    if (s_overlay != NULL) {
        return;
    }
    s_ac = *ac;
    s_have_route = rt != NULL && rt->valid;
    if (s_have_route) {
        s_rt = *rt;
    } else {
        memset(&s_rt, 0, sizeof(s_rt));
    }
    s_view_ok = false;
    s_generation++;

    s_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_overlay, 800, 480);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, COL_BG, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    build_content();

    /* a still-running worker will respawn itself for this generation */
    if (!s_tiles_busy) {
        s_tiles_busy = true;
        if (xTaskCreatePinnedToCore(map_tiles_task, "map_tiles", 10240,
                                    (void *)(intptr_t)s_generation, 3, NULL, 0) != pdPASS) {
            s_tiles_busy = false;
        }
    }
}
