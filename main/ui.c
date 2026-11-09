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
#include "clock_fmt.h"
#include "ui_photo.h"
#include "geo_math.h"
#include "logos.h"
#include "flags.h"
#include "esp_timer.h"
#include "runways.h"
#include "airports.h"
#include "flight_task.h"
#include "flight_data.h"
#include "regcountry.h"
#include "routes.h"
#include "metar.h"
#include "units.h"
#include "extras.h"
#include "ships.h"
#include "airspace.h"
#include "dailystats.h"
#include "tilemap.h"
#include "rainviewer.h"
#include "trails.h"
#include "tz.h"
#include "ui_map.h"
#include "ui_settings.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <assert.h>
#include "esp_heap_caps.h"
#include "lvgl_port.h"
#include "esp_log.h"
#include "waveshare_rgb_lcd_port.h"

#define LIST_W        UISX(310)
#define HEADER_H      UISY(48)
#define MAX_SHOWN     40

/* The panel is a fixed 800x480; the app build may register a larger LVGL
 * display (tablets), so structural sizes come from the live display. On the
 * device these evaluate to exactly 800/480. */
#define SCR_W  LV_HOR_RES
#define SCR_H  LV_VER_RES
/* Design height of the hand-composed right-panel blocks (detail/stats);
 * extra vertical space beyond it is distributed at runtime. */
#define DSN_H  UISY(432)
/* Inner content width of the right panel (16px side padding). Stretchable
 * elements (bars, grids, charts) span it; on the device it is 458. */
#define DTL_W  (SCR_W - LIST_W - UISX(32))
/* Vertical surplus of the right panel over the 432px design; the detail
 * sections spread it out evenly (all zero on the device). */
#define DTL_VEXT (SCR_H - HEADER_H - DSN_H)
#define DTL_Y1 (DTL_VEXT / 4)          /* route block */
#define DTL_Y2 (DTL_VEXT / 2)          /* progress block */
#define DTL_Y3 (DTL_VEXT * 3 / 4)      /* stats grid */

#include "theme.h"

static const char *TAG = "ui";

LV_IMG_DECLARE(img_plane);
LV_IMG_DECLARE(img_heli);
LV_IMG_DECLARE(img_small);
LV_IMG_DECLARE(img_mil);
LV_IMG_DECLARE(img_glider);
LV_IMG_DECLARE(img_balloon);
LV_IMG_DECLARE(img_drone);

/* map sprite per flight_sprite_t id */
static const lv_img_dsc_t *class_sprite(int spr)
{
    switch (spr) {
    case FSPR_SMALL:   return &img_small;
    case FSPR_HELI:    return &img_heli;
    case FSPR_MIL:     return &img_mil;
    case FSPR_GLIDER:  return &img_glider;
    case FSPR_BALLOON: return &img_balloon;
    case FSPR_DRONE:   return &img_drone;
    default:           return &img_plane;
    }
}

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

/* ~26 KB with routes and airline strings: no DMA/ISR involvement, so it
 * lives in PSRAM instead of the scarce internal .bss (allocated in
 * ui_init before anything renders). */
static shown_flight_t *s_shown;
static int  s_shown_count;
static int  s_selected = -1;
static char s_selected_hex[ICAO_HEX_LEN];

static lv_obj_t *s_status_label;
static lv_obj_t *s_weather_label;
static lv_obj_t *s_list_panel;
static lv_obj_t *s_list_rows[MAX_SHOWN];
static lv_obj_t *s_list_mode_btnm;
static uint8_t s_list_mode;      /* 0 planes, 1 ships, 2 both (session only) */
static int s_list_plane_rows;    /* rows currently showing aircraft */
static int s_row_plane_idx[MAX_SHOWN];   /* row -> s_shown index */
static ship_t *s_row_ships;                  /* PSRAM, ships behind list rows */
static int s_row_ship_count;
static ship_t *s_marker_ships;               /* PSRAM, ships behind map markers */
static lv_obj_t *s_ship_pop, *s_ship_pop_name, *s_ship_pop_sub, *s_ship_pop_info;
static lv_obj_t *s_ship_content, *s_ship_name, *s_ship_sub, *s_ship_vals[6];
static uint32_t s_sel_ship_mmsi;             /* 0 = no ship selected */

/* the list toggle also decides what the radar/map draws */
static bool view_shows_planes(void);
static bool view_shows_ships(void);

/* Right-panel view modes */
#define VIEW_DETAIL 0
#define VIEW_MAP    1
#define VIEW_RADAR  2
#define VIEW_STATS  3
#define VIEW_RETRO  4
#define VIEW_COUNT  5
/* The ambient map composes in the 800x480 design space and is zoomed by
 * LVGL on larger panels: full-resolution compose buffers (2x 1.2 MB on
 * the 7B) plus the LVGL framebuffers simply do not coexist in a 6.4 MB
 * PSRAM pool - the reason the 7B screensaver never came up. The zoom is
 * uniform (keyed to width) and the upscale also makes ambient renders
 * ~60% cheaper on big panels. 800x480 boards: identity, pixel-identical. */
#define AMB_RENDER_W (SCR_W > 800 ? 800 : SCR_W)
#define AMB_RENDER_H (SCR_W > 800 ? 480 : SCR_H)
#define AMB_ZOOM_K   ((float)SCR_W / (float)AMB_RENDER_W)

#define EMB_MAP_W   (SCR_W - LIST_W)
#define EMB_MAP_H   (SCR_H - HEADER_H - UISY(187))   /* info bubble keeps its 169px strip */
/* Map bitmaps compose in the 800x480 design space on larger panels and
 * are zoomed by LVGL (see AMB_RENDER_*): full-res compose buffers for
 * three views do not fit next to the LVGL framebuffers on the 7B. Text
 * and widgets stay native-crisp; only the map bitmap scales. */
#define EMB_RENDER_W (SCR_W > 800 ? 800 - 310 : EMB_MAP_W)
#define EMB_RENDER_H (SCR_W > 800 ? (EMB_MAP_H * (800 - 310)) / EMB_MAP_W : EMB_MAP_H)
#define EMB_K        ((float)EMB_MAP_W / (float)EMB_RENDER_W)
/* Bundled world-map fallback image size (tiles replace it once rendered) */
#define EMB_BASE_W  490
#define EMB_BASE_H  245
#define CYCLE_MS    6000
static int        s_view_mode;
static lv_timer_t *s_cycle_timer;
static lv_timer_t *s_retro_timer;   /* 25 Hz sweep, paused when invisible */
static lv_obj_t *s_detail_panel;
static lv_obj_t *s_map_panel;
static lv_obj_t *s_emb_img;
static lv_obj_t *s_emb_line, *s_emb_orig, *s_emb_dest, *s_emb_plane, *s_emb_trail;
static lv_obj_t *s_emb_line2;
static lv_point_t s_emb_pts[33];
static lv_point_t s_emb_trail_pts[TRAIL_LEN];

/* Ambient map tile view (rendered by a worker task, CARTO tiles) */
static uint16_t     *s_emb_tiles;
static lv_img_dsc_t  s_emb_tiles_dsc;
static tile_view_t   s_emb_view;
static bool          s_emb_view_ok;
static char          s_emb_key[24];       /* key of the rendered view */
static int64_t       s_emb_partial_ms;     /* nonzero: rendered with missing tiles */
static int           s_emb_missing;
static char          s_emb_want_key[24];  /* key currently being rendered */
static volatile bool s_emb_busy;
static double        s_emb_bbox[4];       /* latmin, latmax, lonmin, lonmax */

/* Stats view */
/* Full-screen ambient screensaver (map + planes + clock + weather) */
static lv_obj_t *s_amb;
static lv_obj_t *s_amb_img;
static lv_obj_t *s_amb_planes[MAX_AIRCRAFT];
static lv_obj_t *s_amb_lbls[MAX_SHOWN];
static lv_obj_t *s_amb_clock, *s_amb_wx;
static lv_obj_t *s_amb_ring, *s_amb_home;
static lv_obj_t *s_amb_selbub;
static bool s_amb_retro;          /* overlay currently hosts the retro panel */
static bool s_retro_was_hidden;
static char s_amb_sel_cs[9];   /* callsign picked by tapping its sprite */
typedef struct {
    char  callsign[9];
    float lat, lon, track;
    float dist_nm, dir_deg;
    int   alt_ft;
    bool  ground;
    uint8_t fcls;    /* flight_class_t for the marker sprite */
} amb_target_t;
static amb_target_t s_all[MAX_AIRCRAFT];
static int s_all_count;

static uint16_t *s_amb_tiles;
static lv_img_dsc_t s_amb_tiles_dsc;
static tile_view_t s_amb_view;
static bool s_amb_view_ok;
static int  s_amb_missing;   /* tiles absent from the current ambient render */
static lv_obj_t *s_amb_note;  /* first-entry loading-map hint */
static volatile bool s_amb_busy;
static char s_amb_key[48];
static double s_amb_bbox[4];
static int64_t s_amb_last_try;
static float s_amb_scale = 1.0f;   /* upscale so the radius circle fills the height */
static int s_amb_px, s_amb_py;     /* scale pivot: home position in map pixels */
static char s_weather_txt[96];
static bool s_bl_off;
static bool s_bl_forced;   /* switched off by API/MQTT/button: stays dark until a tap or screen_on */

/* Re-assert the configured brightness after a full-on: bl_on() drives the
 * panel to maximum on every board. No-op at 100%% or on on/off-only
 * backlights (CH422G). Exposed for the settings slider and input actions. */
void ui_apply_brightness(void)
{
    const settings_t *c = settings_get();
    if (c->brightness_ctl && c->brightness < 100 && waveshare_rgb_lcd_bl_dimmable()) {
        waveshare_rgb_lcd_bl_pct(c->brightness);
    }
}

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
static lv_obj_t *s_radar_dots[MAX_AIRCRAFT];
static lv_obj_t *s_radar_info;
static lv_obj_t *s_radar_bub, *s_radar_blogo;
static bool s_radar_bub_off;   /* tapped away by the user (#9) */
static lv_obj_t *s_radar_range;
static lv_obj_t *s_radar_img;
static lv_obj_t *s_radar_rings[3];
static lv_obj_t *s_radar_home;

/* Retro radar view: phosphor CRT with a rotating sweep and afterglow blips */
#define RETRO_TRAIL   10          /* beam trail segments */
#define RETRO_TICK_MS 40
#define RETRO_STEP    2.4f        /* deg per tick: full turn every 6 s */
#define RET_COL_BEAM  lv_color_hex(0x53ff8a)
#define RET_COL_DIM   lv_color_hex(0x1e7a44)
#define RET_COL_BG    lv_color_hex(0x03140a)
static lv_obj_t *s_retro_panel;
static lv_obj_t *s_retro_beam[RETRO_TRAIL];
static lv_point_t s_retro_beam_pts[RETRO_TRAIL][2];
static lv_obj_t *s_retro_blips[MAX_AIRCRAFT];
static lv_obj_t *s_retro_lbls[MAX_AIRCRAFT];
static lv_obj_t *s_retro_info;
static float s_retro_angle;
static float s_retro_bearing[MAX_AIRCRAFT];
static bool  s_retro_valid[MAX_AIRCRAFT];
/* optional precipitation echo under the sweep (RainViewer, rain-only) */
static lv_obj_t     *s_retro_rain_img;
static uint16_t     *s_retro_rain_buf;
static lv_img_dsc_t  s_retro_rain_dsc;
static volatile bool s_retro_rain_busy;
static int64_t       s_retro_rain_ms = -1;
static int           s_retro_rain_gen = -1;
/* optional phosphor-green map underlay (land/water from the radar tiles) */
static lv_obj_t     *s_retro_map_img;
static uint16_t     *s_retro_map_buf;
static lv_img_dsc_t  s_retro_map_dsc;
static char          s_retro_map_key[48];
#define RADAR_W  (SCR_W - LIST_W)
#define RADAR_H  (SCR_H - HEADER_H)
#define RADAR_RENDER_W (SCR_W > 800 ? 800 - 310 : RADAR_W)
#define RADAR_RENDER_H (SCR_W > 800 ? (RADAR_H * (800 - 310)) / RADAR_W : RADAR_H)
#define RADAR_K        ((float)RADAR_W / (float)RADAR_RENDER_W)
#define RADAR_CX (RADAR_W / 2)
#define RADAR_CY (RADAR_H / 2 - UISY(6))
#define RADAR_R  (LV_MIN(RADAR_CX, RADAR_CY) - UISY(25))

/* Radar map background (home-area tiles) */
static uint16_t     *s_radar_tiles;
static lv_img_dsc_t  s_radar_tiles_dsc;
static tile_view_t   s_radar_view;
static double s_radar_zoom = 1.0;   /* window multiplier, <1 = closer */
static float  s_radar_scale = 1.0f; /* post-render upscale to fill the panel */
static int    s_radar_spx, s_radar_spy;
static bool          s_radar_view_ok;
static volatile bool s_radar_busy;
static char          s_radar_key[48];
static int64_t       s_radar_partial_ms;   /* nonzero: rendered with missing tiles */
static int64_t       s_radar_done_ms;      /* last render, for the rain recompose */
static int           s_radar_missing;
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
/* Big-screen type tier for the detail view: on canvases much taller than
 * the 432px design (tablets) fonts and the logo scale up. The big fonts are
 * compiled only into the app build; the device always uses the small tier. */
static bool s_big;
static const lv_font_t *s_f_code;   /* callsign + airport codes */
static const lv_font_t *s_f_name;   /* airline, type, times */
static const lv_font_t *s_f_small;  /* cities, footers */
static const lv_font_t *s_f_val;    /* stat tile values */
static int s_city_y;                /* city row y (differs per tier) */
static int s_dcity_w;               /* dest city label width */

static lv_obj_t *s_orig_code, *s_orig_city;
static lv_obj_t *s_orig_flag, *s_dest_flag, *s_reg_flag;
static lv_obj_t *s_orig_time, *s_dest_time, *s_extra_label;
static lv_obj_t *s_dest_code, *s_dest_city;
static lv_obj_t *s_progress_bar;
static lv_obj_t *s_progress_label;
static lv_obj_t *s_look_label;
static lv_obj_t *s_stat_vals[6];
static lv_obj_t *s_detail_empty;
static lv_obj_t *s_detail_content;

static void render_detail(void);
static void render_list_selection(void);
static void fb_upscale(uint16_t *fb, int W, int H, int px, int py, float k);
static void render_map_panel(void);
static void render_radar_panel(void);
static void render_stats_panel(void);
static void render_retro_panel(void);
static void label_set_if_changed(lv_obj_t *l, const char *txt);

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
    case VIEW_RETRO:
        render_retro_panel();
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

/* "14:32" local time at an airport, "" if timezone or clock unknown.
 * Shown only when the airport's zone differs from home: a domestic
 * flight duplicating the header clock next to both airports read as
 * bogus departure/arrival times to everyone who saw it (issue #7). */
static void airport_local_time(const airport_t *ap, char *dst, size_t n)
{
    dst[0] = '\0';
    time_t now = time(NULL);
    if (!ap->tz_known || now < 1600000000) {
        return;
    }
    if (tz_home_known() && ap->tz_offset_s == tz_home_offset()) {
        return;
    }
    time_t local = now + ap->tz_offset_s;
    struct tm tm;
    gmtime_r(&local, &tm);
    clock_fmt(dst, n, tm.tm_hour, tm.tm_min);
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

static void render_list_rows(void);

/* Logos load asynchronously in a worker; repaint the affected panels as
 * soon as a batch lands instead of waiting for the next data cycle. */
static void logo_tick_cb(lv_timer_t *t)
{
    static uint32_t s_last_gen;
    uint32_t gen = logos_generation();
    if (gen == s_last_gen) {
        return;
    }
    s_last_gen = gen;
    if (s_amb != NULL) {
        return;   /* covered by the screensaver; amb_close re-renders */
    }
    render_list_rows();
    render_right();
}

static void clock_timer_cb(lv_timer_t *t)
{
    time_t now = time(NULL);
    if (now < 1600000000) {
        return;
    }
    struct tm tm;
    home_localtime(now, &tm);
    char hm[12];
    clock_fmt(hm, sizeof(hm), tm.tm_hour, tm.tm_min);
    label_set_if_changed(s_clock_label, hm);
    if (s_amb != NULL && s_amb_clock != NULL) {
        label_set_if_changed(s_amb_clock, hm);
    }
}

static void render_list_rows(void);
static const char *ship_type_word(uint8_t t);
static bool radar_view_filter(double lat, double lon, float dist_km);

static int row_of_shown(int shown_idx)
{
    for (int r = 0; r < s_list_plane_rows; r++) {
        if (s_row_plane_idx[r] == shown_idx) {
            return r;
        }
    }
    return -1;
}
static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color);

static void ship_pop_close_cb(lv_event_t *e)
{
    lv_obj_add_flag(s_ship_pop, LV_OBJ_FLAG_HIDDEN);
}

static void ship_popup_show(const ship_t *sh)
{
    if (s_ship_pop == NULL) {
        s_ship_pop = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_ship_pop, UISX(430), UISY(210));
        lv_obj_center(s_ship_pop);
        lv_obj_set_style_bg_color(s_ship_pop, COL_ROW, 0);
        lv_obj_set_style_border_color(s_ship_pop, lv_color_hex(0x4fd1c5), 0);
        lv_obj_set_style_border_width(s_ship_pop, 2, 0);
        lv_obj_set_style_radius(s_ship_pop, UISY(14), 0);
        lv_obj_set_style_pad_all(s_ship_pop, UISY(18), 0);
        lv_obj_set_style_shadow_width(s_ship_pop, 24, 0);
        lv_obj_set_style_shadow_opa(s_ship_pop, LV_OPA_40, 0);
        lv_obj_clear_flag(s_ship_pop, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_ship_pop, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_ship_pop, ship_pop_close_cb, LV_EVENT_CLICKED, NULL);

        s_ship_pop_name = make_label(s_ship_pop, UIFONT(&lv_font_montserrat_28, &lv_font_montserrat_16), COL_TEXT);
        lv_obj_align(s_ship_pop_name, LV_ALIGN_TOP_LEFT, 0, -UISY(4));
        s_ship_pop_sub = make_label(s_ship_pop, UIFONT(&font_pl_16, &font_pl_10), lv_color_hex(0x4fd1c5));
        lv_obj_align(s_ship_pop_sub, LV_ALIGN_TOP_LEFT, 0, UISY(34));
        s_ship_pop_info = make_label(s_ship_pop, UIFONT(&font_pl_16, &font_pl_10), COL_DIM);
        lv_obj_align(s_ship_pop_info, LV_ALIGN_TOP_LEFT, 0, UISY(72));
    }
    char nm[28];
    if (sh->name[0]) {
        strlcpy(nm, sh->name, sizeof(nm));
    } else {
        snprintf(nm, sizeof(nm), "MMSI %lu", (unsigned long)sh->mmsi);
    }
    lv_label_set_text(s_ship_pop_name, nm);
    char sub[64];
    if (sh->dest[0]) {
        snprintf(sub, sizeof(sub), "%s  " LV_SYMBOL_RIGHT "  %s",
                 ship_type_word(sh->stype), sh->dest);
    } else {
        strlcpy(sub, ship_type_word(sh->stype), sizeof(sub));
    }
    lv_label_set_text(s_ship_pop_sub, sub);
    char info[160];
    snprintf(info, sizeof(info),
             "%.1f kt   %d\xC2\xB0 %s   %.1f km\n"
             "MMSI %lu\n%.4f, %.4f",
             (double)sh->sog_kt, (int)sh->cog_deg, lang_compass((int)sh->cog_deg),
             (double)sh->dist_km, (unsigned long)sh->mmsi, sh->lat, sh->lon);
    lv_label_set_text(s_ship_pop_info, info);
    lv_obj_clear_flag(s_ship_pop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ship_pop);
}

static void ship_marker_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_marker_ships != NULL && i >= 0 && i < 40) {
        ship_popup_show(&s_marker_ships[i]);
    }
}

static void list_mode_cb(lv_event_t *e)
{
    uint16_t id = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
    if (id > 2) {
        return;
    }
    s_list_mode = (uint8_t)id;
    if (s_list_mode == 0) {
        s_sel_ship_mmsi = 0;
    }
    render_list_rows();
    render_right();
}

static void row_click_cb(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx >= s_list_plane_rows) {
        int si = (int)(idx - s_list_plane_rows);
        if (si < s_row_ship_count) {
            s_sel_ship_mmsi = s_row_ships[si].mmsi;
            render_list_selection();
            if (s_view_mode != VIEW_DETAIL) {
                lv_timer_reset(s_cycle_timer);
            }
            render_right();
        }
        return;
    }
    s_sel_ship_mmsi = 0;
    if (idx >= 0 && idx < s_list_plane_rows) {
        s_selected = s_row_plane_idx[idx];
        strlcpy(s_selected_hex, s_shown[s_selected].ac.hex, sizeof(s_selected_hex));
        render_list_selection();
        if (s_view_mode != VIEW_DETAIL) {
            lv_timer_reset(s_cycle_timer);
        }
        render_right();
    }
}

static void cycle_timer_cb(lv_timer_t *t)
{
    if (s_shown_count == 0 || settings_get()->follow_mode ||
        settings_get()->pin_nearest) {
        return;
    }
    s_selected = (s_selected + 1) % s_shown_count;
    strlcpy(s_selected_hex, s_shown[s_selected].ac.hex, sizeof(s_selected_hex));
    render_list_selection();
    render_right();
    /* keep the cycled aircraft visible in the list */
    int row = row_of_shown(s_selected);
    if (row >= 0) {
        lv_obj_scroll_to_view(s_list_rows[row], LV_ANIM_ON);
    }
}

static void emb_release(void);

static void apply_view(int mode)
{
    int prev = s_view_mode;
    s_view_mode = mode % VIEW_COUNT;
    if (prev == VIEW_MAP && s_view_mode != VIEW_MAP) {
        emb_release();
    }

    lv_obj_add_flag(s_detail_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_map_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_radar_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_stats_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_retro_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_retro_timer != NULL && (s_amb == NULL || !s_amb_retro)) {
        lv_timer_pause(s_retro_timer);   /* resumed below when retro shows */
    }

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
        lv_label_set_text(s_mode_btn_label, LV_SYMBOL_EYE_OPEN);  /* next: retro */
        lv_timer_pause(s_cycle_timer);
        break;
    case VIEW_RETRO:
        lv_obj_clear_flag(s_retro_panel, LV_OBJ_FLAG_HIDDEN);
        if (s_retro_timer != NULL) {
            lv_timer_resume(s_retro_timer);
        }
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

static void mode_click_cb(lv_event_t *e)
{
    apply_view(s_view_mode + 1);
}

void ui_set_list_mode(int mode)
{
    if (mode >= 0 && mode <= 2) {
        s_list_mode = (uint8_t)mode;
        if (s_list_mode_btnm != NULL) {
            lv_btnmatrix_set_btn_ctrl(s_list_mode_btnm, (uint16_t)mode,
                                      LV_BTNMATRIX_CTRL_CHECKED);
        }
        render_list_rows();
        render_right();
    }
}

static void emb_release(void)
{
    if (!s_emb_busy && s_emb_tiles != NULL) {
        free(s_emb_tiles);
        s_emb_tiles = NULL;
        s_emb_view_ok = false;
        s_emb_key[0] = '\0';
    }
}

void ui_set_view(int mode)
{
    /* 5 and 6 are screenshot helpers for the desktop build: overlays that
     * normally need a tap (route map / photo of the selected flight) */
    if (mode == 5 || mode == 6) {
        if (s_selected >= 0 && s_selected < s_shown_count) {
            const shown_flight_t *sf = &s_shown[s_selected];
            const route_info_t *rt =
                sf->route.callsign[0] && sf->route.valid ? &sf->route : NULL;
            if (mode == 5) {
                ui_map_open(&sf->ac, rt);
            } else {
                ui_photo_open(sf->ac.hex, sf->ac.callsign);
            }
        }
        return;
    }
    apply_view(mode);
}

/* skip the realloc + invalidate when a cyclic update rewrites the same text */
static void label_set_if_changed(lv_obj_t *l, const char *txt)
{
    if (strcmp(lv_label_get_text(l), txt) != 0) {
        lv_label_set_text(l, txt);
    }
}

/* LVGL 8 style writes and lv_img_set_src have no old-value compare, so an
 * unconditional call invalidates (and repaints) even when nothing changed.
 * Reading the current value back is far cheaper than a needless redraw. */
static void text_color_if_changed(lv_obj_t *o, lv_color_t c)
{
    if (lv_obj_get_style_text_color(o, 0).full != c.full) {
        lv_obj_set_style_text_color(o, c, 0);
    }
}

static void img_recolor_if_changed(lv_obj_t *o, lv_color_t c)
{
    if (lv_obj_get_style_img_recolor(o, 0).full != c.full) {
        lv_obj_set_style_img_recolor(o, c, 0);
    }
}

static void img_src_if_changed(lv_obj_t *o, const void *src)
{
    if (lv_img_get_src(o) != src) {
        lv_img_set_src(o, src);
    }
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

/* Basemap credit line; RainViewer joins it when the rain layer is on. */
static const char *map_attribution(void)
{
    static char buf[72];
    snprintf(buf, sizeof(buf), "%s%s", tilemap_source_credit(),
             settings_get()->rain_overlay ? " \xC2\xB7 \xC2\xA9 RainViewer" : "");
    return buf;
}

/* List marker color per aircraft class (liner keeps the accent). */
static lv_color_t class_color(flight_class_t fc)
{
    switch (fc) {
    case FCLS_MIL:   return lv_color_hex(0xff6b6b);
    case FCLS_HELI:  return lv_color_hex(0xffa94d);
    case FCLS_SMALL: return lv_color_hex(0x39d98a);
    case FCLS_OTHER: return COL_DIM;
    default:         return COL_ACCENT;
    }
}

static void amb_show(void);

static void clock_click_cb(lv_event_t *e)
{
    (void)e;
    amb_show();
}

static void build_header(lv_obj_t *scr)
{
    lv_obj_t *hdr = make_panel(scr);
    lv_obj_set_size(hdr, SCR_W, HEADER_H);
    lv_obj_set_pos(hdr, 0, 0);

    s_weather_label = make_label(hdr, UIFONT(&font_pl_20, &font_pl_12), COL_ACCENT);
    lv_obj_set_width(s_weather_label, UISX(340));
    lv_label_set_long_mode(s_weather_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_weather_label, LV_SYMBOL_GPS " esp32flight");
    lv_obj_align(s_weather_label, LV_ALIGN_LEFT_MID, UISX(14), 0);

    s_clock_label = make_label(hdr, UIFONT(&font_pl_20, &font_pl_12), COL_TEXT);
    /* dead center: offset +60 collided with the status label on busy
     * headers (#33, clock painted over the city name) */
    lv_obj_align(s_clock_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_clock_label, "");
    /* tap the clock to start the screensaver right away */
    lv_obj_add_flag(s_clock_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_clock_label, UISY(14));
    lv_obj_add_event_cb(s_clock_label, clock_click_cb, LV_EVENT_CLICKED, NULL);

    s_status_label = make_label(hdr, UIFONT(&font_pl_14, &font_pl_8), COL_DIM);
    lv_obj_set_width(s_status_label, UISX(230));
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_status_label, LV_ALIGN_RIGHT_MID, -UISX(122), 0);
    lv_label_set_text(s_status_label, "...");

    lv_obj_t *gear = lv_btn_create(hdr);
    lv_obj_set_size(gear, UISX(46), UISY(36));
    lv_obj_align(gear, LV_ALIGN_RIGHT_MID, -UISX(10), 0);
    lv_obj_set_style_bg_color(gear, COL_ROW, 0);
    lv_obj_add_event_cb(gear, settings_click_cb, LV_EVENT_CLICKED, NULL);
    s_gear_label = make_label(gear, UIFONT(&lv_font_montserrat_16, &lv_font_montserrat_10), COL_TEXT);
    lv_label_set_text(s_gear_label, LV_SYMBOL_SETTINGS);
    lv_obj_center(s_gear_label);

    lv_obj_t *mode = lv_btn_create(hdr);
    lv_obj_set_size(mode, UISX(46), UISY(36));
    lv_obj_align(mode, LV_ALIGN_RIGHT_MID, -UISX(62), 0);
    lv_obj_set_style_bg_color(mode, COL_ROW, 0);
    lv_obj_add_event_cb(mode, mode_click_cb, LV_EVENT_CLICKED, NULL);
    s_mode_btn_label = make_label(mode, UIFONT(&lv_font_montserrat_16, &lv_font_montserrat_10), COL_TEXT);
    lv_label_set_text(s_mode_btn_label, LV_SYMBOL_LOOP);
    lv_obj_center(s_mode_btn_label);
}

static void build_list(lv_obj_t *scr)
{
    s_list_panel = make_panel(scr);
    lv_obj_set_size(s_list_panel, LIST_W, SCR_H - HEADER_H);
    lv_obj_set_pos(s_list_panel, 0, HEADER_H);
    lv_obj_set_style_bg_color(s_list_panel, COL_BG, 0);
    lv_obj_set_style_pad_all(s_list_panel, UISY(8), 0);
    lv_obj_set_style_pad_row(s_list_panel, UISY(6), 0);
    lv_obj_set_flex_flow(s_list_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_list_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list_panel, LV_DIR_VER);

    static const char *lm_map[4];
    lm_map[0] = L()->lm_planes;
    lm_map[1] = L()->lm_ships;
    lm_map[2] = L()->lm_all;
    lm_map[3] = "";
    s_list_mode_btnm = lv_btnmatrix_create(s_list_panel);
    lv_btnmatrix_set_map(s_list_mode_btnm, lm_map);
    lv_btnmatrix_set_one_checked(s_list_mode_btnm, true);
    lv_btnmatrix_set_btn_ctrl_all(s_list_mode_btnm, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_btn_ctrl(s_list_mode_btnm, 0, LV_BTNMATRIX_CTRL_CHECKED);
    lv_obj_set_size(s_list_mode_btnm, LIST_W - UISX(16) - UISX(8), UISY(40));
    lv_obj_set_style_bg_opa(s_list_mode_btnm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list_mode_btnm, 0, 0);
    lv_obj_set_style_pad_all(s_list_mode_btnm, 0, 0);
    lv_obj_set_style_pad_gap(s_list_mode_btnm, UISY(4), 0);
    lv_obj_set_style_bg_color(s_list_mode_btnm, COL_ROW, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_list_mode_btnm, COL_DIM, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_list_mode_btnm, COL_ACCENT,
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_list_mode_btnm, COL_BG,
                                LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_radius(s_list_mode_btnm, UISY(8), LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_list_mode_btnm, UIFONT(&font_pl_14, &font_pl_8), LV_PART_ITEMS);
    lv_obj_add_event_cb(s_list_mode_btnm, list_mode_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_flag(s_list_mode_btnm, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < MAX_SHOWN; i++) {
        lv_obj_t *row = lv_obj_create(s_list_panel);
        lv_obj_set_size(row, LIST_W - UISX(16) - UISX(8), UISY(64));
        lv_obj_set_style_bg_color(row, COL_ROW, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, UISY(8), 0);
        lv_obj_set_style_pad_all(row, UISY(8), 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *cs = make_label(row, UIFONT(&lv_font_montserrat_20, &lv_font_montserrat_12), COL_TEXT);
        lv_obj_align(cs, LV_ALIGN_TOP_LEFT, UISX(48), -2);
        lv_obj_t *type = make_label(row, UIFONT(&lv_font_montserrat_14, &lv_font_montserrat_8), COL_ACCENT);
        lv_obj_align(type, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_t *info = make_label(row, UIFONT(&lv_font_montserrat_12, &lv_font_montserrat_8), COL_DIM);
        lv_obj_align(info, LV_ALIGN_BOTTOM_LEFT, UISX(48), 2);

        lv_obj_t *route = make_label(row, UIFONT(&lv_font_montserrat_12, &lv_font_montserrat_8), COL_DIM);
        lv_obj_align(route, LV_ALIGN_BOTTOM_RIGHT, 0, 2);

        /* chip border and rounded corners are baked into the PNG assets */
        lv_obj_t *logo = lv_img_create(row);
        lv_img_set_pivot(logo, 0, 0);
        lv_img_set_zoom(logo, 256 * UISY(40) / 90);
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
    int cw = (DTL_W + 10) / 3;         /* 156 on the device */
    lv_obj_set_size(box, cw - UISX(10), s_big ? UISY(76) : UISY(56));
    lv_obj_set_pos(box, col * cw, row * (s_big ? UISY(88) : UISY(64)));
    lv_obj_set_style_bg_color(box, COL_ROW, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, UISY(8), 0);
    lv_obj_set_style_pad_all(box, UISY(8), 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *n = make_label(box, UIFONT(&font_pl_14, &font_pl_8), COL_DIM);
    lv_label_set_text(n, name);
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, -UISY(4));

    *val_out = make_label(box, s_f_val, COL_TEXT);
    lv_obj_align(*val_out, LV_ALIGN_BOTTOM_LEFT, 0, 2);
    return box;
}

/* The bundled world map is authored for the 800x480 boards; on larger panels
   it is zoomed up to fill the map area (exactly 1.0 on the original boards). */
static float emb_world_scale(void)
{
    float kx = (float)EMB_MAP_W / EMB_BASE_W;
    float ky = (float)EMB_MAP_H / EMB_BASE_H;
    return kx < ky ? kx : ky;
}

static void project_emb(double lat, double lon, lv_coord_t *x, lv_coord_t *y)
{
    if (s_emb_view_ok) {
        int xx, yy;
        tilemap_project(&s_emb_view, lat, lon, &xx, &yy);
        /* render space -> panel space (identity on 800x480) */
        *x = (lv_coord_t)(EMB_MAP_W / 2 + (xx - EMB_RENDER_W / 2) * EMB_K);
        *y = (lv_coord_t)(EMB_MAP_H / 2 + (yy - EMB_RENDER_H / 2) * EMB_K);
        return;
    }
    float k = emb_world_scale();
    *x = (lv_coord_t)(EMB_MAP_W / 2 + ((lon + 180.0) / 360.0 - 0.5) * EMB_BASE_W * k);
    *y = (lv_coord_t)(EMB_MAP_H / 2 + ((90.0 - lat) / 180.0 - 0.5) * EMB_BASE_H * k);
}

/* Cross-canvas compose instead of a dedicated scratch buffer: the radar
 * and the ambient screensaver are never visible at the same time, so
 * whichever is hidden lends its canvas as the compose target and the
 * finished frame is copied under the LVGL lock. When the borrowed canvas
 * is the ambient's, its kept frame is invalidated first (the next
 * screensaver entry re-renders from the flash tile cache in a moment).
 * This freed a full-screen buffer - the difference between the 7B
 * fitting its memory budget and not. Renders already serialize on the
 * tilemap mutex, so one borrow at a time is guaranteed. */
static uint16_t *amb_canvas_borrow(void)
{
    if (s_amb != NULL || s_amb_tiles == NULL) {
        return NULL;   /* screensaver on screen (or no canvas): not lendable */
    }
    s_amb_view_ok = false;   /* kept frame is about to be scribbled over */
    s_amb_key[0] = '\0';
    return s_amb_tiles;
}

static void emb_tiles_job(void)
{
    char key[24];
    double b[4];
    strlcpy(key, s_emb_want_key, sizeof(key));
    memcpy(b, s_emb_bbox, sizeof(b));

    uint16_t *scratch = amb_canvas_borrow();
    bool in_place = scratch == NULL;
    if (in_place) {
        scratch = s_emb_tiles;
    }
    tile_view_t view;
    bool ok = scratch != NULL &&
              tilemap_render(scratch, EMB_RENDER_W, EMB_RENDER_H,
                             b[0], b[1], b[2], b[3], &view);

    if (lvgl_port_lock(-1)) {
        if (ok && strcmp(key, s_emb_want_key) == 0) {
            if (s_emb_view_ok && strcmp(key, s_emb_key) == 0 &&
                view.missing > s_emb_missing) {
                s_emb_partial_ms = esp_timer_get_time() / 1000;   /* retry later */
                ok = false;   /* keep the more complete frame on screen */
            }
            ok = ok && s_emb_tiles != NULL;   /* boot-allocated */
        } else {
            ok = false;
        }
        if (!ok && in_place) {
            s_emb_view_ok = false;   /* canvas already overwritten: re-render */
            s_emb_key[0] = '\0';
        }
        if (ok) {
            if (!in_place) {
                memcpy(s_emb_tiles, scratch, (size_t)EMB_RENDER_W * EMB_RENDER_H * 2);
            }
            s_emb_view = view;
            s_emb_view_ok = true;
            strlcpy(s_emb_key, key, sizeof(s_emb_key));
            /* partial render (offline blip): keep it, retry in 20 s */
            s_emb_missing = view.missing;
            s_emb_partial_ms = view.missing > 0 ? esp_timer_get_time() / 1000 : 0;
            s_emb_tiles_dsc.header.always_zero = 0;
            s_emb_tiles_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
            s_emb_tiles_dsc.header.w = EMB_RENDER_W;
            s_emb_tiles_dsc.header.h = EMB_RENDER_H;
            s_emb_tiles_dsc.data = (const uint8_t *)s_emb_tiles;
            s_emb_tiles_dsc.data_size = (size_t)EMB_RENDER_W * EMB_RENDER_H * 2;
            lv_img_set_src(s_emb_img, &s_emb_tiles_dsc);
            if (EMB_MAP_W > EMB_RENDER_W) {
                lv_img_set_zoom(s_emb_img, (uint16_t)(EMB_K * 256.0f + 0.5f));
                lv_obj_set_pos(s_emb_img, EMB_MAP_W / 2 - EMB_RENDER_W / 2,
                               EMB_MAP_H / 2 - EMB_RENDER_H / 2);
            } else {
                lv_img_set_zoom(s_emb_img, LV_IMG_ZOOM_NONE);
                lv_obj_set_pos(s_emb_img, 0, 0);
            }
            lv_obj_invalidate(s_emb_img);
            if (s_view_mode == VIEW_MAP) {
                render_map_panel();
            }
        }
        lvgl_port_unlock();
    }
    s_emb_busy = false;
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
    bool emb_stale = s_emb_partial_ms != 0 &&
                     esp_timer_get_time() / 1000 - s_emb_partial_ms > 20000;
    if ((strcmp(key, s_emb_key) == 0 && !emb_stale) || s_emb_busy) {
        return;
    }

    /* fall back to the bundled map while tiles load */
    s_emb_view_ok = false;
    const lv_img_dsc_t *fallback = ui_map_get_image_small();
    if (fallback != NULL) {
        lv_img_set_src(s_emb_img, fallback);
        lv_img_set_zoom(s_emb_img, (uint16_t)(emb_world_scale() * 256.0f + 0.5f));
        lv_obj_set_pos(s_emb_img, (EMB_MAP_W - EMB_BASE_W) / 2,
                                 (EMB_MAP_H - EMB_BASE_H) / 2);
    }

    double latmin, latmax, lonmin, lonmax;
    if (rt != NULL) {
        latmin = latmax = rt->origin.lat;
        lonmin = lonmax = rt->origin.lon;
        double lats[2] = { rt->destination.lat, ac->has_pos ? ac->lat : rt->destination.lat };
        double lons[2] = { rt->destination.lon, ac->has_pos ? ac->lon : rt->destination.lon };
        for (int i = 0; i < 2; i++) {
            /* Pacific routes: keep longitudes on one continuous axis */
            lons[i] = geo_lon_unwrap(rt->origin.lon, lons[i]);
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
    if (!tilemap_worker_submit(emb_tiles_job)) {
        ESP_LOGE(TAG, "emb tiles: worker queue full");
        s_emb_busy = false;
    }
}

/* Rotatable plane sprite, tinted by altitude at render time */
static lv_obj_t *plane_img(lv_obj_t *parent)
{
    lv_obj_t *im = lv_img_create(parent);
    lv_img_set_src(im, &img_plane);
    lv_img_set_zoom(im, UIZOOM(256));
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
    lv_obj_set_size(s_map_panel, SCR_W - LIST_W, SCR_H - HEADER_H);
    lv_obj_set_pos(s_map_panel, LIST_W, HEADER_H);
    lv_obj_add_flag(s_map_panel, LV_OBJ_FLAG_HIDDEN);

    /* Bundled 490x245 world map as the instant/offline base; a worker task
     * upgrades it to a CARTO tile view zoomed to the route. */
    s_emb_img = lv_img_create(s_map_panel);
    const lv_img_dsc_t *map = ui_map_get_image_small();
    if (map != NULL) {
        lv_img_set_src(s_emb_img, map);
        lv_img_set_zoom(s_emb_img, (uint16_t)(emb_world_scale() * 256.0f + 0.5f));
    }
    lv_obj_set_pos(s_emb_img, (EMB_MAP_W - EMB_BASE_W) / 2,
                             (EMB_MAP_H - EMB_BASE_H) / 2);

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

    s_emb_line2 = lv_line_create(s_map_panel);
    lv_obj_set_style_line_width(s_emb_line2, 2, 0);
    lv_obj_set_style_line_color(s_emb_line2, COL_ACCENT, 0);
    lv_obj_set_style_line_rounded(s_emb_line2, true, 0);
    lv_obj_add_flag(s_emb_line2, LV_OBJ_FLAG_HIDDEN);

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
    lv_obj_set_size(bubble, SCR_W - LIST_W - UISX(20), SCR_H - HEADER_H - EMB_MAP_H - UISY(18));
    lv_obj_set_pos(bubble, UISX(10), EMB_MAP_H + UISY(8));
    lv_obj_set_style_bg_color(bubble, COL_ROW, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_radius(bubble, UISY(12), 0);
    lv_obj_set_style_pad_all(bubble, UISY(12), 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    s_mb_logo = lv_img_create(bubble);
    lv_img_set_pivot(s_mb_logo, 0, 0);
    lv_img_set_zoom(s_mb_logo, 256 * UISY(48) / 90);
    lv_img_set_size_mode(s_mb_logo, LV_IMG_SIZE_MODE_REAL);
    lv_obj_set_pos(s_mb_logo, 0, 0);
    lv_obj_add_flag(s_mb_logo, LV_OBJ_FLAG_HIDDEN);

    s_mb_callsign = make_label(bubble, UIFONT(&lv_font_montserrat_24, &lv_font_montserrat_14), COL_TEXT);
    lv_obj_set_pos(s_mb_callsign, UISX(60), 0);
    s_mb_type = make_label(bubble, UIFONT(&font_pl_14, &font_pl_8), COL_ACCENT);
    lv_obj_set_pos(s_mb_type, UISX(60), UISY(30));

    s_mb_route = make_label(bubble, UIFONT(&font_pl_16, &font_pl_10), COL_TEXT);
    lv_obj_set_pos(s_mb_route, 0, UISY(58));
    lv_obj_set_width(s_mb_route, SCR_W - LIST_W - UISX(20) - UISX(24));
    lv_label_set_long_mode(s_mb_route, LV_LABEL_LONG_DOT);

    s_mb_bar = lv_bar_create(bubble);
    lv_obj_set_size(s_mb_bar, SCR_W - LIST_W - UISX(20) - UISX(24), UISY(8));
    lv_obj_set_pos(s_mb_bar, 0, UISY(90));
    lv_bar_set_range(s_mb_bar, 0, 100);
    lv_obj_set_style_bg_color(s_mb_bar, COL_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_mb_bar, COL_ACCENT, LV_PART_INDICATOR);

    s_mb_stats = make_label(bubble, UIFONT(&font_pl_14, &font_pl_8), COL_DIM);
    lv_obj_set_pos(s_mb_stats, 0, UISY(106));
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
        double glat[33], glon[33];
        int split = 33;
        for (int i = 0; i < 33; i++) {
            geo_gc_point(rt->origin.lat, rt->origin.lon,
                         rt->destination.lat, rt->destination.lon,
                         (double)i / 32.0, &glat[i], &glon[i]);
            if (i > 0 && split == 33 && fabs(glon[i] - glon[i - 1]) > 180.0) {
                split = i;   /* the path crosses the antimeridian here */
            }
            project_emb(glat[i], glon[i], &x, &y);
            s_emb_pts[i].x = x;
            s_emb_pts[i].y = y;
        }
        lv_line_set_points(s_emb_line, s_emb_pts, split);
        lv_obj_clear_flag(s_emb_line, LV_OBJ_FLAG_HIDDEN);
        if (split < 33) {
            lv_line_set_points(s_emb_line2, &s_emb_pts[split], 33 - split);
            lv_obj_clear_flag(s_emb_line2, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_emb_line2, LV_OBJ_FLAG_HIDDEN);
        }

        project_emb(rt->origin.lat, rt->origin.lon, &x, &y);
        lv_obj_set_pos(s_emb_orig, x - UISX(5), y - UISY(5));
        lv_obj_clear_flag(s_emb_orig, LV_OBJ_FLAG_HIDDEN);
        project_emb(rt->destination.lat, rt->destination.lon, &x, &y);
        lv_obj_set_pos(s_emb_dest, x - UISX(5), y - UISY(5));
        lv_obj_clear_flag(s_emb_dest, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_emb_line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_emb_orig, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_emb_dest, LV_OBJ_FLAG_HIDDEN);
    }

    if (ac->has_pos) {
        project_emb(ac->lat, ac->lon, &x, &y);
        lv_obj_set_pos(s_emb_plane, x - 14, y - 14);   /* UIZOOM keeps the 28px box */
        lv_img_set_src(s_emb_plane, class_sprite(flight_sprite(ac)));
        lv_img_set_angle(s_emb_plane, (int)(ac->track_deg * 10));
        img_recolor_if_changed(s_emb_plane,
                               alt_color(ac->alt_baro_ft, ac->on_ground));
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
        char ua[20], us[20];
        snprintf(buf, sizeof(buf), "%s   %s   %d%%, %.0f km to go   %s",
                 units_alt(ac->alt_baro_ft, ua, sizeof(ua)),
                 units_speed(ac->gs_kts, us, sizeof(us)),
                 (int)(prog * 100.0), remaining, eta);
        lv_label_set_text(s_mb_stats, buf);
    } else {
        lv_label_set_text(s_mb_route, L()->route_unknown);
        lv_bar_set_value(s_mb_bar, 0, LV_ANIM_OFF);
        char ua[20], us[20];
        snprintf(buf, sizeof(buf), "%s   %s   %.1f km away",
                 units_alt(ac->alt_baro_ft, ua, sizeof(ua)),
                 units_speed(ac->gs_kts, us, sizeof(us)),
                 ac->dist_nm >= 0 ? ac->dist_nm * 1.852 : 0.0);
        lv_label_set_text(s_mb_stats, buf);
    }
}

static void radar_tiles_job(void)
{
    char key[48];
    double b[4];
    strlcpy(key, s_radar_want, sizeof(key));
    memcpy(b, s_radar_bbox, sizeof(b));

    /* ambient hidden -> borrow its canvas (flicker-free swap below);
     * ambient covering the screen -> the radar panel is invisible, so
     * composing straight into the radar canvas cannot be seen */
    uint16_t *scratch = amb_canvas_borrow();
    bool in_place = scratch == NULL;
    if (in_place) {
        scratch = s_radar_tiles;
    }
    tile_view_t view;
    bool ok = scratch != NULL &&
              tilemap_render(scratch, RADAR_RENDER_W, RADAR_RENDER_H,
                             b[0], b[1], b[2], b[3], &view);
    if (ok) {
        runways_draw(scratch, RADAR_RENDER_W, RADAR_RENDER_H, &view);
    }
    /* integer tile zooms leave dead margin around the range ring; upscale
       so the ring (divided by the user zoom) fills most of the height */
    float scale = 1.0f;
    int shx = 0, shy = 0;
    if (ok) {
        int ex, ey;
        tilemap_project(&view, s_home_lat, s_home_lon, &shx, &shy);
        tilemap_project(&view, s_home_lat, b[3], &ex, &ey);
        float r = (float)(ex - shx);
        float target = 0.92f * (RADAR_RENDER_H / 2) / (float)s_radar_zoom;
        if (r > 16.0f && target / r > 1.05f) {
            scale = target / r;
            if (scale > 2.6f) {
                scale = 2.6f;
            }
            fb_upscale(scratch, RADAR_RENDER_W, RADAR_RENDER_H, shx, shy, scale);
        }
    }

    if (lvgl_port_lock(-1)) {
        if (ok && strcmp(key, s_radar_want) == 0) {
            if (s_radar_view_ok && strcmp(key, s_radar_key) == 0 &&
                view.missing > s_radar_missing) {
                s_radar_partial_ms = esp_timer_get_time() / 1000;   /* retry later */
                ok = false;   /* keep the more complete frame on screen */
            }
            ok = ok && s_radar_tiles != NULL;   /* boot-allocated */
        } else {
            ok = false;
        }
        /* A failed in-place render (screensaver up, decode out of memory
         * on the 7B) has already scribbled over the displayed canvas. The
         * view key used to stay, so the map was never rendered again and
         * stayed black until a reboot (Discord report, 7B). Forget it so
         * the next pass renders afresh. */
        if (!ok && in_place) {
            s_radar_view_ok = false;
            s_radar_key[0] = '\0';
        }
        if (ok) {
            if (!in_place) {
                memcpy(s_radar_tiles, scratch, (size_t)RADAR_RENDER_W * RADAR_RENDER_H * 2);
            }
            s_radar_view = view;
            s_radar_view_ok = true;
            s_radar_scale = scale;
            s_radar_spx = shx;
            s_radar_spy = shy;
            strlcpy(s_radar_key, key, sizeof(s_radar_key));
            /* partial render (offline blip): keep it, retry in 20 s */
            s_radar_missing = view.missing;
            s_radar_partial_ms = view.missing > 0 ? esp_timer_get_time() / 1000 : 0;
            s_radar_done_ms = esp_timer_get_time() / 1000;
            s_radar_tiles_dsc.header.always_zero = 0;
            s_radar_tiles_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
            s_radar_tiles_dsc.header.w = RADAR_RENDER_W;
            s_radar_tiles_dsc.header.h = RADAR_RENDER_H;
            s_radar_tiles_dsc.data = (const uint8_t *)s_radar_tiles;
            s_radar_tiles_dsc.data_size = (size_t)RADAR_RENDER_W * RADAR_RENDER_H * 2;
            lv_img_set_src(s_radar_img, &s_radar_tiles_dsc);
            if (RADAR_W > RADAR_RENDER_W) {
                lv_img_set_zoom(s_radar_img, (uint16_t)(RADAR_K * 256.0f + 0.5f));
                lv_obj_set_pos(s_radar_img, RADAR_W / 2 - RADAR_RENDER_W / 2,
                               RADAR_H / 2 - RADAR_RENDER_H / 2);
            }
            lv_obj_invalidate(s_radar_img);
            if (s_view_mode == VIEW_RADAR) {
                render_radar_panel();
                render_list_rows();
            }
        }
        lvgl_port_unlock();
    }
    s_radar_busy = false;
}

static void radar_tiles_want(void)
{
    if (!s_home_ok || s_radar_busy) {
        return;
    }
    int radius_nm = settings_get()->radius_nm;
    char key[48];
    snprintf(key, sizeof(key), "%.3f,%.3f,%d,%.2f", s_home_lat, s_home_lon,
             radius_nm, s_radar_zoom);
    int64_t now_ms = esp_timer_get_time() / 1000;
    bool radar_stale = s_radar_partial_ms != 0 && now_ms - s_radar_partial_ms > 20000;
    /* The rain frame is baked into the rendered tiles, so an unchanged
     * view kept the weather of its first render forever (#46). Recompose
     * every 10 minutes while the overlay is on, like the screensaver. */
    bool rain_stale = settings_get()->rain_overlay && s_radar_view_ok &&
                      s_radar_done_ms != 0 && now_ms - s_radar_done_ms > 10 * 60 * 1000;
    if (strcmp(key, s_radar_key) == 0 && !radar_stale && !rain_stale) {
        return;
    }
    double rkm = radius_nm * 1.852 * s_radar_zoom;
    if (rkm < 2.0) {
        rkm = 2.0;
    }
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
    if (!tilemap_worker_submit(radar_tiles_job)) {
        ESP_LOGE(TAG, "radar tiles: worker queue full");
        s_radar_busy = false;
    }
}

static void radar_zoom_cb(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    double z = s_radar_zoom * (dir > 0 ? 1.0 / 1.5 : 1.5);
    if (z < 0.18) {
        z = 0.18;
    }
    if (z > 3.4) {
        z = 3.4;
    }
    if (z > 0.95 && z < 1.06) {
        z = 1.0;   /* snap back to the exact radius fit */
    }
    s_radar_zoom = z;
    radar_tiles_want();
}

static void radar_dot_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_all_count || s_all[i].callsign[0] == '\0') {
        return;
    }
    for (int j = 0; j < s_shown_count; j++) {
        if (strcmp(s_shown[j].ac.callsign, s_all[i].callsign) == 0) {
            if (j == s_selected && s_sel_ship_mmsi == 0) {
                /* second tap on the selected plane toggles the bubble away */
                s_radar_bub_off = !s_radar_bub_off;
                render_radar_panel();
                return;
            }
            s_radar_bub_off = false;
            s_sel_ship_mmsi = 0;
            s_selected = j;
            strlcpy(s_selected_hex, s_shown[j].ac.hex, sizeof(s_selected_hex));
            render_list_selection();
            int row = row_of_shown(j);
            if (row >= 0) {
                lv_obj_scroll_to_view(s_list_rows[row], LV_ANIM_ON);
            }
            render_radar_panel();
            lv_timer_reset(s_cycle_timer);
            return;
        }
    }
}

static void build_radar_panel(lv_obj_t *scr)
{
    s_radar_panel = make_panel(scr);
    lv_obj_set_size(s_radar_panel, RADAR_W, RADAR_H);
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
    lv_obj_set_size(home, UISX(10), UISY(10));
    lv_obj_set_pos(home, RADAR_CX - UISX(5), RADAR_CY - UISY(5));
    lv_obj_set_style_radius(home, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(home, COL_ACCENT, 0);
    lv_obj_set_style_border_width(home, 0, 0);
    lv_obj_clear_flag(home, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_radar_home = home;

    for (int i = 0; i < MAX_AIRCRAFT; i++) {
        s_radar_dots[i] = plane_img(s_radar_panel);
        lv_obj_add_flag(s_radar_dots[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(s_radar_dots[i], 8);
        lv_obj_add_event_cb(s_radar_dots[i], radar_dot_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }

    s_radar_bub = lv_obj_create(s_radar_panel);
    lv_obj_set_size(s_radar_bub, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_radar_bub, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(s_radar_bub, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_radar_bub, 0, 0);
    lv_obj_set_style_pad_all(s_radar_bub, UISY(6), 0);
    lv_obj_set_style_pad_column(s_radar_bub, UISX(8), 0);
    lv_obj_set_style_radius(s_radar_bub, UISY(6), 0);
    lv_obj_set_flex_flow(s_radar_bub, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_radar_bub, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_radar_bub, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_radar_bub, LV_OBJ_FLAG_HIDDEN);
    s_radar_blogo = lv_img_create(s_radar_bub);
    lv_img_set_pivot(s_radar_blogo, 0, 0);
    lv_img_set_zoom(s_radar_blogo, 256 * UISY(36) / 90);
    lv_img_set_size_mode(s_radar_blogo, LV_IMG_SIZE_MODE_REAL);
    lv_obj_add_flag(s_radar_blogo, LV_OBJ_FLAG_HIDDEN);
    s_radar_info = make_label(s_radar_bub, UIFONT(&font_pl_14, &font_pl_8), COL_TEXT);
    lv_label_set_text(s_radar_info, "");

    lv_obj_t *rattr = make_label(s_radar_panel, UIFONT(&font_pl_14, &font_pl_8), COL_DIM);
    lv_label_set_text(rattr, map_attribution());
    lv_obj_align(rattr, LV_ALIGN_BOTTOM_LEFT, UISX(6), -2);

    static const char *rzsym[2] = { LV_SYMBOL_PLUS, LV_SYMBOL_MINUS };
    for (int zi = 0; zi < 2; zi++) {
        lv_obj_t *zb = lv_btn_create(s_radar_panel);
        lv_obj_set_size(zb, UISX(52), UISY(44));
        lv_obj_align(zb, LV_ALIGN_BOTTOM_RIGHT, -UISX(10), -UISY(78) + zi * UISY(52));
        lv_obj_set_style_bg_color(zb, COL_ACCENT, 0);
        lv_obj_set_style_bg_opa(zb, LV_OPA_90, 0);
        lv_obj_set_style_shadow_width(zb, 12, 0);
        lv_obj_set_style_shadow_opa(zb, LV_OPA_40, 0);
        lv_obj_add_event_cb(zb, radar_zoom_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)(zi == 0 ? 1 : -1));
        lv_obj_t *zl = lv_label_create(zb);
        lv_obj_set_style_text_color(zl, COL_BG, 0);
        lv_label_set_text(zl, rzsym[zi]);
        lv_obj_center(zl);
    }

    s_radar_range = make_label(s_radar_panel, UIFONT(&font_pl_14, &font_pl_8), COL_DIM);
    lv_obj_align(s_radar_range, LV_ALIGN_BOTTOM_RIGHT, -UISX(10), -UISY(6));
    lv_label_set_text(s_radar_range, "");
}

/* ---------- optional extra objects: ISS, radiosondes, AIS ships ---------- */

#define MAX_SHIP_MARKERS 40

static lv_obj_t *s_x_iss_dot, *s_x_iss_lbl, *s_x_iss_status;
static lv_obj_t *s_x_sonde_dot[MAX_SONDES], *s_x_sonde_lbl[MAX_SONDES];
static lv_obj_t *s_x_ship_dot[MAX_SHIP_MARKERS], *s_x_ship_lbl[MAX_SHIP_MARKERS];

/* overlapping ship labels collapse into count badges with a tap-list */
#define MAX_SHIP_GROUPS 8
#define GRP_MEMBERS     48
#define CLUSTER_PX      UISX(46)
static lv_obj_t *s_x_grp[MAX_SHIP_GROUPS];
static ship_t (*s_grp_ships)[GRP_MEMBERS];   /* PSRAM, MAX_SHIP_GROUPS rows */
static int s_grp_count[MAX_SHIP_GROUPS];
static lv_obj_t *s_grp_pop;
static ship_t *s_pop_ships;                  /* PSRAM */
static int s_pop_count;

static bool view_shows_planes(void)
{
    return !(settings_get()->ships_enabled && s_list_mode == 1);
}

static bool view_shows_ships(void)
{
    return settings_get()->ships_enabled && s_list_mode != 0;
}

/* 64-slot ship array is too big for a task stack; both users of this
   buffer run in the LVGL task, so one shared PSRAM block is enough */
static ship_t *ui_ship_buf(void)
{
    static ship_t *buf;
    if (buf == NULL) {
        buf = heap_caps_malloc(MAX_SHIPS * sizeof(ship_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return buf;
}

static lv_obj_t *extra_dot(lv_color_t color, int size, int radius)
{
    lv_obj_t *d = lv_obj_create(s_radar_panel);
    lv_obj_set_size(d, size, size);
    lv_obj_set_style_radius(d, radius, 0);
    lv_obj_set_style_bg_color(d, color, 0);
    lv_obj_set_style_border_width(d, 1, 0);
    lv_obj_set_style_border_color(d, lv_color_white(), 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    return d;
}

static lv_obj_t *extra_lbl(lv_color_t color)
{
    lv_obj_t *l = lv_label_create(s_radar_panel);
    lv_obj_set_style_text_font(l, UIFONT(&font_pl_14, &font_pl_8), 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

/* tilemap projection plus the fill-the-panel upscale */
static void radar_project(double lat, double lon, int *x, int *y)
{
    tilemap_project(&s_radar_view, lat, lon, x, y);
    if (s_radar_scale != 1.0f) {
        *x = s_radar_spx + (int)((*x - s_radar_spx) * s_radar_scale);
        *y = s_radar_spy + (int)((*y - s_radar_spy) * s_radar_scale);
    }
    /* render space -> panel space (identity on 800x480) */
    *x = RADAR_W / 2 + (int)((*x - RADAR_RENDER_W / 2) * RADAR_K);
    *y = RADAR_H / 2 + (int)((*y - RADAR_RENDER_H / 2) * RADAR_K);
}

/* place a lat/lon at dist_km from home onto the radar; false = outside */
static bool radar_place(double lat, double lon, float dist_km,
                        bool map_mode, int radius_nm, int *x, int *y)
{
    if (map_mode) {
        radar_project(lat, lon, x, y);
        return *x >= -10 && *x <= RADAR_W + 10 && *y >= -10 && *y <= RADAR_H + 10;
    }
    float frac = dist_km / (radius_nm * 1.852f);
    if (frac > 1.0f) {
        return false;
    }
    float rad = (float)geo_bearing_deg(s_home_lat, s_home_lon, lat, lon) *
                (float)M_PI / 180.0f;
    *x = RADAR_CX + (int)(sinf(rad) * frac * RADAR_R);
    *y = RADAR_CY - (int)(cosf(rad) * frac * RADAR_R);
    return true;
}

static lv_obj_t *s_x_asp_line[MAX_AIRSPACES];
static lv_obj_t *s_x_asp_lbl[MAX_AIRSPACES];
static lv_point_t (*s_x_asp_pts)[MAX_ASP_POINTS + 1];

static lv_color_t airspace_color(int type)
{
    switch (type) {
    case 4: case 27:          return lv_color_hex(0xe06666);  /* CTR/MCTR */
    case 7: case 26: case 13: return lv_color_hex(0x6fa8dc);  /* TMA/MTMA/ATZ */
    default:                  return lv_color_hex(0xe6b34d);  /* R/D/P/TRA */
    }
}

static void render_airspaces(bool map_mode)
{
    int n = map_mode ? airspace_count() : 0;
    if (n > 0 && s_x_asp_pts == NULL) {
        s_x_asp_pts = heap_caps_calloc(MAX_AIRSPACES,
                                       sizeof(*s_x_asp_pts),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_x_asp_pts == NULL) {
            return;
        }
    }
    for (int i = 0; i < MAX_AIRSPACES; i++) {
        bool show = i < n;
        const airspace_t *a = show ? airspace_get(i) : NULL;
        if (a == NULL) {
            if (s_x_asp_line[i] != NULL) {
                lv_obj_add_flag(s_x_asp_line[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_x_asp_lbl[i], LV_OBJ_FLAG_HIDDEN);
            }
            continue;
        }
        if (s_x_asp_line[i] == NULL) {
            s_x_asp_line[i] = lv_line_create(s_radar_panel);
            lv_obj_set_style_line_width(s_x_asp_line[i], 1, 0);
            lv_obj_set_style_line_dash_width(s_x_asp_line[i], 4, 0);
            lv_obj_set_style_line_dash_gap(s_x_asp_line[i], 4, 0);
            lv_obj_set_style_line_opa(s_x_asp_line[i], LV_OPA_70, 0);
            lv_obj_clear_flag(s_x_asp_line[i], LV_OBJ_FLAG_CLICKABLE);
            s_x_asp_lbl[i] = extra_lbl(lv_color_white());
        }
        lv_color_t col = airspace_color(a->type);
        lv_obj_set_style_line_color(s_x_asp_line[i], col, 0);
        lv_obj_set_style_text_color(s_x_asp_lbl[i], col, 0);
        int m = 0, lx = 0, ly = 0;
        for (int k = 0; k < a->n_points; k++) {
            int x, y;
            radar_project(a->lat[k], a->lon[k], &x, &y);
            /* keep coordinates sane for lvgl even when far off screen */
            if (x < -2000) x = -2000;
            if (x > 2000) x = 2000;
            if (y < -2000) y = -2000;
            if (y > 2000) y = 2000;
            s_x_asp_pts[i][m].x = (lv_coord_t)x;
            s_x_asp_pts[i][m].y = (lv_coord_t)y;
            if (m == 0) {
                lx = x;
                ly = y;
            }
            m++;
        }
        /* close the ring */
        s_x_asp_pts[i][m] = s_x_asp_pts[i][0];
        m++;
        lv_line_set_points(s_x_asp_line[i], s_x_asp_pts[i], m);
        lv_obj_clear_flag(s_x_asp_line[i], LV_OBJ_FLAG_HIDDEN);
        if (lx >= 0 && lx <= RADAR_W - 40 && ly >= 8 && ly <= RADAR_H - 20) {
            lv_label_set_text(s_x_asp_lbl[i], airspace_type_str(a->type));
            lv_obj_set_pos(s_x_asp_lbl[i], lx + UISX(3), ly - UISY(16));
            lv_obj_clear_flag(s_x_asp_lbl[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_x_asp_lbl[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void grp_pop_close_cb(lv_event_t *e)
{
    lv_obj_add_flag(s_grp_pop, LV_OBJ_FLAG_HIDDEN);
}

static void grp_row_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_add_flag(s_grp_pop, LV_OBJ_FLAG_HIDDEN);
    if (k >= 0 && k < s_pop_count) {
        ship_popup_show(&s_pop_ships[k]);
    }
}

static void ship_group_cb(lv_event_t *e)
{
    int g = (int)(intptr_t)lv_event_get_user_data(e);
    if (g < 0 || g >= MAX_SHIP_GROUPS || s_grp_count[g] == 0) {
        return;
    }
    if (s_pop_ships == NULL) {
        return;
    }
    s_pop_count = s_grp_count[g] < GRP_MEMBERS ? s_grp_count[g] : GRP_MEMBERS;
    memcpy(s_pop_ships, s_grp_ships[g], s_pop_count * sizeof(ship_t));
    for (int i = 1; i < s_pop_count; i++) {
        ship_t tmp = s_pop_ships[i];
        int j = i - 1;
        while (j >= 0 && s_pop_ships[j].dist_km > tmp.dist_km) {
            s_pop_ships[j + 1] = s_pop_ships[j];
            j--;
        }
        s_pop_ships[j + 1] = tmp;
    }

    if (s_grp_pop == NULL) {
        s_grp_pop = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_grp_pop, UISX(470), UISY(330));
        lv_obj_center(s_grp_pop);
        lv_obj_set_style_bg_color(s_grp_pop, COL_ROW, 0);
        lv_obj_set_style_border_color(s_grp_pop, lv_color_hex(0x4fd1c5), 0);
        lv_obj_set_style_border_width(s_grp_pop, 2, 0);
        lv_obj_set_style_radius(s_grp_pop, UISY(14), 0);
        lv_obj_set_style_pad_all(s_grp_pop, UISY(12), 0);
        lv_obj_set_style_pad_row(s_grp_pop, UISY(4), 0);
        lv_obj_set_flex_flow(s_grp_pop, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(s_grp_pop, LV_DIR_VER);
        lv_obj_add_event_cb(s_grp_pop, grp_pop_close_cb, LV_EVENT_CLICKED, NULL);
    }
    lv_obj_clean(s_grp_pop);
    lv_obj_t *title = make_label(s_grp_pop, UIFONT(&font_pl_16, &font_pl_10), lv_color_hex(0x4fd1c5));
    lv_label_set_text_fmt(title, "%s \xC2\xB7 %d", L()->lm_ships, s_pop_count);
    for (int k = 0; k < s_pop_count; k++) {
        lv_obj_t *row = lv_obj_create(s_grp_pop);
        lv_obj_set_size(row, LV_PCT(100), UISY(44));
        lv_obj_set_style_bg_color(row, COL_PANEL, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, UISY(8), 0);
        lv_obj_set_style_pad_all(row, UISY(8), 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, grp_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);
        lv_obj_t *nm = make_label(row, UIFONT(&font_pl_16, &font_pl_10), COL_TEXT);
        char nb[26];
        if (s_pop_ships[k].name[0]) {
            strlcpy(nb, s_pop_ships[k].name, sizeof(nb));
        } else {
            snprintf(nb, sizeof(nb), "%lu", (unsigned long)s_pop_ships[k].mmsi);
        }
        lv_label_set_text(nm, nb);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *inf = make_label(row, UIFONT(&font_pl_14, &font_pl_8), COL_DIM);
        char ib[52], us[20];
        snprintf(ib, sizeof(ib), "%s  %s  %.1f km",
                 ship_type_word(s_pop_ships[k].stype),
                 units_speed(s_pop_ships[k].sog_kt, us, sizeof(us)),
                 (double)s_pop_ships[k].dist_km);
        lv_label_set_text(inf, ib);
        lv_obj_align(inf, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    lv_obj_clear_flag(s_grp_pop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_grp_pop);
}

static void render_radar_extras(bool map_mode, int radius_nm)
{
    if (s_x_iss_dot == NULL) {
        s_x_iss_dot = extra_dot(lv_color_hex(0x9be8ff), 12, LV_RADIUS_CIRCLE);
        s_x_iss_lbl = extra_lbl(lv_color_hex(0x9be8ff));
        s_x_iss_status = extra_lbl(lv_color_hex(0x9be8ff));
        lv_obj_align(s_x_iss_status, LV_ALIGN_TOP_RIGHT, -UISX(8), UISY(6));
        for (int i = 0; i < MAX_SONDES; i++) {
            s_x_sonde_dot[i] = extra_dot(lv_color_hex(0xffb347), 10, LV_RADIUS_CIRCLE);
            s_x_sonde_lbl[i] = extra_lbl(lv_color_hex(0xffb347));
        }
        for (int g = 0; g < MAX_SHIP_GROUPS; g++) {
            lv_obj_t *b = lv_obj_create(s_radar_panel);
            lv_obj_set_size(b, LV_SIZE_CONTENT, UISY(30));
            lv_obj_set_style_bg_color(b, lv_color_hex(0x123a36), 0);
            lv_obj_set_style_border_color(b, lv_color_hex(0x4fd1c5), 0);
            lv_obj_set_style_border_width(b, 1, 0);
            lv_obj_set_style_radius(b, UISY(15), 0);
            lv_obj_set_style_pad_hor(b, UISX(10), 0);
            lv_obj_set_style_pad_ver(b, UISY(4), 0);
            lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(b, ship_group_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)g);
            lv_obj_t *bl = make_label(b, UIFONT(&font_pl_14, &font_pl_8), lv_color_hex(0x4fd1c5));
            lv_obj_center(bl);
            lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
            s_x_grp[g] = b;
        }
        for (int i = 0; i < MAX_SHIP_MARKERS; i++) {
            s_x_ship_dot[i] = extra_dot(lv_color_hex(0x4fd1c5), 9, 2);
            lv_obj_add_flag(s_x_ship_dot[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(s_x_ship_dot[i], 10);
            lv_obj_add_event_cb(s_x_ship_dot[i], ship_marker_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);
            s_x_ship_lbl[i] = extra_lbl(lv_color_hex(0x4fd1c5));
        }
    }

    int x, y;
    iss_state_t iss;
    bool iss_shown = false, iss_status = false;
    if (extras_get_iss(&iss)) {
        if (radar_place(iss.lat, iss.lon, iss.dist_km, map_mode, radius_nm, &x, &y)) {
            lv_obj_set_pos(s_x_iss_dot, x - UISX(6), y - UISY(6));
            lv_label_set_text(s_x_iss_lbl, "ISS");
            lv_obj_set_pos(s_x_iss_lbl, x + UISX(9), y - UISY(8));
            iss_shown = true;
        } else if (iss.elev_deg > 0) {
            lv_label_set_text_fmt(s_x_iss_status, "ISS %d\xC2\xB0 %s",
                                  (int)iss.elev_deg, lang_compass((int)iss.az_deg));
            iss_status = true;
        }
    }
    lv_obj_add_flag(s_x_iss_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_x_iss_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_x_iss_status, LV_OBJ_FLAG_HIDDEN);
    if (iss_shown) {
        lv_obj_clear_flag(s_x_iss_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_x_iss_lbl, LV_OBJ_FLAG_HIDDEN);
    } else if (iss_status) {
        lv_obj_clear_flag(s_x_iss_status, LV_OBJ_FLAG_HIDDEN);
    }

    sonde_t sondes[MAX_SONDES];
    int ns = extras_get_sondes(sondes, MAX_SONDES);
    for (int i = 0; i < MAX_SONDES; i++) {
        bool show = i < ns &&
                    radar_place(sondes[i].lat, sondes[i].lon, sondes[i].dist_km,
                                map_mode, radius_nm, &x, &y);
        if (show) {
            lv_obj_set_pos(s_x_sonde_dot[i], x - UISX(5), y - UISY(5));
            char slbl[48];
            snprintf(slbl, sizeof(slbl), "%s %s%.1f km",
                     sondes[i].type[0] ? sondes[i].type : "sonde",
                     sondes[i].vel_v >= 0 ? LV_SYMBOL_UP " " : LV_SYMBOL_DOWN " ",
                     (double)(sondes[i].alt_m / 1000.0f));
            lv_label_set_text(s_x_sonde_lbl[i], slbl);
            lv_obj_set_pos(s_x_sonde_lbl[i], x + UISX(8), y - UISY(8));
            lv_obj_clear_flag(s_x_sonde_dot[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_x_sonde_lbl[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_x_sonde_dot[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_x_sonde_lbl[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    ship_t *shp = ui_ship_buf();
    int nsh = (shp != NULL && view_shows_ships()) ? ships_get(shp, MAX_SHIPS) : 0;

    /* place everything first, then collapse overlaps into count badges */
    static int px[MAX_SHIPS], py[MAX_SHIPS];
    static bool used[MAX_SHIPS];
    int placed_idx[MAX_SHIPS];
    int placed = 0;
    for (int i = 0; i < nsh; i++) {
        if (radar_place(shp[i].lat, shp[i].lon, shp[i].dist_km,
                        map_mode, radius_nm, &x, &y)) {
            px[placed] = x;
            py[placed] = y;
            placed_idx[placed] = i;
            used[placed] = false;
            placed++;
        }
    }
    if (s_grp_ships == NULL) {
        s_grp_ships = heap_caps_malloc(MAX_SHIP_GROUPS * sizeof(*s_grp_ships),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    /* pass 1: proximity groups; pass 2: merge groups whose badges
       would overlap; then emit singles + badges */
    static int gid[MAX_SHIPS];
    static long gx[MAX_SHIPS], gy[MAX_SHIPS];
    static int gcnt[MAX_SHIPS];
    int ng = 0;
    for (int i = 0; i < placed; i++) {
        gid[i] = -1;
    }
    for (int i = 0; i < placed; i++) {
        if (gid[i] >= 0) {
            continue;
        }
        gid[i] = ng;
        gx[ng] = px[i];
        gy[ng] = py[i];
        gcnt[ng] = 1;
        for (int j = i + 1; j < placed; j++) {
            if (gid[j] < 0 &&
                abs(px[j] - (int)(gx[ng] / gcnt[ng])) < CLUSTER_PX &&
                abs(py[j] - (int)(gy[ng] / gcnt[ng])) < CLUSTER_PX) {
                gid[j] = ng;
                gx[ng] += px[j];
                gy[ng] += py[j];
                gcnt[ng]++;
            }
        }
        ng++;
    }
    /* merge until no two multi-ship badges overlap (badge ~120x40 px) */
    bool merged = true;
    while (merged) {
        merged = false;
        for (int a = 0; a < ng && !merged; a++) {
            if (gcnt[a] < 2) {
                continue;
            }
            for (int b = a + 1; b < ng; b++) {
                if (gcnt[b] < 1 || (gcnt[b] == 1 && gcnt[a] == 1)) {
                    continue;
                }
                if (gcnt[b] >= 2 || gcnt[a] >= 2) {
                    int ax = (int)(gx[a] / gcnt[a]), ay = (int)(gy[a] / gcnt[a]);
                    int bx = (int)(gx[b] / gcnt[b]), by = (int)(gy[b] / gcnt[b]);
                    if (abs(ax - bx) < 124 && abs(ay - by) < 42) {
                        for (int i = 0; i < placed; i++) {
                            if (gid[i] == b) {
                                gid[i] = a;
                            }
                        }
                        gx[a] += gx[b];
                        gy[a] += gy[b];
                        gcnt[a] += gcnt[b];
                        gcnt[b] = 0;
                        merged = true;
                        break;
                    }
                }
            }
        }
    }
    int drawn = 0, groups = 0;
    for (int g = 0; g < ng; g++) {
        if (gcnt[g] == 0) {
            continue;
        }
        if (gcnt[g] == 1) {
            for (int i = 0; i < placed && drawn < MAX_SHIP_MARKERS; i++) {
                if (gid[i] != g) {
                    continue;
                }
                const ship_t *sh1 = &shp[placed_idx[i]];
                if (s_marker_ships == NULL) {
                    break;
                }
                lv_obj_set_pos(s_x_ship_dot[drawn], px[i] - UISX(4), py[i] - UISY(4));
                s_marker_ships[drawn] = *sh1;
                lv_label_set_text(s_x_ship_lbl[drawn],
                                  sh1->name[0] ? sh1->name : "ship");
                lv_obj_set_pos(s_x_ship_lbl[drawn], px[i] + UISX(8), py[i] - UISY(8));
                lv_obj_clear_flag(s_x_ship_dot[drawn], LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(s_x_ship_lbl[drawn], LV_OBJ_FLAG_HIDDEN);
                drawn++;
                break;
            }
            continue;
        }
        if (groups >= MAX_SHIP_GROUPS || s_grp_ships == NULL) {
            continue;
        }
        int m = 0;
        for (int i = 0; i < placed; i++) {
            if (gid[i] == g && m < GRP_MEMBERS) {
                s_grp_ships[groups][m++] = shp[placed_idx[i]];
            }
        }
        s_grp_count[groups] = m;
        lv_obj_t *bl = lv_obj_get_child(s_x_grp[groups], 0);
        lv_label_set_text_fmt(bl, "%s \xC2\xB7 %d", L()->lm_ships, gcnt[g]);
        lv_obj_update_layout(s_x_grp[groups]);
        lv_obj_set_pos(s_x_grp[groups],
                       (int)(gx[g] / gcnt[g]) - lv_obj_get_width(s_x_grp[groups]) / 2,
                       (int)(gy[g] / gcnt[g]) - UISY(15));
        lv_obj_clear_flag(s_x_grp[groups], LV_OBJ_FLAG_HIDDEN);
        groups++;
    }
    for (int i = drawn; i < MAX_SHIP_MARKERS; i++) {
        lv_obj_add_flag(s_x_ship_dot[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_x_ship_lbl[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int g = groups; g < MAX_SHIP_GROUPS; g++) {
        lv_obj_add_flag(s_x_grp[g], LV_OBJ_FLAG_HIDDEN);
        s_grp_count[g] = 0;
    }
}

static void render_radar_panel(void)
{
    radar_tiles_want();

    int radius_nm = settings_get()->radius_nm;
    bool map_mode = s_radar_view_ok;
    int hx = RADAR_CX, hy = RADAR_CY;
    int rpx = RADAR_R;

    if (map_mode) {
        radar_project(s_home_lat, s_home_lon, &hx, &hy);
        int ex, ey;
        radar_project(s_home_lat, s_radar_bbox[3], &ex, &ey);
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
        lv_obj_set_pos(s_radar_home, hx - UISX(5), hy - UISY(5));
        lv_label_set_text_fmt(s_radar_range, "%d km", (int)(radius_nm * 1.852));
    } else {
        lv_obj_add_flag(s_radar_img, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 3; i++) {
            int r = RADAR_R * (i + 1) / 3;
            lv_obj_clear_flag(s_radar_rings[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(s_radar_rings[i], r * 2, r * 2);
            lv_obj_set_pos(s_radar_rings[i], RADAR_CX - r, RADAR_CY - r);
        }
        lv_obj_set_pos(s_radar_home, RADAR_CX - UISX(5), RADAR_CY - UISY(5));
        lv_label_set_text_fmt(s_radar_range, L()->ring_fmt, (int)(radius_nm * 1.852 / 3));
    }

    const aircraft_t *selac = (s_selected >= 0 && s_selected < s_shown_count)
                                  ? &s_shown[s_selected].ac : NULL;
    bool planes_on = view_shows_planes();
    for (int i = 0; i < MAX_AIRCRAFT; i++) {
        if (!planes_on || i >= s_all_count || s_all[i].dist_nm < 0) {
            lv_obj_add_flag(s_radar_dots[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const amb_target_t *t = &s_all[i];
        int x, y;
        if (map_mode) {
            radar_project(t->lat, t->lon, &x, &y);
            if (x < -14 || x > RADAR_W + 14 || y < -14 || y > RADAR_H + 14) {
                lv_obj_add_flag(s_radar_dots[i], LV_OBJ_FLAG_HIDDEN);
                continue;
            }
        } else {
            float frac = t->dist_nm / (float)radius_nm;
            if (frac > 1.0f) {
                frac = 1.0f;
            }
            float rad = t->dir_deg * (float)M_PI / 180.0f;
            x = RADAR_CX + (int)(sinf(rad) * frac * RADAR_R);
            y = RADAR_CY - (int)(cosf(rad) * frac * RADAR_R);
        }
        bool sel = selac != NULL && selac->callsign[0] &&
                   strcmp(t->callsign, selac->callsign) == 0;
        lv_obj_set_pos(s_radar_dots[i], x - 14, y - 14);   /* UIZOOM keeps the 28px box */
        img_src_if_changed(s_radar_dots[i], class_sprite(t->fcls));
        lv_img_set_angle(s_radar_dots[i], (int)(t->track * 10));
        lv_img_set_zoom(s_radar_dots[i], sel ? UIZOOM(384) : UIZOOM(232));
        img_recolor_if_changed(s_radar_dots[i],
                               alt_color(t->alt_ft, t->ground));
        lv_obj_clear_flag(s_radar_dots[i], LV_OBJ_FLAG_HIDDEN);

        if (sel) {
            char info[96];
            if (selac->on_ground) {
                snprintf(info, sizeof(info), "%s\n%s  %.1f km",
                         selac->callsign[0] ? selac->callsign : selac->hex,
                         L()->ground, selac->dist_nm * 1.852);
            } else {
                char ua[20], us[20];
                snprintf(info, sizeof(info), "%s\n%s  %s  %.1f km",
                         selac->callsign[0] ? selac->callsign : selac->hex,
                         units_alt(selac->alt_baro_ft, ua, sizeof(ua)),
                         units_speed(selac->gs_kts, us, sizeof(us)),
                         selac->dist_nm * 1.852);
            }
            lv_label_set_text(s_radar_info, info);
            const char *lcode = airline_code(selac, &s_shown[s_selected].route);
            const lv_img_dsc_t *ldsc = lcode ? logos_get(lcode) : NULL;
            if (ldsc != NULL) {
                img_src_if_changed(s_radar_blogo, ldsc);
                lv_obj_clear_flag(s_radar_blogo, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_radar_blogo, LV_OBJ_FLAG_HIDDEN);
            }
            if (s_radar_bub_off) {
                lv_obj_add_flag(s_radar_bub, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(s_radar_bub, LV_OBJ_FLAG_HIDDEN);
            }
            int lx = x + UISX(12), ly = y - UISY(10);
            if (lx > RADAR_W - UISX(190)) {
                lx = x - UISX(190);
            }
            if (ly < 0) {
                ly = 0;
            }
            if (ly > RADAR_H - UISY(60)) {
                ly = RADAR_H - UISY(60);
            }
            lv_obj_set_pos(s_radar_bub, lx, ly);
        }
    }
    if (!planes_on || s_selected < 0 || s_selected >= s_shown_count ||
        selac == NULL) {
        lv_obj_add_flag(s_radar_bub, LV_OBJ_FLAG_HIDDEN);
    }
    render_radar_extras(map_mode, radius_nm);
    render_airspaces(map_mode);
}

/* ---------- full-screen ambient screensaver ---------- */

static void render_ambient(void);

/* Upscale the rendered map in place around (px,py) so the observation
 * circle spans the full screen height. One-time cost in the worker task;
 * the displayed image stays a plain untransformed bitmap. */
static void fb_upscale(uint16_t *fb, int W, int H, int px, int py, float k)
{
    /* Scratch-frame version: the buffer under our feet is usually the live
     * LVGL image source, and transforming it in place paints garbage on the
     * screen mid-pass. When the scratch allocation fails we simply skip the
     * upscale and the view stays at its native framing. */
    uint16_t *tmp = heap_caps_malloc((size_t)W * H * 2, MALLOC_CAP_SPIRAM);
    if (tmp == NULL) {
        return;
    }
    for (int y = 0; y < H; y++) {
        if ((y & 63) == 63) {
            vTaskDelay(1);      /* let IDLE0 feed the task watchdog */
        }
        float sy = py + (y - py) / k;
        int y0 = (int)sy;
        float fy = sy - y0;
        if (y0 < 0) { y0 = 0; fy = 0; }
        if (y0 > H - 2) { y0 = H - 2; fy = 1; }
        for (int x = 0; x < W; x++) {
            float sx = px + (x - px) / k;
            int x0 = (int)sx;
            float fx = sx - x0;
            if (x0 < 0) { x0 = 0; fx = 0; }
            if (x0 > W - 2) { x0 = W - 2; fx = 1; }
            uint16_t c00 = fb[y0 * W + x0], c01 = fb[y0 * W + x0 + 1];
            uint16_t c10 = fb[(y0 + 1) * W + x0], c11 = fb[(y0 + 1) * W + x0 + 1];
            float w00 = (1 - fx) * (1 - fy), w01 = fx * (1 - fy);
            float w10 = (1 - fx) * fy, w11 = fx * fy;
            int r = (int)((c00 >> 11) * w00 + (c01 >> 11) * w01 +
                          (c10 >> 11) * w10 + (c11 >> 11) * w11);
            int g = (int)(((c00 >> 5) & 0x3F) * w00 + ((c01 >> 5) & 0x3F) * w01 +
                          ((c10 >> 5) & 0x3F) * w10 + ((c11 >> 5) & 0x3F) * w11);
            int b = (int)((c00 & 0x1F) * w00 + (c01 & 0x1F) * w01 +
                          (c10 & 0x1F) * w10 + (c11 & 0x1F) * w11);
            tmp[y * W + x] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
    memcpy(fb, tmp, (size_t)W * H * 2);
    free(tmp);
}

static void amb_img_fit(void);

static void amb_tiles_job(void)
{
    char key[48];
    double b[4];
    strlcpy(key, s_amb_key, sizeof(key));
    memcpy(b, s_amb_bbox, sizeof(b));

    /* Composes in its own canvas: with the flash tile cache a render is
     * fast and nearly always complete, and skipping the background flood
     * on same-area re-renders (tilemap TM_NO_FLOOD) means a visible retry
     * morphs into the fresh frame instead of flashing dark. */
    uint16_t *scratch = s_amb_tiles;
    /* Refreshing a view already on screen (10-minute rain recompose,
     * partial retry): compose off screen when PSRAM has room to spare.
     * Rewriting the displayed canvas let every LVGL redraw of the range
     * ring's box show the half-built frame, a blank band across the map
     * several times an hour (#48). The margin keeps tile decoding (which
     * also needs big blocks, and the 7B is tight) from starving; without
     * it the old in-place path remains. */
    const size_t amb_bytes = (size_t)AMB_RENDER_W * AMB_RENDER_H * 2;
    uint16_t *offscreen = NULL;
    if (s_amb_view_ok &&
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) > amb_bytes + 1024 * 1024) {
        offscreen = heap_caps_malloc(amb_bytes, MALLOC_CAP_SPIRAM);
    }
    if (offscreen != NULL) {
        scratch = offscreen;
    } else if (lvgl_port_lock(1000)) {
        s_amb_view_ok = false;   /* canvas is being rewritten */
        lvgl_port_unlock();
    }
    tile_view_t view;
    bool ok = scratch != NULL &&
              tilemap_render(scratch, AMB_RENDER_W, AMB_RENDER_H, b[0], b[1], b[2], b[3], &view);
    if (ok) {
        runways_draw(scratch, AMB_RENDER_W, AMB_RENDER_H, &view);
    }

        float scale = 1.0f;
        int hx = AMB_RENDER_W / 2, hy = AMB_RENDER_H / 2;
        if (ok) {
            /* Integer tile zooms rarely land exactly; pre-stretch the map
             * once so the observation circle spans the full height. */
            int ex, ey;
            tilemap_project(&view, s_home_lat, s_home_lon, &hx, &hy);
            double rkm = settings_get()->radius_nm * 1.852;
            tilemap_project(&view, s_home_lat + rkm / 111.0, s_home_lon, &ex, &ey);
            float r = (float)(hy - ey);
            float half = (float)(AMB_RENDER_H / 2);
            if (r > 20.0f && r < half) {
                scale = half / r;
                if (scale > 2.5f) {
                    scale = 2.5f;
                }
            }
            if (scale > 1.05f) {
                fb_upscale(scratch, AMB_RENDER_W, AMB_RENDER_H, hx, hy, scale);
            } else {
                scale = 1.0f;
            }
        }

        if (lvgl_port_lock(-1)) {
        ok = ok && s_amb_tiles != NULL;   /* boot-allocated */
        if (ok && offscreen != NULL && view.missing > s_amb_missing) {
            ok = false;   /* keep the more complete frame already shown */
        }
        if (ok && offscreen != NULL) {
            memcpy(s_amb_tiles, offscreen, amb_bytes);
        }
        if (ok) {
            s_amb_view = view;
            s_amb_view_ok = true;
            s_amb_missing = view.missing;
            s_amb_scale = scale;
            s_amb_px = hx;
            s_amb_py = hy;
            s_amb_tiles_dsc.header.always_zero = 0;
            s_amb_tiles_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
            s_amb_tiles_dsc.header.w = AMB_RENDER_W;
            s_amb_tiles_dsc.header.h = AMB_RENDER_H;
            s_amb_tiles_dsc.data = (const uint8_t *)s_amb_tiles;
            s_amb_tiles_dsc.data_size = (size_t)AMB_RENDER_W * AMB_RENDER_H * 2;
            if (s_amb != NULL && s_amb_img != NULL) {
                lv_img_set_src(s_amb_img, &s_amb_tiles_dsc);
                amb_img_fit();
                if (s_amb_note != NULL) {
                    lv_obj_add_flag(s_amb_note, LV_OBJ_FLAG_HIDDEN);
                }
                render_ambient();
            }
        }
        lvgl_port_unlock();
    }
    bool was_off = offscreen != NULL;
    free(offscreen);
    ESP_LOGI("ui", "ambient tiles: %s (scale %.2f%s)", ok ? "ok" : "FAILED",
             (double)scale, was_off ? ", off screen" : "");
    s_amb_busy = false;
}

/* Bundled 800x400 world.png zoomed to fill the ambient screen (1.0 on the
   800x480 boards, which keeps the historical layout pixel-identical). */
static float amb_world_scale(void)
{
    float kx = (float)SCR_W / 800.0f;
    float ky = (float)(SCR_H - 80) / 400.0f;
    return kx < ky ? kx : ky;
}

/* Attach geometry for the (possibly design-space) ambient canvas: LVGL
 * zooms around the widget center, so position the nominal box centered
 * and let the uniform zoom fill the panel. Identity on 800x480. */
static void amb_img_fit(void)
{
    if (SCR_W > AMB_RENDER_W) {
        lv_img_set_zoom(s_amb_img, (uint16_t)(AMB_ZOOM_K * 256.0f + 0.5f));
        lv_obj_set_pos(s_amb_img, SCR_W / 2 - AMB_RENDER_W / 2,
                       SCR_H / 2 - AMB_RENDER_H / 2);
    } else {
        lv_img_set_zoom(s_amb_img, LV_IMG_ZOOM_NONE);
        lv_obj_set_pos(s_amb_img, 0, 0);
    }
}

static void amb_proj(double lat, double lon, lv_coord_t *x, lv_coord_t *y)
{
    if (s_amb_view_ok) {
        int xx, yy;
        tilemap_project(&s_amb_view, lat, lon, &xx, &yy);
        if (s_amb_scale != 1.0f) {
            xx = s_amb_px + (int)((xx - s_amb_px) * s_amb_scale);
            yy = s_amb_py + (int)((yy - s_amb_py) * s_amb_scale);
        }
        /* render space -> screen space (identity on 800x480 panels) */
        *x = (lv_coord_t)(SCR_W / 2 + (xx - AMB_RENDER_W / 2) * AMB_ZOOM_K);
        *y = (lv_coord_t)(SCR_H / 2 + (yy - AMB_RENDER_H / 2) * AMB_ZOOM_K);
        return;
    }
    float k = amb_world_scale();
    *x = (lv_coord_t)(SCR_W / 2 + ((lon + 180.0) / 360.0 - 0.5) * 800.0 * k);
    *y = (lv_coord_t)(40 + (SCR_H - 80) / 2 +
                      ((90.0 - lat) / 180.0 - 0.5) * 400.0 * k);
}

static void amb_spawn_tiles(void)
{
    if (!s_home_ok || s_amb_busy) {
        return;
    }
    int radius_nm = settings_get()->radius_nm;
    double rkm = radius_nm * 1.852;
    double dlat = rkm / 111.0;
    double dlon = rkm / (111.0 * cos(s_home_lat * M_PI / 180.0));
    s_amb_bbox[0] = s_home_lat - dlat;
    s_amb_bbox[1] = s_home_lat + dlat;
    s_amb_bbox[2] = s_home_lon - dlon;
    s_amb_bbox[3] = s_home_lon + dlon;
    snprintf(s_amb_key, sizeof(s_amb_key), "amb");
    s_amb_last_try = esp_timer_get_time() / 1000;
    ESP_LOGI("ui", "ambient tiles: spawning worker");
    s_amb_busy = true;
    if (!tilemap_worker_submit(amb_tiles_job)) {
        ESP_LOGE("ui", "ambient tiles: worker queue full");
        s_amb_busy = false;
    }
}

/* Fetch and persist the screensaver's map right after boot, while PSRAM
 * still has room for TLS and tile decoding: the flash tile cache then
 * serves every later ambient render offline, so the screensaver works
 * no matter how tight memory is by the time it first engages. */
void ui_prewarm_ambient(void)
{
    if (!s_amb_view_ok) {
        amb_spawn_tiles();
    }
}

static void render_ambient(void)
{
    if (s_amb == NULL) {
        return;
    }
    if (s_amb_retro) {
        render_retro_panel();
        return;
    }
    /* tile fetch failed or was partial (offline blip): retry every 20 s
     * until the view is complete. With the rain overlay on, a complete
     * view also recomposes every 10 minutes (the retro scope's cadence):
     * the radar frame is baked into the tiles, so without this the
     * screensaver kept the weather from whenever it was opened, frozen
     * all night (#21). Base tiles come from the flash cache and
     * TM_NO_FLOOD morphs the visible frame, so the refresh is cheap
     * and easy on the eyes. */
    int64_t now_ms = esp_timer_get_time() / 1000;
    bool rain_stale = settings_get()->rain_overlay && s_amb_view_ok &&
                      s_amb_missing == 0 &&
                      now_ms - s_amb_last_try > 10 * 60 * 1000;
    if ((!s_amb_view_ok || s_amb_missing > 0 || rain_stale) &&
        !s_amb_busy && s_home_ok && now_ms - s_amb_last_try > 20000) {
        amb_spawn_tiles();
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
            lv_obj_set_pos(s_amb_home, hx - UISX(4), hy - UISY(4));
            lv_obj_clear_flag(s_amb_home, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* Local view without a tile projection (alloc or fetch failed, retried
     * above): the world-map fallback math would collapse every aircraft in
     * a 30 nm radius into one false blob (issue #12), so draw none. The
     * world projection stays for the pre-location boot state only. */
    bool proj_usable = s_amb_view_ok || !s_home_ok;
    /* every aircraft in range gets a sprite */
    for (int i = 0; i < MAX_AIRCRAFT; i++) {
        if (i >= s_all_count || !proj_usable) {
            lv_obj_add_flag(s_amb_planes[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_coord_t x, y;
        amb_proj(s_all[i].lat, s_all[i].lon, &x, &y);
        if (x < -14 || x > SCR_W + 14 || y < -14 || y > SCR_H + 14) {
            lv_obj_add_flag(s_amb_planes[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        img_src_if_changed(s_amb_planes[i], class_sprite(s_all[i].fcls));
        lv_obj_set_pos(s_amb_planes[i], x - 14, y - 14);   /* UIZOOM keeps the 28px box */
        lv_img_set_angle(s_amb_planes[i], (int)(s_all[i].track * 10));
        img_recolor_if_changed(s_amb_planes[i],
                               alt_color(s_all[i].alt_ft, s_all[i].ground));
        lv_obj_clear_flag(s_amb_planes[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* bubble for the tapped aircraft */
    if (s_amb_selbub != NULL) {
        int sel = -1;
        for (int i = 0; proj_usable && s_amb_sel_cs[0] && i < s_all_count; i++) {
            if (strcmp(s_all[i].callsign, s_amb_sel_cs) == 0) {
                sel = i;
                break;
            }
        }
        if (sel >= 0) {
            char txt[160];
            const route_info_t *rt = routes_get_cached(s_all[sel].callsign);
            if (rt != NULL && rt->valid) {
                char ua[20];
                char cities[64];
                if (settings_get()->show_route &&
                    (rt->origin.city[0] || rt->destination.city[0])) {
                    /* cities under the codes (#28); capped so the bubble
                     * stays narrower than half the panel */
                    snprintf(cities, sizeof(cities), "\n%.14s " LV_SYMBOL_RIGHT " %.14s",
                             rt->origin.city[0] ? rt->origin.city : "?",
                             rt->destination.city[0] ? rt->destination.city : "?");
                } else {
                    cities[0] = '\0';
                }
                snprintf(txt, sizeof(txt), "%s\n%s " LV_SYMBOL_RIGHT " %s%s\n%s",
                         s_all[sel].callsign,
                         rt->origin.iata[0] ? rt->origin.iata : rt->origin.icao,
                         rt->destination.iata[0] ? rt->destination.iata
                                                 : rt->destination.icao,
                         cities,
                         units_alt(s_all[sel].alt_ft, ua, sizeof(ua)));
            } else {
                char ua[20];
                snprintf(txt, sizeof(txt), "%s\n%s", s_all[sel].callsign,
                         units_alt(s_all[sel].alt_ft, ua, sizeof(ua)));
            }
            lv_label_set_text(s_amb_selbub, txt);
            lv_coord_t x, y;
            amb_proj(s_all[sel].lat, s_all[sel].lon, &x, &y);
            int lx = x + 18, ly = y - 12;
            if (lx > SCR_W - 180) {
                lx = x - 180;
            }
            if (ly < 0) {
                ly = 0;
            }
            if (ly > SCR_H - 70) {
                ly = SCR_H - 70;
            }
            lv_obj_set_pos(s_amb_selbub, lx, ly);
            lv_obj_clear_flag(s_amb_selbub, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_amb_selbub);
        } else {
            lv_obj_add_flag(s_amb_selbub, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* info bubbles only for the nearest ones, to keep the map readable */
    lv_area_t placed[MAX_SHOWN];
    int nplaced = 0;
    for (int i = 0; i < MAX_SHOWN; i++) {
        if (i >= s_shown_count || !s_shown[i].ac.has_pos) {
            lv_obj_add_flag(s_amb_lbls[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const aircraft_t *ac = &s_shown[i].ac;
        lv_coord_t x, y;
        amb_proj(ac->lat, ac->lon, &x, &y);
        if (x < -14 || x > SCR_W + 14 || y < -14 || y > SCR_H + 14) {
            lv_obj_add_flag(s_amb_lbls[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

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
        if (lx > SCR_W - 110) {
            lx = x - 110;
        }
        if (ly < 0) {
            ly = 0;
        }
        if (ly > SCR_H - 40) {
            ly = SCR_H - 40;
        }
        /* skip bubbles that would overlap one already on screen */
        lv_area_t box = { lx, ly, lx + 112, ly + (rt != NULL ? 38 : 20) };
        bool clash = false;
        for (int p = 0; p < nplaced; p++) {
            if (box.x1 < placed[p].x2 && box.x2 > placed[p].x1 &&
                box.y1 < placed[p].y2 && box.y2 > placed[p].y1) {
                clash = true;
                break;
            }
        }
        if (clash) {
            lv_obj_add_flag(s_amb_lbls[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        placed[nplaced++] = box;
        lv_obj_set_pos(s_amb_lbls[i], lx, ly);
        lv_obj_clear_flag(s_amb_lbls[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void amb_restore_retro(void)
{
    if (!s_amb_retro || s_retro_panel == NULL) {
        return;
    }
    lv_obj_set_parent(s_retro_panel, lv_scr_act());
    lv_obj_set_pos(s_retro_panel, LIST_W, HEADER_H);
    if (s_retro_was_hidden) {
        lv_obj_add_flag(s_retro_panel, LV_OBJ_FLAG_HIDDEN);
        if (s_retro_timer != NULL) {
            lv_timer_pause(s_retro_timer);   /* sweep idles with the scope */
        }
    }
    s_amb_retro = false;
}

static void amb_close(void)
{
    if (s_amb != NULL) {
        amb_restore_retro();   /* rescue the adopted retro panel first */
        lv_obj_del(s_amb);
        s_amb = NULL;
        /* children died with the parent: drop every pointer into them, or
           the next periodic update writes into freed memory (field crash
           found on the 7B: weather tick after an ambient cycle) */
        s_amb_clock = NULL;
        s_amb_wx = NULL;
        s_amb_note = NULL;
        /* The canvas is boot-allocated and permanent: freeing it under
         * pressure used to guarantee the re-allocation would fail later
         * in a fragmented PSRAM (the 7B's forever-dark screensaver). */
        s_amb_scale = 1.0f;
        s_amb_sel_cs[0] = '\0';
        /* ui_update skipped list/right renders while the overlay covered
         * them; bring the underlying panels back up to date now */
        render_list_rows();
        render_list_selection();
        render_right();
    }
}

static void amb_click_cb(lv_event_t *e)
{
    if (s_bl_off) {
        waveshare_rgb_lcd_bl_on();
        ui_apply_brightness();
        s_bl_off = false;
        s_bl_forced = false;
        return;     /* first tap only wakes the screen */
    }
    /* tap on (near) a plane selects it and shows its bubble;
     * tap on empty map closes the screensaver */
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t pt;
    if (indev != NULL) {
        lv_indev_get_point(indev, &pt);
        /* no sprites on screen (no tile projection) -> nothing to pick */
        bool pickable = s_amb_view_ok || !s_home_ok;
        int best = -1, best_d2 = 48 * 48;
        for (int i = 0; pickable && i < s_all_count; i++) {
            lv_coord_t x, y;
            amb_proj(s_all[i].lat, s_all[i].lon, &x, &y);
            int dx = pt.x - x, dy = pt.y - y;
            int d2 = dx * dx + dy * dy;
            if (d2 < best_d2) {
                best_d2 = d2;
                best = i;
            }
        }
        if (best >= 0) {
            strlcpy(s_amb_sel_cs, s_all[best].callsign, sizeof(s_amb_sel_cs));
            render_ambient();
            return;
        }
        if (s_amb_sel_cs[0]) {      /* deselect first, close on next tap */
            s_amb_sel_cs[0] = '\0';
            render_ambient();
            return;
        }
    }
    amb_close();
}

static void amb_show(void)
{
    if (s_amb != NULL) {
        return;
    }
    s_amb_retro = settings_get()->amb_style == 1 && s_retro_panel != NULL;
    if (s_amb_retro) {
        /* CRT bezel: black screen, the retro scope centered on it */
        s_amb = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_amb, SCR_W, SCR_H);
        lv_obj_set_pos(s_amb, 0, 0);
        lv_obj_set_style_bg_color(s_amb, lv_color_black(), 0);
        lv_obj_set_style_border_width(s_amb, 0, 0);
        lv_obj_set_style_radius(s_amb, 0, 0);
        lv_obj_set_style_pad_all(s_amb, 0, 0);
        lv_obj_clear_flag(s_amb, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_amb, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_amb, amb_click_cb, LV_EVENT_CLICKED, NULL);
        s_retro_was_hidden = lv_obj_has_flag(s_retro_panel, LV_OBJ_FLAG_HIDDEN);
        if (s_retro_timer != NULL) {
            lv_timer_resume(s_retro_timer);
        }
        lv_obj_set_parent(s_retro_panel, s_amb);
        lv_obj_set_pos(s_retro_panel, (SCR_W - RADAR_W) / 2, (SCR_H - RADAR_H) / 2);
        lv_obj_clear_flag(s_retro_panel, LV_OBJ_FLAG_HIDDEN);
        render_retro_panel();
        return;
    }
    s_amb = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_amb, SCR_W, SCR_H);
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
    /* No world-map placeholder (in a local screensaver it read as a bug):
     * re-entries show the last rendered frame instantly, a true first
     * entry gets a quiet note until tiles land. */
    if (s_amb_view_ok && s_amb_tiles != NULL) {
        lv_img_set_src(s_amb_img, &s_amb_tiles_dsc);
        amb_img_fit();
    } else {
        s_amb_note = make_label(s_amb, UIFONT(&font_pl_16, &font_pl_10),
                                lv_color_hex(0x66707a));
        lv_label_set_text(s_amb_note, L()->amb_loading);
        lv_obj_align(s_amb_note, LV_ALIGN_CENTER, 0, 0);
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
    lv_obj_set_size(s_amb_home, UISX(8), UISY(8));
    lv_obj_set_style_bg_color(s_amb_home, COL_ACCENT, 0);
    lv_obj_set_style_border_width(s_amb_home, 0, 0);
    lv_obj_set_style_radius(s_amb_home, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(s_amb_home, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_amb_home, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < MAX_AIRCRAFT; i++) {
        s_amb_planes[i] = plane_img(s_amb);
        lv_obj_add_flag(s_amb_planes[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < MAX_SHOWN; i++) {
        s_amb_lbls[i] = make_label(s_amb, UIFONT(&font_pl_14, &font_pl_8), COL_TEXT);
        lv_obj_set_style_bg_color(s_amb_lbls[i], lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_amb_lbls[i], LV_OPA_50, 0);
        lv_obj_set_style_pad_hor(s_amb_lbls[i], UISX(4), 0);
        lv_obj_add_flag(s_amb_lbls[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_amb_selbub = make_label(s_amb, UIFONT(&font_pl_16, &font_pl_10), lv_color_hex(0xffffff));
    lv_obj_set_style_bg_color(s_amb_selbub, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_amb_selbub, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(s_amb_selbub, UISY(6), 0);
    lv_obj_set_style_border_width(s_amb_selbub, 1, 0);
    lv_obj_set_style_border_color(s_amb_selbub, COL_ACCENT, 0);
    lv_obj_add_flag(s_amb_selbub, LV_OBJ_FLAG_HIDDEN);

    s_amb_clock = make_label(s_amb, UIFONT(&lv_font_montserrat_32, &lv_font_montserrat_20), lv_color_hex(0xffffff));
    lv_obj_set_style_bg_color(s_amb_clock, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_amb_clock, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(s_amb_clock, UISY(8), 0);
    lv_obj_align(s_amb_clock, LV_ALIGN_TOP_LEFT, UISX(10), UISY(8));
    lv_label_set_text(s_amb_clock, "");

    lv_obj_t *attr = make_label(s_amb, UIFONT(&font_pl_14, &font_pl_8), lv_color_hex(0x777777));
    lv_label_set_text(attr, map_attribution());
    lv_obj_align(attr, LV_ALIGN_BOTTOM_RIGHT, -UISX(8), -UISY(4));

    s_amb_wx = make_label(s_amb, UIFONT(&font_pl_16, &font_pl_10), lv_color_hex(0xdddddd));
    lv_obj_set_style_bg_color(s_amb_wx, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_amb_wx, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(s_amb_wx, UISY(6), 0);
    lv_obj_align(s_amb_wx, LV_ALIGN_TOP_RIGHT, -UISX(10), UISY(12));
    lv_label_set_text(s_amb_wx, s_weather_txt);

    /* home-area tiles for the whole screen */
    amb_spawn_tiles();
    render_ambient();
}

void ui_selected_hex(char *out, size_t n)
{
    strlcpy(out, s_selected_hex, n);
}

bool ui_screen_dark(void)
{
#ifdef APKFLIGHT
    extern volatile bool g_app_background;
    return s_bl_off || g_app_background;
#else
    return s_bl_off;
#endif
}

/* idle watcher: screensaver + night backlight */
static void idle_timer_cb(lv_timer_t *t)
{
    const settings_t *cfg = settings_get();
    uint32_t idle_ms = lv_disp_get_inactive_time(NULL);

    /* #43: optional time window for the screensaver. Outside it the
     * radar stays up; when the window closes a running screensaver ends
     * too (unless the night schedule has the panel dark). No clock yet
     * means no window check. */
    bool amb_window = true;
    /* Track mode: the point is to watch that one flight, so no screensaver
     * (a running one closes) while it is on. */
    if (cfg->track_on && cfg->track_id[0]) {
        amb_window = false;
    } else if (cfg->amb_sched) {
        time_t now = time(NULL);
        if (now > 1600000000) {
            time_t l = now + (tz_home_known() ? tz_home_offset() : 0);
            struct tm tm;
            gmtime_r(&l, &tm);
            int m = tm.tm_hour * 60 + tm.tm_min;
            int a = cfg->amb_from_min, b = cfg->amb_to_min;
            amb_window = a == b || (a < b ? (m >= a && m < b) : (m >= a || m < b));
        }
    }
    if (cfg->ambient_idle_min > 0 && s_amb == NULL && amb_window &&
        idle_ms > (uint32_t)cfg->ambient_idle_min * 60000U) {
        amb_show();
    } else if (!amb_window && s_amb != NULL && !s_bl_off) {
        amb_close();
    }

    if (cfg->night_enabled) {
        time_t now = time(NULL);
        if (now > 1600000000) {
            time_t l = now + (tz_home_known() ? tz_home_offset() : 0);
            struct tm tm;
            gmtime_r(&l, &tm);
            int m = tm.tm_hour * 60 + tm.tm_min;
            int a = cfg->night_start_min, b = cfg->night_end_min;
            if (cfg->night_auto && s_home_ok) {
                int rise, set;
                if (geo_sun_times(s_home_lat, s_home_lon, (long long)now,
                                  tz_home_known() ? tz_home_offset() : 0,
                                  &rise, &set)) {
                    a = set;    /* dark from sunset... */
                    b = rise;   /* ...to sunrise */
                }
            }
            bool night = a <= b ? (m >= a && m < b) : (m >= a || m < b);
            if (night && !s_bl_off && s_amb != NULL &&
                idle_ms > (uint32_t)(cfg->ambient_idle_min + 5) * 60000U) {
                waveshare_rgb_lcd_bl_off();
                s_bl_off = true;
            } else if (!night && s_bl_off && !s_bl_forced) {
                /* Morning restore must NOT require the screensaver to still
                 * be up: a tap in the dark can close it invisibly, and the
                 * old gate then kept the panel black past sunrise (7B
                 * owner report) until a reboot. */
                waveshare_rgb_lcd_bl_on();
                ui_apply_brightness();
                s_bl_off = false;
            }
        }
    }

    /* Safety net alongside the screensaver's tap-to-wake: any recent input
     * while dark relights the panel (this timer runs every 10 s), and so
     * does disabling night mode from the web panel while dark. */
    if (s_bl_off && (idle_ms < 11000U || (!cfg->night_enabled && !s_bl_forced))) {
        waveshare_rgb_lcd_bl_on();
        ui_apply_brightness();
        s_bl_off = false;
        s_bl_forced = false;
    }
}


/* ---- Retro radar: phosphor CRT sweep over the same targets ---- */

static void retro_rain_job(void)
{
    int w = RADAR_W, h = RADAR_H;
    if (s_retro_rain_buf == NULL) {
        s_retro_rain_buf = heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
    }
    double rkm = settings_get()->radius_nm * 1.852;
    double dlat = rkm / 111.0;
    double dlon = rkm / (111.0 * cos(s_home_lat * M_PI / 180.0));
    tile_view_t view;
    bool ok = s_retro_rain_buf != NULL &&
              tilemap_render_rain(s_retro_rain_buf, w, h,
                                  s_home_lat - dlat, s_home_lat + dlat,
                                  s_home_lon - dlon, s_home_lon + dlon, &view);
    if (lvgl_port_lock(-1)) {
        if (ok && s_retro_rain_img != NULL) {
            s_retro_rain_dsc.header.always_zero = 0;
            s_retro_rain_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
            s_retro_rain_dsc.header.w = w;
            s_retro_rain_dsc.header.h = h;
            s_retro_rain_dsc.data = (const uint8_t *)s_retro_rain_buf;
            s_retro_rain_dsc.data_size = (size_t)w * h * 2;
            lv_img_set_src(s_retro_rain_img, &s_retro_rain_dsc);
            lv_obj_clear_flag(s_retro_rain_img, LV_OBJ_FLAG_HIDDEN);
        }
        lvgl_port_unlock();
    }
    s_retro_rain_ms = esp_timer_get_time() / 1000;
    s_retro_rain_gen = rainviewer_generation();
    s_retro_rain_busy = false;
}

static void retro_rain_want(void)
{
    if (!settings_get()->rain_overlay) {
        if (s_retro_rain_img != NULL) {
            lv_obj_add_flag(s_retro_rain_img, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (!s_home_ok || s_retro_rain_busy) {
        return;
    }
    int64_t now = esp_timer_get_time() / 1000;
    if (s_retro_rain_ms >= 0 && now - s_retro_rain_ms < 10 * 60 * 1000) {
        return;
    }
    s_retro_rain_busy = true;
    if (!tilemap_worker_submit(retro_rain_job)) {
        s_retro_rain_busy = false;
    }
}

static void retro_timer_cb(lv_timer_t *t)
{
    if (s_retro_panel == NULL || lv_obj_has_flag(s_retro_panel, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    s_retro_angle += RETRO_STEP;
    if (s_retro_angle >= 360.0f) {
        s_retro_angle -= 360.0f;
    }

    int cx = RADAR_CX, cy = RADAR_CY, r = RADAR_R;
    for (int k = 0; k < RETRO_TRAIL; k++) {
        float a = (s_retro_angle - k * 2.0f) * (float)M_PI / 180.0f;
        s_retro_beam_pts[k][0].x = cx;
        s_retro_beam_pts[k][0].y = cy;
        s_retro_beam_pts[k][1].x = cx + (lv_coord_t)(sinf(a) * r);
        s_retro_beam_pts[k][1].y = cy - (lv_coord_t)(cosf(a) * r);
        lv_line_set_points(s_retro_beam[k], s_retro_beam_pts[k], 2);
    }

    /* blips glow when the beam passes and decay until the next sweep */
    for (int i = 0; i < MAX_AIRCRAFT; i++) {
        if (!s_retro_valid[i]) {
            continue;
        }
        float delta = s_retro_angle - s_retro_bearing[i];
        while (delta < 0) {
            delta += 360.0f;
        }
        int opa = (int)(255.0f * (1.0f - delta / 320.0f));
        if (opa < 25) {
            opa = 25;
        }
        lv_obj_set_style_bg_opa(s_retro_blips[i], opa, 0);
        lv_obj_set_style_text_opa(s_retro_lbls[i], opa > 60 ? opa : 0, 0);
    }
}

/* see retro_map_update: grayscale-band classifier, lift-aware */
static inline int retro_cls(uint16_t c, bool unlift)
{
    int r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
    if (r5 * 2 - g6 > 3 || g6 - r5 * 2 > 3 ||
        b5 * 2 - g6 > 3 || g6 - b5 * 2 > 3) {
        return 3;               /* colored: rain overlay, markers */
    }
    int l8 = g6 * 4;
    if (unlift) {
        /* invert v' = v + (255-v)*77/256 */
        l8 = (l8 * 256 - 77 * 255) / 179;
        if (l8 < 0) {
            l8 = 0;
        }
    }
    if (l8 < 15) {
        return 0;               /* land */
    }
    if (l8 < 30) {
        return 2;               /* roads, minor features */
    }
    if (l8 <= 130) {
        return 1;               /* water */
    }
    return 3;                   /* labels */
}

static void retro_map_update(void)
{
    if (!settings_get()->retro_map || !s_radar_view_ok) {
        if (s_retro_map_img != NULL) {
            lv_obj_add_flag(s_retro_map_img, LV_OBJ_FLAG_HIDDEN);
        }
        s_retro_map_key[0] = '\0';
        return;
    }
    if (strcmp(s_retro_map_key, s_radar_key) == 0) {
        lv_obj_clear_flag(s_retro_map_img, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (s_retro_map_buf == NULL) {
        s_retro_map_buf = heap_caps_malloc((size_t)RADAR_W * RADAR_H * 2,
                                           MALLOC_CAP_SPIRAM);
    }
    if (s_retro_map_buf == NULL || s_radar_tiles == NULL) {
        return;
    }
    int hx, hy, ex, ey;
    radar_project(s_home_lat, s_home_lon, &hx, &hy);
    double rkm = settings_get()->radius_nm * 1.852;
    radar_project(s_home_lat + rkm / 111.0, s_home_lon, &ex, &ey);
    float radius_px = (float)(hy - ey);
    if (radius_px < 10.0f) {
        return;
    }
    float k = radius_px / (float)RADAR_R;   /* panel px per scope px */
    /* Land/water classification (CARTO dark_all is grayscale: land
     * #090909, roads mid grays, water #262626). "Brighter map tiles"
     * lifts v += 30% of headroom at decode - undo it first, or the
     * classes collapse into 5 quantization steps. Tuned offline against
     * the real tiles. Classes: 0 land, 1 water, 2 road, 3 other. */
    const bool unlift = settings_get()->map_light;
    /* The classifier is tuned for CARTO's grayscale dark tiles. Since
     * 0.4.6 the default is OpenStreetMap (colour, darkened on device) or
     * any custom server, where land/water bands mean nothing and the
     * underlay came out empty. There the scope gets a phosphor rendering
     * of the tile's own brightness instead: same map, green CRT look,
     * clipped to the scope circle, dim enough for blips to stay on top. */
    const settings_t *rcfg = settings_get();
    const bool phosphor = rcfg->tile_url[0] != '\0' || rcfg->carto_key[0] == '\0';
    const int r2 = RADAR_R * RADAR_R;
    for (int y = 0; phosphor && y < RADAR_H; y++) {
        uint16_t *dst = &s_retro_map_buf[y * RADAR_W];
        float sy = hy + (y - RADAR_CY) * k;
        int dy = y - RADAR_CY;
        for (int x = 0; x < RADAR_W; x++) {
            int dx = x - RADAR_CX;
            uint16_t out = 0;
            float sx = hx + (x - RADAR_CX) * k;
            int isx = (int)(RADAR_RENDER_W / 2 + (sx - RADAR_W / 2) / RADAR_K);
            int isy = (int)(RADAR_RENDER_H / 2 + (sy - RADAR_H / 2) / RADAR_K);
            if (dx * dx + dy * dy <= r2 &&
                isx >= 0 && isx < RADAR_RENDER_W && isy >= 0 && isy < RADAR_RENDER_H) {
                uint16_t c = s_radar_tiles[isy * RADAR_RENDER_W + isx];
                /* luma 0..63 from RGB565 */
                int l = (((c >> 11) & 31) * 2 * 77 + ((c >> 5) & 63) * 150 +
                         (c & 31) * 2 * 29) >> 8;
                int g = l * 5 / 8 + 2;          /* phosphor green, ~60 % */
                if (g > 40) {
                    g = 40;
                }
                out = (uint16_t)(((g / 6) << 11) | (g << 5) | (g / 6));
            }
            dst[x] = out;
        }
    }
    for (int y = 0; !phosphor && y < RADAR_H; y++) {
        uint16_t *dst = &s_retro_map_buf[y * RADAR_W];
        float sy = hy + (y - RADAR_CY) * k;
        for (int x = 0; x < RADAR_W; x++) {
            float sx = hx + (x - RADAR_CX) * k;
            uint16_t out = 0;
            /* panel space -> tile render space (identity when RADAR_K is 1) */
            int isx = (int)(RADAR_RENDER_W / 2 + (sx - RADAR_W / 2) / RADAR_K);
            int isy = (int)(RADAR_RENDER_H / 2 + (sy - RADAR_H / 2) / RADAR_K);
            if (isx >= 1 && isx < RADAR_RENDER_W - 1 &&
                isy >= 1 && isy < RADAR_RENDER_H - 1) {
                const uint16_t *src = &s_radar_tiles[isy * RADAR_RENDER_W + isx];
                int c0 = retro_cls(src[0], unlift);
                /* two-sided boundary with diagonals: a solid ~2 px stroke
                 * instead of a resample-thinned 1 px thread */
                int other = c0 == 1 ? 0 : c0 == 0 ? 1 : -1;
                bool coast = false;
                if (other >= 0) {
                    static const int nx[8] = { -1, 1, 0, 0, -1, 1, -1, 1 };
                    static const int ny[8] = { 0, 0, -1, 1, -1, -1, 1, 1 };
                    for (int n = 0; n < 8 && !coast; n++) {
                        coast = retro_cls(src[ny[n] * RADAR_RENDER_W + nx[n]],
                                          unlift) == other;
                    }
                }
                if (coast) {
                    out = (uint16_t)((12 << 11) | (63 << 5) | 12);   /* mint-hot */
                } else if (c0 == 1) {
                    out = (uint16_t)((1 << 11) | (11 << 5) | 1);
                } else if (c0 == 2) {
                    out = (uint16_t)((2 << 11) | (17 << 5) | 2);
                } else if (c0 == 3) {
                    out = (uint16_t)((1 << 11) | (7 << 5) | 1);
                }
            }
            dst[x] = out;
        }
    }
    s_retro_map_dsc.header.always_zero = 0;
    s_retro_map_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_retro_map_dsc.header.w = RADAR_W;
    s_retro_map_dsc.header.h = RADAR_H;
    s_retro_map_dsc.data = (const uint8_t *)s_retro_map_buf;
    s_retro_map_dsc.data_size = (size_t)RADAR_W * RADAR_H * 2;
    lv_img_set_src(s_retro_map_img, &s_retro_map_dsc);
    lv_obj_clear_flag(s_retro_map_img, LV_OBJ_FLAG_HIDDEN);
    strlcpy(s_retro_map_key, s_radar_key, sizeof(s_retro_map_key));
    ESP_LOGI(TAG, "retro map underlay rebuilt");
}

static void render_retro_panel(void)
{
    int cx = RADAR_CX, cy = RADAR_CY, r = RADAR_R;
    int radius_nm = settings_get()->radius_nm;
    if (settings_get()->retro_map) {
        radar_tiles_want();   /* scope wants the canvas even if radar view never opened */
    }
    retro_map_update();
    lv_area_t lbl_box[24];
    int nlbl = 0;

    for (int i = 0; i < MAX_AIRCRAFT; i++) {
        if (i >= s_all_count || s_all[i].dist_nm < 0 ||
            s_all[i].dist_nm > radius_nm * 1.02f) {
            s_retro_valid[i] = false;
            lv_obj_add_flag(s_retro_blips[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_retro_lbls[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        float frac = s_all[i].dist_nm / (float)radius_nm;
        float rad = s_all[i].dir_deg * (float)M_PI / 180.0f;
        int x = cx + (int)(sinf(rad) * frac * r);
        int y = cy - (int)(cosf(rad) * frac * r);
        lv_obj_set_pos(s_retro_blips[i], x - UISX(3), y - UISY(3));
        lv_obj_set_pos(s_retro_lbls[i], x + UISX(8), y - UISY(7));
        lv_label_set_text(s_retro_lbls[i], s_all[i].callsign);
        lv_obj_clear_flag(s_retro_blips[i], LV_OBJ_FLAG_HIDDEN);
        s_retro_bearing[i] = s_all[i].dir_deg;
        s_retro_valid[i] = true;

        /* labels declutter like the screensaver: first come, first labeled */
        bool clash = nlbl >= 24;
        for (int p2 = 0; p2 < nlbl && !clash; p2++) {
            if (x + 8 < lbl_box[p2].x2 && x + 8 + 78 > lbl_box[p2].x1 &&
                y - 7 < lbl_box[p2].y2 && y - 7 + 16 > lbl_box[p2].y1) {
                clash = true;
            }
        }
        if (clash) {
            lv_obj_add_flag(s_retro_lbls[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_retro_lbls[i], LV_OBJ_FLAG_HIDDEN);
            lbl_box[nlbl].x1 = x + 8;
            lbl_box[nlbl].y1 = y - 7;
            lbl_box[nlbl].x2 = x + 8 + 78;
            lbl_box[nlbl].y2 = y - 7 + 16;
            nlbl++;
        }
    }

    retro_rain_want();

    char info[64];
    if (s_home_ok) {
        snprintf(info, sizeof(info), "%.4f%c %.4f%c  R%dkm",
                 fabs(s_home_lat), s_home_lat >= 0 ? 'N' : 'S',
                 fabs(s_home_lon), s_home_lon >= 0 ? 'E' : 'W',
                 (int)(radius_nm * 1.852));
    } else {
        snprintf(info, sizeof(info), "R%dkm", (int)(radius_nm * 1.852));
    }
    lv_label_set_text(s_retro_info, info);
}

static void build_retro_panel(lv_obj_t *scr)
{
    s_retro_panel = make_panel(scr);
    lv_obj_set_size(s_retro_panel, RADAR_W, RADAR_H);
    lv_obj_set_pos(s_retro_panel, LIST_W, HEADER_H);
    lv_obj_set_style_bg_color(s_retro_panel, RET_COL_BG, 0);
    lv_obj_add_flag(s_retro_panel, LV_OBJ_FLAG_HIDDEN);

    int cx = RADAR_CX, cy = RADAR_CY, r = RADAR_R;

    s_retro_map_img = lv_img_create(s_retro_panel);
    lv_obj_set_pos(s_retro_map_img, 0, 0);
    lv_obj_add_flag(s_retro_map_img, LV_OBJ_FLAG_HIDDEN);

    s_retro_rain_img = lv_img_create(s_retro_panel);
    lv_obj_set_pos(s_retro_rain_img, 0, 0);
    lv_obj_set_style_img_opa(s_retro_rain_img, 140, 0);
    lv_obj_add_flag(s_retro_rain_img, LV_OBJ_FLAG_HIDDEN);

    /* range rings + crosshair */
    for (int i = 1; i <= 3; i++) {
        int rr = r * i / 3;
        lv_obj_t *ring = lv_obj_create(s_retro_panel);
        lv_obj_set_size(ring, rr * 2, rr * 2);
        lv_obj_set_pos(ring, cx - rr, cy - rr);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_set_style_border_color(ring, RET_COL_DIM, 0);
        lv_obj_set_style_border_opa(ring, i == 3 ? 200 : 110, 0);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }
    static lv_point_t xh[2][2];
    for (int i = 0; i < 2; i++) {
        lv_obj_t *ln = lv_line_create(s_retro_panel);
        if (i == 0) {
            xh[i][0].x = cx - r; xh[i][0].y = cy;
            xh[i][1].x = cx + r; xh[i][1].y = cy;
        } else {
            xh[i][0].x = cx; xh[i][0].y = cy - r;
            xh[i][1].x = cx; xh[i][1].y = cy + r;
        }
        lv_line_set_points(ln, xh[i], 2);
        lv_obj_set_style_line_width(ln, 1, 0);
        lv_obj_set_style_line_color(ln, RET_COL_DIM, 0);
        lv_obj_set_style_line_opa(ln, 70, 0);
    }

    /* compass letters */
    static const char *dirs[4] = { "N", "E", "S", "W" };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *l = make_label(s_retro_panel, UIFONT(&lv_font_montserrat_16, &lv_font_montserrat_10), RET_COL_DIM);
        lv_label_set_text(l, dirs[i]);
        int dx = (i == 1) ? r + 8 : (i == 3) ? -r - 18 : -5;
        int dy = (i == 0) ? -r - 20 : (i == 2) ? r + 4 : -10;
        lv_obj_set_pos(l, cx + dx, cy + dy);
    }

    /* rotating beam: leading edge bright, trail fading out */
    for (int k = RETRO_TRAIL - 1; k >= 0; k--) {
        s_retro_beam[k] = lv_line_create(s_retro_panel);
        lv_obj_set_style_line_width(s_retro_beam[k], k == 0 ? 2 : 3, 0);
        lv_obj_set_style_line_color(s_retro_beam[k], RET_COL_BEAM, 0);
        lv_obj_set_style_line_opa(s_retro_beam[k],
                                  k == 0 ? 255 : 150 - k * 14, 0);
    }

    /* home dot */
    lv_obj_t *home = lv_obj_create(s_retro_panel);
    lv_obj_set_size(home, UISX(6), UISY(6));
    lv_obj_set_pos(home, cx - UISX(3), cy - UISY(3));
    lv_obj_set_style_radius(home, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(home, RET_COL_BEAM, 0);
    lv_obj_set_style_border_width(home, 0, 0);
    lv_obj_clear_flag(home, LV_OBJ_FLAG_SCROLLABLE);

    /* blips + callsign ghosts */
    for (int i = 0; i < MAX_AIRCRAFT; i++) {
        s_retro_blips[i] = lv_obj_create(s_retro_panel);
        lv_obj_set_size(s_retro_blips[i], UISX(7), UISY(7));
        lv_obj_set_style_radius(s_retro_blips[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_retro_blips[i], RET_COL_BEAM, 0);
        lv_obj_set_style_border_width(s_retro_blips[i], 0, 0);
        lv_obj_clear_flag(s_retro_blips[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_retro_blips[i], LV_OBJ_FLAG_HIDDEN);
        s_retro_lbls[i] = make_label(s_retro_panel, UIFONT(&font_pl_14, &font_pl_8), RET_COL_BEAM);
        lv_obj_add_flag(s_retro_lbls[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_retro_info = make_label(s_retro_panel, UIFONT(&font_pl_14, &font_pl_8), RET_COL_DIM);
    lv_obj_align(s_retro_info, LV_ALIGN_TOP_RIGHT, -UISX(10), UISY(6));

    s_retro_timer = lv_timer_create(retro_timer_cb, RETRO_TICK_MS, NULL);
    lv_timer_pause(s_retro_timer);   /* runs only while a retro scope is visible */
}

static void build_stats_panel(lv_obj_t *scr)
{
    s_stats_panel = make_panel(scr);
    lv_obj_set_size(s_stats_panel, SCR_W - LIST_W, SCR_H - HEADER_H);
    lv_obj_set_pos(s_stats_panel, LIST_W, HEADER_H);
    lv_obj_add_flag(s_stats_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *c = lv_obj_create(s_stats_panel);
    lv_obj_set_size(c, SCR_W - LIST_W, SCR_H - HEADER_H);
    lv_obj_set_pos(c, 0, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, UISY(16), 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    /* extra height beyond the 432px design goes into the hourly chart */
    int vext = SCR_H - HEADER_H - DSN_H;

    lv_obj_t *title = make_label(c, UIFONT(&font_pl_20, &font_pl_12), COL_ACCENT);
    lv_label_set_text(title, L()->stats_title);
    lv_obj_set_pos(title, 0, 0);

    const int tile_sp = (DTL_W + 6) / 4;    /* 116 on the device */
    const int tile_w = tile_sp - 6;
    const char *names[4] = { L()->st_unique, L()->st_highest, L()->st_fastest, L()->st_farthest };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *box = lv_obj_create(c);
        lv_obj_set_size(box, tile_w, UISY(62));
        lv_obj_set_pos(box, i * tile_sp, UISY(36));
        lv_obj_set_style_bg_color(box, COL_ROW, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_radius(box, UISY(8), 0);
        lv_obj_set_style_pad_all(box, UISY(8), 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *n = make_label(box, UIFONT(&font_pl_14, &font_pl_8), COL_DIM);
        lv_label_set_text(n, names[i]);
        lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
        lv_obj_set_size(n, tile_w - UISX(16), UISY(18));
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, -UISY(4));
        s_sv_vals[i] = make_label(box, UIFONT(&font_pl_16, &font_pl_10), COL_TEXT);
        lv_obj_align(s_sv_vals[i], LV_ALIGN_BOTTOM_LEFT, 0, UISY(4));
    }

    lv_obj_t *ch = make_label(c, UIFONT(&font_pl_14, &font_pl_8), COL_DIM);
    lv_label_set_text(ch, L()->st_hourly);
    lv_obj_set_pos(ch, 0, UISY(112));

    s_sv_chart = lv_chart_create(c);
    lv_obj_set_size(s_sv_chart, DTL_W, UISY(120) + vext);
    lv_obj_set_pos(s_sv_chart, 0, UISY(134));
    lv_chart_set_type(s_sv_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(s_sv_chart, 24);
    lv_chart_set_div_line_count(s_sv_chart, 3, 0);
    lv_obj_set_style_bg_color(s_sv_chart, COL_ROW, 0);
    lv_obj_set_style_border_width(s_sv_chart, 0, 0);
    lv_obj_set_style_pad_column(s_sv_chart, 2, LV_PART_ITEMS);
    s_sv_series = lv_chart_add_series(s_sv_chart, COL_ACCENT, LV_CHART_AXIS_PRIMARY_Y);

    lv_obj_t *th = make_label(c, UIFONT(&font_pl_14, &font_pl_8), COL_DIM);
    lv_label_set_text(th, L()->st_top_airlines);
    lv_obj_set_pos(th, 0, UISY(272) + vext);

    for (int i = 0; i < 8; i++) {
        s_sv_top[i] = make_label(c, UIFONT(&font_pl_16, &font_pl_10), COL_TEXT);
        lv_obj_set_pos(s_sv_top[i], (i % 2) * (DTL_W / 2 + UISX(7)), UISY(298) + vext + (i / 2) * UISY(26));
        lv_label_set_text(s_sv_top[i], "");
        if (i >= 6) {
            lv_obj_add_flag(s_sv_top[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    s_sv_days = make_label(c, UIFONT(&font_pl_14, &font_pl_8), COL_DIM);
    lv_obj_align(s_sv_days, LV_ALIGN_TOP_RIGHT, 0, UISY(6));
    lv_label_set_text(s_sv_days, "");

    s_sv_metar = make_label(c, UIFONT(&font_pl_14, &font_pl_8), COL_DIM);
    lv_obj_set_pos(s_sv_metar, 0, UISY(378) + vext);
    lv_obj_set_size(s_sv_metar, DTL_W, UISY(18));
    lv_label_set_long_mode(s_sv_metar, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_sv_metar, "");
}

static void render_stats_panel(void)
{
    /* Hourly counters move once per new aircraft at most: skip the whole
     * chart + label rewrite (a full-panel invalidation) when the stats
     * snapshot, METAR and language are unchanged since the last pass. */
    static app_stats_t s_sv_last;
    static uint32_t s_sv_last_sig;
    static bool s_sv_valid;   /* zeroed stats equal the zeroed snapshot */
    uint32_t sig = (uint32_t)settings_get()->metar_decoded |
                   ((uint32_t)settings_get()->metric_units << 1) |
                   ((uint32_t)settings_get()->lang << 2) |
                   ((uint32_t)(metar_get()[0]) << 8) |
                   ((uint32_t)strlen(metar_get()) << 16);
    if (s_sv_valid &&
        memcmp(&s_stats_snap, &s_sv_last, sizeof(s_sv_last)) == 0 &&
        sig == s_sv_last_sig) {
        return;
    }
    s_sv_valid = true;
    s_sv_last = s_stats_snap;
    s_sv_last_sig = sig;

    char buf[48];
    snprintf(buf, sizeof(buf), "%d", s_stats_snap.unique);
    lv_label_set_text(s_sv_vals[0], buf);
    units_alt(s_stats_snap.max_alt_ft, buf, sizeof(buf));
    lv_label_set_text(s_sv_vals[1], buf);
    units_speed(s_stats_snap.max_gs_kt, buf, sizeof(buf));
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
    char mt[200];
    if (settings_get()->metar_decoded) {
        metar_decoded(mt, sizeof(mt));
    } else {
        strlcpy(mt, metar_get(), sizeof(mt));
        char *rmk = strstr(mt, " RMK");
        if (rmk != NULL) {
            *rmk = '\0';    /* remarks are noise on a one-line display */
        }
    }
    lv_label_set_text(s_sv_metar, mt);

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
    lv_obj_set_size(panel, SCR_W - LIST_W, SCR_H - HEADER_H);
    lv_obj_set_pos(panel, LIST_W, HEADER_H);
    s_detail_panel = panel;

    s_detail_empty = make_label(panel, UIFONT(&font_pl_20, &font_pl_12), COL_DIM);
    lv_label_set_text(s_detail_empty, L()->waiting_aircraft);
    lv_obj_center(s_detail_empty);

    s_detail_content = lv_obj_create(panel);
    lv_obj_set_size(s_detail_content, SCR_W - LIST_W, SCR_H - HEADER_H);
    lv_obj_set_pos(s_detail_content, 0, 0);
    lv_obj_set_style_bg_opa(s_detail_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_detail_content, 0, 0);
    lv_obj_set_style_pad_all(s_detail_content, UISY(16), 0);
    lv_obj_clear_flag(s_detail_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_detail_content, LV_OBJ_FLAG_HIDDEN);

/* The app build, the P4 boards (1280x720/1280x800) and the 7B (1024x600,
 * #50) get the big type tier; the big fonts are only compiled for those. */
#if defined(APKFLIGHT) || defined(CONFIG_IDF_TARGET_ESP32P4) || \
    defined(CONFIG_CANFLIGHT_BOARD_WAVESHARE_7B)
    s_big = DTL_VEXT >= 160;
    s_f_code = s_big ? UIFONT(&lv_font_montserrat_44, &lv_font_montserrat_28) : UIFONT(&lv_font_montserrat_32, &lv_font_montserrat_20);
    s_f_val = s_big ? UIFONT(&lv_font_montserrat_28, &lv_font_montserrat_16) : UIFONT(&lv_font_montserrat_20, &lv_font_montserrat_12);
    s_f_name = s_big ? UIFONT(&font_pl_20, &font_pl_12) : UIFONT(&font_pl_16, &font_pl_10);
    s_f_small = s_big ? UIFONT(&font_pl_16, &font_pl_10) : UIFONT(&font_pl_14, &font_pl_8);
#else
    s_big = false;
    s_f_code = UIFONT(&lv_font_montserrat_32, &lv_font_montserrat_20);
    s_f_val = UIFONT(&lv_font_montserrat_20, &lv_font_montserrat_12);
    s_f_name = UIFONT(&font_pl_16, &font_pl_10);
    s_f_small = UIFONT(&font_pl_14, &font_pl_8);
#endif
    s_city_y = (s_big ? UISY(176) : UISY(152)) + DTL_Y1;
    s_dcity_w = s_big ? UISX(260) : UISX(200);
    int logo_d = s_big ? UISY(128) : UISY(90);

    /* Top: logo + names */
    s_logo_fallback = lv_obj_create(s_detail_content);
    lv_obj_set_size(s_logo_fallback, logo_d, logo_d);
    lv_obj_set_pos(s_logo_fallback, 0, 0);
    lv_obj_set_style_bg_color(s_logo_fallback, COL_ROW_SEL, 0);
    lv_obj_set_style_radius(s_logo_fallback, logo_d / 2, 0);
    lv_obj_set_style_border_width(s_logo_fallback, 0, 0);
    lv_obj_clear_flag(s_logo_fallback, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *fb_label = make_label(s_logo_fallback, UIFONT(&lv_font_montserrat_24, &lv_font_montserrat_14), COL_TEXT);
    lv_obj_center(fb_label);

    s_logo_img = lv_img_create(s_detail_content);
    lv_obj_set_pos(s_logo_img, 0, 0);
    lv_obj_set_style_radius(s_logo_img, UISY(12), 0);
    lv_obj_set_style_clip_corner(s_logo_img, true, 0);
    if (logo_d != 90) {   /* logos are ~90px sources; scale to the tier */
        lv_img_set_zoom(s_logo_img, 256 * logo_d / 90);
        lv_img_set_pivot(s_logo_img, 0, 0);
        lv_img_set_size_mode(s_logo_img, LV_IMG_SIZE_MODE_REAL);
    }
    lv_obj_add_flag(s_logo_img, LV_OBJ_FLAG_HIDDEN);

    int name_x = logo_d + 16;
    s_callsign_label = make_label(s_detail_content, s_f_code, COL_TEXT);
    lv_obj_set_pos(s_callsign_label, name_x, 0);
    s_airline_label = make_label(s_detail_content, s_f_name, COL_DIM);
    lv_obj_set_pos(s_airline_label, name_x, s_big ? UISY(56) : UISY(38));
    s_type_label = make_label(s_detail_content, s_f_name, COL_ACCENT);
    lv_obj_set_pos(s_type_label, name_x, s_big ? UISY(88) : UISY(62));

    lv_obj_t *btn_map = lv_btn_create(s_detail_content);
    lv_obj_set_size(btn_map, s_big ? UISX(118) : UISX(96), s_big ? UISY(48) : UISY(40));
    lv_obj_align(btn_map, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_map, COL_ROW, 0);
    lv_obj_add_event_cb(btn_map, map_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ml = make_label(btn_map, s_f_name, COL_TEXT);
    lv_label_set_text_fmt(ml, LV_SYMBOL_GPS " %s", L()->map_btn);
    lv_obj_center(ml);

    lv_obj_t *btn_photo = lv_btn_create(s_detail_content);
    lv_obj_set_size(btn_photo, s_big ? UISX(72) : UISX(60), s_big ? UISY(48) : UISY(40));
    lv_obj_align(btn_photo, LV_ALIGN_TOP_RIGHT, s_big ? -UISX(128) : -UISX(104), 0);
    lv_obj_set_style_bg_color(btn_photo, COL_ROW, 0);
    lv_obj_add_event_cb(btn_photo, photo_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pl = make_label(btn_photo, s_f_name, COL_TEXT);
    lv_label_set_text(pl, LV_SYMBOL_IMAGE);
    lv_obj_center(pl);

    /* Route: ORIG -> DEST */
    s_orig_code = make_label(s_detail_content, s_f_code, COL_TEXT);
    lv_obj_set_pos(s_orig_code, 0, UISY(116) + DTL_Y1);
    s_orig_city = make_label(s_detail_content, s_f_small, COL_DIM);
    lv_obj_set_pos(s_orig_city, 0, s_city_y);
    s_orig_flag = lv_img_create(s_detail_content);
    lv_obj_set_pos(s_orig_flag, 0, s_city_y - 2);
    lv_obj_add_flag(s_orig_flag, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *arrow = make_label(s_detail_content,
                                 s_big ? UIFONT(&lv_font_montserrat_32, &lv_font_montserrat_20) : UIFONT(&lv_font_montserrat_24, &lv_font_montserrat_14),
                                 COL_ACCENT);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_pos(arrow, DTL_W / 2 - (s_big ? UISX(10) : UISX(7)), (s_big ? UISY(128) : UISY(122)) + DTL_Y1);

    s_orig_time = make_label(s_detail_content, s_f_name, COL_DIM);
    lv_obj_add_flag(s_orig_time, LV_OBJ_FLAG_HIDDEN);

    s_dest_code = make_label(s_detail_content, s_f_code, COL_TEXT);
    lv_obj_set_pos(s_dest_code, DTL_W - UISX(138), UISY(116) + DTL_Y1);   /* render right-aligns it */

    s_dest_time = make_label(s_detail_content, s_f_name, COL_DIM);
    lv_obj_add_flag(s_dest_time, LV_OBJ_FLAG_HIDDEN);
    s_dest_city = make_label(s_detail_content, s_f_small, COL_DIM);
    lv_obj_set_style_text_align(s_dest_city, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_dest_city, DTL_W - s_dcity_w, s_city_y);
    lv_obj_set_width(s_dest_city, s_dcity_w);
    s_dest_flag = lv_img_create(s_detail_content);
    lv_obj_set_pos(s_dest_flag, DTL_W - UISX(30), s_city_y - 2);
    lv_obj_add_flag(s_dest_flag, LV_OBJ_FLAG_HIDDEN);

    s_progress_bar = lv_bar_create(s_detail_content);
    lv_obj_set_size(s_progress_bar, DTL_W, s_big ? UISY(16) : UISY(12));
    lv_obj_set_pos(s_progress_bar, 0, UISY(180) + DTL_Y2);
    lv_bar_set_range(s_progress_bar, 0, 100);
    lv_obj_set_style_bg_color(s_progress_bar, COL_ROW, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_progress_bar, COL_ACCENT, LV_PART_INDICATOR);

    s_progress_label = make_label(s_detail_content, s_f_small, COL_DIM);
    lv_obj_set_pos(s_progress_label, 0, (s_big ? UISY(206) : UISY(200)) + DTL_Y2);

    s_look_label = make_label(s_detail_content, s_f_small, COL_ACCENT);
    lv_obj_set_pos(s_look_label, 0, (s_big ? UISY(231) : UISY(219)) + DTL_Y2);
    lv_obj_set_width(s_look_label, DTL_W);
    lv_label_set_long_mode(s_look_label, LV_LABEL_LONG_DOT);

    /* Stats grid */
    lv_obj_t *grid = lv_obj_create(s_detail_content);
    lv_obj_set_size(grid, DTL_W + UISX(10), s_big ? UISY(172) : UISY(130));
    lv_obj_set_pos(grid, 0, (s_big ? UISY(252) : UISY(238)) + DTL_Y3);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    s_extra_label = make_label(s_detail_content, s_f_small, COL_DIM);
    lv_obj_set_pos(s_extra_label, 0, UISY(378) + DTL_VEXT);
    lv_obj_set_width(s_extra_label, DTL_W);
    lv_label_set_long_mode(s_extra_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_speed(s_extra_label, 12, 0);

    make_stat(grid, 0, 0, L()->st_alt, &s_stat_vals[0]);
    make_stat(grid, 1, 0, L()->st_speed, &s_stat_vals[1]);
    make_stat(grid, 2, 0, L()->st_vrate, &s_stat_vals[2]);
    make_stat(grid, 0, 1, L()->st_dist, &s_stat_vals[3]);
    make_stat(grid, 1, 1, L()->st_track, &s_stat_vals[4]);
    lv_obj_t *regbox = make_stat(grid, 2, 1, L()->st_reg, &s_stat_vals[5]);
    s_reg_flag = lv_img_create(regbox);
    lv_obj_align(s_reg_flag, LV_ALIGN_TOP_RIGHT, 0, -2);
    lv_obj_add_flag(s_reg_flag, LV_OBJ_FLAG_HIDDEN);

    /* ship detail: same panel, own content shown when a ship is selected */
    s_ship_content = lv_obj_create(panel);
    lv_obj_set_size(s_ship_content, SCR_W - LIST_W, SCR_H - HEADER_H);
    lv_obj_set_pos(s_ship_content, 0, 0);
    lv_obj_set_style_bg_opa(s_ship_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ship_content, 0, 0);
    lv_obj_set_style_pad_all(s_ship_content, UISY(16), 0);
    lv_obj_clear_flag(s_ship_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ship_content, LV_OBJ_FLAG_HIDDEN);

    s_ship_name = make_label(s_ship_content, s_f_code, COL_TEXT);
    lv_obj_set_pos(s_ship_name, 0, 0);
    lv_obj_set_width(s_ship_name, DTL_W);
    lv_label_set_long_mode(s_ship_name, LV_LABEL_LONG_DOT);
    s_ship_sub = make_label(s_ship_content, s_f_name, lv_color_hex(0x4fd1c5));
    lv_obj_set_pos(s_ship_sub, 0, s_big ? UISY(62) : UISY(48));

    lv_obj_t *sgrid = lv_obj_create(s_ship_content);
    lv_obj_set_size(sgrid, DTL_W + UISX(10), s_big ? UISY(172) : UISY(130));
    lv_obj_set_pos(sgrid, 0, s_big ? UISY(130) : UISY(104));
    lv_obj_set_style_bg_opa(sgrid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sgrid, 0, 0);
    lv_obj_set_style_pad_all(sgrid, 0, 0);
    lv_obj_clear_flag(sgrid, LV_OBJ_FLAG_SCROLLABLE);
    make_stat(sgrid, 0, 0, L()->st_speed, &s_ship_vals[0]);
    make_stat(sgrid, 1, 0, L()->st_track, &s_ship_vals[1]);
    make_stat(sgrid, 2, 0, L()->st_dist, &s_ship_vals[2]);
    make_stat(sgrid, 0, 1, L()->ship_dest, &s_ship_vals[3]);
    make_stat(sgrid, 1, 1, "MMSI", &s_ship_vals[4]);
    make_stat(sgrid, 2, 1, L()->ship_pos, &s_ship_vals[5]);
}

void ui_init(void)
{
    s_shown = heap_caps_calloc(MAX_SHOWN, sizeof(*s_shown), MALLOC_CAP_SPIRAM);
    assert(s_shown != NULL);
    /* ship bookkeeping is bulky; keep it out of internal RAM */
    s_row_ships = heap_caps_calloc(MAX_SHOWN, sizeof(ship_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_marker_ships = heap_caps_calloc(40, sizeof(ship_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pop_ships = heap_caps_calloc(GRP_MEMBERS, sizeof(ship_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);

    build_header(scr);
    build_list(scr);
    build_detail(scr);
    build_map_panel(scr);
    build_radar_panel(scr);
    build_stats_panel(scr);
    build_retro_panel(scr);

    /* All big tile framebuffers are session-persistent once used, so they
     * are allocated HERE, while PSRAM is one unfragmented block. Lazy
     * allocation used to fail on the 7B (1.2 MB each): after an hour of
     * caches and TLS churn no contiguous block that size was left, and
     * the ambient map never came up (issue #12, second act). Boot-time
     * allocation costs nothing extra in steady state - every buffer ends
     * up allocated anyway once each view has been visited. */
    s_amb_tiles = heap_caps_malloc((size_t)AMB_RENDER_W * AMB_RENDER_H * 2, MALLOC_CAP_SPIRAM);
    s_radar_tiles = heap_caps_malloc((size_t)RADAR_RENDER_W * RADAR_RENDER_H * 2, MALLOC_CAP_SPIRAM);
    s_emb_tiles = heap_caps_malloc((size_t)EMB_RENDER_W * EMB_RENDER_H * 2, MALLOC_CAP_SPIRAM);
    if (s_amb_tiles == NULL || s_radar_tiles == NULL || s_emb_tiles == NULL) {
        ESP_LOGE(TAG, "tile framebuffer alloc FAILED at boot (amb %d radar %d emb %d)",
                 s_amb_tiles != NULL, s_radar_tiles != NULL, s_emb_tiles != NULL);
    }
    s_cycle_timer = lv_timer_create(cycle_timer_cb, CYCLE_MS, NULL);
    lv_timer_pause(s_cycle_timer);
    lv_timer_create(clock_timer_cb, 5000, NULL);
    lv_timer_create(logo_tick_cb, 500, NULL);
    lv_timer_create(idle_timer_cb, 10000, NULL);
}

static bool s_update_avail;
static char s_update_tag[16];
static lv_timer_t *s_gear_blink;

static void gear_blink_cb(lv_timer_t *t)
{
    static bool on;
    on = !on;
    if (s_gear_label != NULL) {
        lv_obj_set_style_text_color(s_gear_label,
                                    on ? lv_color_hex(0xffd166) : COL_TEXT, 0);
    }
}

void ui_set_update_available(bool available, const char *tag)
{
    s_update_avail = available;
    if (tag != NULL) {
        strlcpy(s_update_tag, tag, sizeof(s_update_tag));
    }
    if (available && s_gear_blink == NULL) {
        s_gear_blink = lv_timer_create(gear_blink_cb, 600, NULL);
    } else if (!available && s_gear_blink != NULL) {
        lv_timer_del(s_gear_blink);
        s_gear_blink = NULL;
        if (s_gear_label != NULL) {
            lv_obj_set_style_text_color(s_gear_label, COL_TEXT, 0);
        }
    }
}

bool ui_update_available(void)
{
    return s_update_avail;
}

const char *ui_update_tag(void)
{
    return s_update_tag;
}

static lv_obj_t *s_flyover_banner;

static void flyover_banner_close(lv_timer_t *t)
{
    if (s_flyover_banner != NULL) {
        lv_obj_del(s_flyover_banner);
        s_flyover_banner = NULL;
    }
}

static void flyover_banner_click(lv_event_t *e)
{
    if (s_flyover_banner != NULL) {
        lv_obj_del(s_flyover_banner);
        s_flyover_banner = NULL;
    }
}

void ui_flyover_banner(const char *callsign, int eta_min, double cpa_km)
{
    if (!lvgl_port_lock(200 / portTICK_PERIOD_MS)) {
        return;
    }
    if (s_flyover_banner != NULL) {
        lv_obj_del(s_flyover_banner);
        s_flyover_banner = NULL;
    }
    s_flyover_banner = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_flyover_banner, UISX(460), UISY(56));
    lv_obj_align(s_flyover_banner, LV_ALIGN_TOP_MID, 0, UISY(54));
    lv_obj_set_style_bg_color(s_flyover_banner, lv_color_hex(0xb8860b), 0);
    lv_obj_set_style_border_width(s_flyover_banner, 0, 0);
    lv_obj_set_style_radius(s_flyover_banner, UISY(10), 0);
    lv_obj_set_style_shadow_width(s_flyover_banner, 16, 0);
    lv_obj_set_style_shadow_opa(s_flyover_banner, LV_OPA_50, 0);
    lv_obj_clear_flag(s_flyover_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_flyover_banner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_flyover_banner, flyover_banner_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = make_label(s_flyover_banner, UIFONT(&font_pl_16, &font_pl_10), lv_color_hex(0xffffff));
    char cpa[64], txt[96];
    snprintf(cpa, sizeof(cpa), L()->cpa_fmt, eta_min, cpa_km);
    snprintf(txt, sizeof(txt), LV_SYMBOL_EYE_OPEN "  %s  \xC2\xB7  %s", callsign, cpa);
    lv_label_set_text(l, txt);
    lv_obj_center(l);

    lv_timer_t *t = lv_timer_create(flyover_banner_close, 20000, NULL);
    lv_timer_set_repeat_count(t, 1);
    lvgl_port_unlock();
}

void ui_set_status_alert(bool alert)
{
    if (s_status_label != NULL) {
        lv_obj_set_style_text_color(s_status_label,
                                    alert ? lv_color_hex(0xff5252) : COL_DIM, 0);
        lv_obj_set_style_text_font(s_status_label,
                                   alert ? UIFONT(&font_pl_20, &font_pl_12) : UIFONT(&font_pl_14, &font_pl_8), 0);
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
    if (s_amb != NULL && s_amb_wx != NULL) {
        lv_label_set_text(s_amb_wx, text);
    }
}

static void render_list_selection(void)
{
    /* LVGL style writes have no old-value compare: an unconditional write
     * invalidates the row even when nothing changed, which used to repaint
     * the whole list column every refresh. Shadow the last state per row
     * (3 = never written; theme changes bump s_theme_gen to force a pass). */
    static uint8_t s_row_sel_shadow[MAX_SHOWN];
    static uint8_t s_row_sel_theme = 0xFF;
    if (s_row_sel_theme != (uint8_t)settings_get()->theme) {
        s_row_sel_theme = (uint8_t)settings_get()->theme;
        memset(s_row_sel_shadow, 3, sizeof(s_row_sel_shadow));
    }
    for (int i = 0; i < MAX_SHOWN; i++) {
        bool sel;
        if (i < s_list_plane_rows) {
            sel = s_row_plane_idx[i] == s_selected && s_sel_ship_mmsi == 0;
        } else {
            int si = i - s_list_plane_rows;
            sel = si < s_row_ship_count && s_sel_ship_mmsi != 0 &&
                  s_row_ships[si].mmsi == s_sel_ship_mmsi;
        }
        if (s_row_sel_shadow[i] == (uint8_t)sel) {
            continue;
        }
        s_row_sel_shadow[i] = (uint8_t)sel;
        lv_obj_set_style_bg_color(s_list_rows[i], sel ? COL_ROW_SEL : COL_ROW, 0);
        /* the dim sub-line drowns in the selection color on some themes
           (#11): brighten it towards the text color while selected */
        lv_obj_t *info = lv_obj_get_child(s_list_rows[i], 2);
        if (info != NULL) {
            lv_obj_set_style_text_color(info,
                sel ? lv_color_mix(COL_TEXT, COL_DIM, 170) : COL_DIM, 0);
        }
    }
}

/* fresh data for the selected ship; clears the selection when it is gone */
static bool sel_ship_get(ship_t *out)
{
    if (s_sel_ship_mmsi == 0) {
        return false;
    }
    for (int i = 0; i < s_row_ship_count; i++) {
        if (s_row_ships[i].mmsi == s_sel_ship_mmsi) {
            *out = s_row_ships[i];
            return true;
        }
    }
    s_sel_ship_mmsi = 0;
    return false;
}

static void render_detail(void)
{
    ship_t sh;
    static ship_t s_ship_snap;
    static bool s_ship_snap_ok;
    if (sel_ship_get(&sh)) {
        lv_obj_add_flag(s_detail_content, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_detail_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ship_content, LV_OBJ_FLAG_HIDDEN);
        if (s_ship_snap_ok && memcmp(&sh, &s_ship_snap, sizeof(sh)) == 0) {
            return;   /* AIS updates arrive every few seconds at most */
        }
        s_ship_snap = sh;
        s_ship_snap_ok = true;
        char nm[28];
        if (sh.name[0]) {
            strlcpy(nm, sh.name, sizeof(nm));
        } else {
            snprintf(nm, sizeof(nm), "MMSI %lu", (unsigned long)sh.mmsi);
        }
        lv_label_set_text(s_ship_name, nm);
        lv_label_set_text(s_ship_sub, ship_type_word(sh.stype));
        char v[40];
        if (settings_get()->metric_units) {
            snprintf(v, sizeof(v), "%.0f km/h", (double)(sh.sog_kt * 1.852f));
        } else {
            snprintf(v, sizeof(v), "%.1f kt", (double)sh.sog_kt);
        }
        lv_label_set_text(s_ship_vals[0], v);
        snprintf(v, sizeof(v), "%d\xC2\xB0 %s", (int)sh.cog_deg,
                 lang_compass((int)sh.cog_deg));
        lv_label_set_text(s_ship_vals[1], v);
        snprintf(v, sizeof(v), "%.1f km", (double)sh.dist_km);
        lv_label_set_text(s_ship_vals[2], v);
        lv_label_set_text(s_ship_vals[3], sh.dest[0] ? sh.dest : "-");
        snprintf(v, sizeof(v), "%lu", (unsigned long)sh.mmsi);
        lv_label_set_text(s_ship_vals[4], v);
        snprintf(v, sizeof(v), "%.3f %.3f", sh.lat, sh.lon);
        lv_label_set_text(s_ship_vals[5], v);
        return;
    }
    if (s_ship_content != NULL) {
        lv_obj_add_flag(s_ship_content, LV_OBJ_FLAG_HIDDEN);
    }
    s_ship_snap_ok = false;   /* re-render the ship card on the next select */
    if (s_selected < 0 || s_selected >= s_shown_count) {
        lv_obj_add_flag(s_detail_content, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_detail_empty, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_detail_content, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_detail_empty, LV_OBJ_FLAG_HIDDEN);

    /* The card rewrites ~25 labels; skip all of it (and the invalidations)
     * when the selected flight's data is bit-identical to the last render.
     * The clock minute joins the signature for the airport local times,
     * and a missing logo keeps re-rendering so it can pop in when fetched. */
    static shown_flight_t s_det_snap;
    static int s_det_minute = -1;
    static uint8_t s_det_cfg = 0xFF;
    static bool s_det_had_logo;
    const settings_t *dcfg = settings_get();
    uint8_t cfgsig = (uint8_t)((dcfg->theme << 4) |
                               (dcfg->metric_units << 2) | (dcfg->lang & 3));
    int minute = (int)(time(NULL) / 60);
    if (minute == s_det_minute && cfgsig == s_det_cfg && s_det_had_logo &&
        memcmp(&s_shown[s_selected], &s_det_snap, sizeof(s_det_snap)) == 0) {
        return;
    }
    s_det_snap = s_shown[s_selected];
    s_det_minute = minute;
    s_det_cfg = cfgsig;

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
    s_det_had_logo = logo != NULL;
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
        const lv_img_dsc_t *ofl = flags_get(rt->origin.country);
        if (ofl != NULL) {
            lv_img_set_src(s_orig_flag, ofl);
            lv_obj_clear_flag(s_orig_flag, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_orig_city, flags_width(ofl) + UISX(8), s_city_y);
        } else {
            lv_obj_add_flag(s_orig_flag, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_orig_city, 0, s_city_y);
        }
        lv_label_set_text_fmt(s_orig_city, "%s%s%s%s",
                              rt->origin.city,
                              rt->origin.country[0] ? " (" : "",
                              rt->origin.country,
                              rt->origin.country[0] ? ")" : "");
        lv_label_set_text(s_orig_time, lt);
        if (lt[0]) {
            lv_obj_clear_flag(s_orig_time, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_orig_time, LV_OBJ_FLAG_HIDDEN);
        }
        lv_label_set_text(s_dest_code,
                          rt->destination.iata[0] ? rt->destination.iata : rt->destination.icao);
        airport_local_time(&rt->destination, lt, sizeof(lt));
        const lv_img_dsc_t *dfl = flags_get(rt->destination.country);
        if (dfl != NULL) {
            lv_img_set_src(s_dest_flag, dfl);
            lv_obj_clear_flag(s_dest_flag, LV_OBJ_FLAG_HIDDEN);
            int dfw = flags_width(dfl);
            lv_obj_set_pos(s_dest_flag, DTL_W - dfw, s_city_y - 2);
            lv_obj_set_width(s_dest_city, s_dcity_w - dfw - UISX(8));
        } else {
            lv_obj_add_flag(s_dest_flag, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_width(s_dest_city, s_dcity_w);
        }
        lv_label_set_text_fmt(s_dest_city, "%s%s%s%s",
                              rt->destination.city,
                              rt->destination.country[0] ? " (" : "",
                              rt->destination.country,
                              rt->destination.country[0] ? ")" : "");
        lv_label_set_text(s_dest_time, lt);
        if (lt[0]) {
            lv_obj_clear_flag(s_dest_time, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_dest_time, LV_OBJ_FLAG_HIDDEN);
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
        lv_obj_set_pos(s_orig_city, 0, s_city_y);
        lv_label_set_text(s_dest_code, "----");
        lv_label_set_text(s_dest_city, "");
        lv_obj_add_flag(s_orig_flag, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_dest_flag, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_orig_time, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_dest_time, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(s_progress_label, "");
    }

    /* Right-align the destination code and snap the local times to the
     * airport codes (the code labels are content-sized). */
    lv_obj_update_layout(s_detail_content);
    lv_obj_set_x(s_dest_code, DTL_W - lv_obj_get_width(s_dest_code));
    lv_obj_align_to(s_orig_time, s_orig_code, LV_ALIGN_OUT_RIGHT_BOTTOM, UISX(10), -UISY(6));
    lv_obj_align_to(s_dest_time, s_dest_code, LV_ALIGN_OUT_LEFT_BOTTOM, -UISX(10), -UISY(6));

    if (ac->on_ground) {
        lv_label_set_text(s_stat_vals[0], L()->ground);
    } else {
        units_alt(ac->alt_baro_ft, buf, sizeof(buf));
        lv_label_set_text(s_stat_vals[0], buf);
    }
    units_speed(ac->gs_kts, buf, sizeof(buf));
    lv_label_set_text(s_stat_vals[1], buf);
    units_vrate(ac->baro_rate_fpm, buf, sizeof(buf));
    lv_label_set_text(s_stat_vals[2], buf);
    snprintf(buf, sizeof(buf), "%.1f km", ac->dist_nm >= 0 ? ac->dist_nm * 1.852 : 0.0);
    lv_label_set_text(s_stat_vals[3], buf);
    snprintf(buf, sizeof(buf), "%.0f\xC2\xB0", (double)ac->track_deg);
    lv_label_set_text(s_stat_vals[4], buf);
    char extra[128];
    size_t el = 0;
    extra[0] = '\0';
    if (ac->squawk[0]) {
        el += snprintf(extra + el, sizeof(extra) - el, "Squawk %s", ac->squawk);
    }
    if (ac->category[0]) {
        el += snprintf(extra + el, sizeof(extra) - el, "%s%s %s",
                       el ? "  \xC2\xB7  " : "", L()->cat_word, ac->category);
    }
    flight_class_t fc = flight_class(ac);
    if (fc != FCLS_LINER) {
        el += snprintf(extra + el, sizeof(extra) - el, "%s%s",
                       el ? "  \xC2\xB7  " : "", L()->cls_names[fc]);
    }
    if (rt != NULL && rt->valid && !ac->on_ground && ac->gs_kts > 80 &&
        rt->destination.tz_known) {
        time_t now = time(NULL);
        if (now > 1600000000) {
            double rem_km = geo_haversine_km(ac->lat, ac->lon,
                                             rt->destination.lat, rt->destination.lon);
            time_t arr = now + (time_t)(rem_km / (ac->gs_kts * 1.852) * 3600.0)
                             + rt->destination.tz_offset_s;
            struct tm tm;
            gmtime_r(&arr, &tm);
            char hh[12], af[64];
            clock_fmt(hh, sizeof(hh), tm.tm_hour, tm.tm_min);
            snprintf(af, sizeof(af), L()->arr_fmt, hh);
            el += snprintf(extra + el, sizeof(extra) - el, "%s%s",
                           el ? "  \xC2\xB7  " : "", af);
        }
    }
    char apt[8];
    int eta_min;
    if (ac->has_pos &&
        runways_approach(ac->lat, ac->lon, ac->track_deg, ac->alt_baro_ft,
                         ac->baro_rate_fpm, ac->gs_kts, apt, sizeof(apt),
                         &eta_min)) {
        airport_t ai;
        const char *where = airports_lookup(apt, &ai) && ai.city[0] ? ai.city : apt;
        char ap[64];
        snprintf(ap, sizeof(ap), L()->apr_fmt, where, eta_min);
        el += snprintf(extra + el, sizeof(extra) - el, "%s%s",
                       el ? "  \xC2\xB7  " : "", ap);
    }
    lv_label_set_text(s_extra_label, extra);

    const char *cc = reg_country(ac->reg);
    const lv_img_dsc_t *rfl = ac->reg[0] && cc != NULL ? flags_get(cc) : NULL;
    if (rfl != NULL) {
        lv_img_set_src(s_reg_flag, rfl);
        lv_obj_clear_flag(s_reg_flag, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_reg_flag, LV_OBJ_FLAG_HIDDEN);
    }
    if (ac->reg[0] && cc != NULL) {
        /* ASCII only: this tile uses the built-in Montserrat font */
        snprintf(buf, sizeof(buf), "%s (%s)", ac->reg, cc);
        lv_label_set_text(s_stat_vals[5], buf);
    } else {
        lv_label_set_text(s_stat_vals[5], ac->reg[0] ? ac->reg : "-");
    }

    /* spotter line: where to look + flyover prediction */
    char look[128] = "";
    /* below ~1 degree the aircraft is behind the horizon (a tracked flight
     * hundreds of km away): no "look there" hint */
    if (ac->has_pos && !ac->on_ground && ac->dist_nm >= 0 && s_home_ok &&
        geo_elevation_deg(ac->dist_nm * 1.852, ac->alt_baro_ft) >= 1.0) {
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
    s_all_count = 0;
    for (int i = 0; i < list->count && i < MAX_AIRCRAFT; i++) {
        if (!list->ac[i].has_pos) {
            continue;
        }
        amb_target_t *t = &s_all[s_all_count++];
        strlcpy(t->callsign, list->ac[i].callsign, sizeof(t->callsign));
        t->lat = (float)list->ac[i].lat;
        t->lon = (float)list->ac[i].lon;
        t->track = list->ac[i].track_deg;
        t->dist_nm = list->ac[i].dist_nm;
        t->dir_deg = list->ac[i].dir_deg;
        t->alt_ft = list->ac[i].alt_baro_ft;
        t->ground = list->ac[i].on_ground;
        t->fcls = (uint8_t)flight_sprite(&list->ac[i]);
    }
    s_shown_count = list->count < MAX_SHOWN ? list->count : MAX_SHOWN;
    for (int i = 0; i < s_shown_count; i++) {
        s_shown[i].ac = list->ac[i];
        const route_info_t *rt = routes_get_cached(list->ac[i].callsign);
        if (rt != NULL && rt->valid && list->ac[i].has_pos &&
            !geo_route_plausible_dir(rt->origin.lat, rt->origin.lon,
                                     rt->destination.lat, rt->destination.lon,
                                     list->ac[i].lat, list->ac[i].lon,
                                     list->ac[i].track_deg, list->ac[i].gs_kts,
                                     list->ac[i].baro_rate_fpm)) {
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
    /* #44: the detail view sticks to the nearest aircraft (the list is
     * sorted by distance). A tap on another one keeps it for 30 s. */
    if (settings_get()->pin_nearest && s_shown_count > 0 && s_selected != 0 &&
        lv_disp_get_inactive_time(NULL) > 30000U) {
        s_selected = 0;
        strlcpy(s_selected_hex, s_shown[0].ac.hex, sizeof(s_selected_hex));
    }

    /* The ambient overlay is opaque and full-screen: rendering the list
     * and right panel under it would invalidate large areas and repaint
     * the covering map image for nothing. amb_close() re-renders once. */
    if (s_amb == NULL) {
        render_list_rows();
        render_list_selection();
        render_right();
    }
    render_ambient();
}

static const char *ship_type_word(uint8_t t)
{
    if (t == 30) return "FISHING";
    if (t == 31 || t == 32 || t == 52) return "TUG";
    if (t == 35) return "MILITARY";
    if (t == 36) return "SAIL";
    if (t == 37) return "PLEASURE";
    if (t >= 60 && t <= 69) return "PASSENGER";
    if (t >= 70 && t <= 79) return "CARGO";
    if (t >= 80 && t <= 89) return "TANKER";
    return "SHIP";
}

/* on the radar map view the list mirrors the visible frame */
static bool radar_view_filter(double lat, double lon, float dist_km)
{
    if (s_view_mode != VIEW_RADAR || !s_radar_view_ok) {
        return true;
    }
    int x, y;
    return radar_place(lat, lon, dist_km, true,
                       settings_get()->radius_nm, &x, &y);
}

static void render_list_rows(void)
{
    bool ships_on = settings_get()->ships_enabled;
    if (s_list_mode_btnm != NULL) {
        if (ships_on) {
            lv_obj_clear_flag(s_list_mode_btnm, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_list_mode_btnm, LV_OBJ_FLAG_HIDDEN);
        }
    }
    uint8_t mode = ships_on ? s_list_mode : 0;

    ship_t *ships = ui_ship_buf();
    int n_ships = 0;
    if (mode != 0 && ships != NULL) {
        n_ships = ships_get(ships, MAX_SHIPS);
        /* nearest first */
        for (int i = 1; i < n_ships; i++) {
            ship_t tmp = ships[i];
            int j = i - 1;
            while (j >= 0 && ships[j].dist_km > tmp.dist_km) {
                ships[j + 1] = ships[j];
                j--;
            }
            ships[j + 1] = tmp;
        }
    }
    int n_planes = 0;
    if (mode != 1) {
        for (int i = 0; i < s_shown_count && n_planes < MAX_SHOWN; i++) {
            if (radar_view_filter(s_shown[i].ac.lat, s_shown[i].ac.lon,
                                  (float)(s_shown[i].ac.dist_nm * 1.852))) {
                s_row_plane_idx[n_planes++] = i;
            }
        }
    }
    s_list_plane_rows = n_planes;
    s_row_ship_count = 0;
    for (int i = 0; s_row_ships != NULL && i < n_ships && s_row_ship_count < MAX_SHOWN; i++) {
        if (radar_view_filter(ships[i].lat, ships[i].lon, ships[i].dist_km)) {
            s_row_ships[s_row_ship_count++] = ships[i];
        }
    }

    for (int i = 0; i < MAX_SHOWN; i++) {
        if (i >= n_planes && i - n_planes < n_ships) {
            const ship_t *sh = &ships[i - n_planes];
            lv_obj_t *row = s_list_rows[i];
            lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *cs_label = lv_obj_get_child(row, 0);
            char nm[24];
            if (sh->name[0]) {
                strlcpy(nm, sh->name, sizeof(nm));
            } else {
                snprintf(nm, sizeof(nm), "%lu", (unsigned long)sh->mmsi);
            }
            label_set_if_changed(cs_label, nm);
            text_color_if_changed(cs_label, lv_color_hex(0x4fd1c5));
            lv_obj_t *typechip = lv_obj_get_child(row, 1);
            label_set_if_changed(typechip, sh->dest[0] ? sh->dest : ship_type_word(sh->stype));
            text_color_if_changed(typechip, lv_color_hex(0x4fd1c5));
            char info[64];
            char us[20];
            snprintf(info, sizeof(info), "%s  %s  %.1f km",
                     ship_type_word(sh->stype),
                     units_speed(sh->sog_kt, us, sizeof(us)), (double)sh->dist_km);
            label_set_if_changed(lv_obj_get_child(row, 2), info);
            lv_obj_add_flag(lv_obj_get_child(row, 3), LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lv_obj_get_child(row, 4), LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        if (i < n_planes) {
            const aircraft_t *ac = &s_shown[s_row_plane_idx[i]].ac;
            lv_obj_t *row = s_list_rows[i];
            lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);

            lv_obj_t *cs_label = lv_obj_get_child(row, 0);
            label_set_if_changed(cs_label, ac->callsign[0] ? ac->callsign : ac->hex);
            /* gold for military / heavies / watchlist hits */
            bool interesting = flight_is_interesting(ac, settings_get()->watch_regs);
            text_color_if_changed(cs_label,
                                  interesting ? lv_color_hex(0xffd166) : COL_TEXT);
            lv_obj_t *typechip = lv_obj_get_child(row, 1);
            label_set_if_changed(typechip, ac->type_icao[0] ? ac->type_icao : "?");
            text_color_if_changed(typechip, class_color(flight_class(ac)));

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
                char ua[20], us[20];
                snprintf(info, sizeof(info), "%s%s  %s  %.1f km",
                         trend, units_alt(ac->alt_baro_ft, ua, sizeof(ua)),
                         units_speed(ac->gs_kts, us, sizeof(us)), ac->dist_nm * 1.852);
            }
            label_set_if_changed(lv_obj_get_child(row, 2), info);

            /* origin/destination codes on the right of the info line (#28) */
            lv_obj_t *route_lbl = lv_obj_get_child(row, 3);
            const route_info_t *rrt = &s_shown[s_row_plane_idx[i]].route;
            if (settings_get()->show_route && rrt->callsign[0] && rrt->valid) {
                char rtxt[16];
                snprintf(rtxt, sizeof(rtxt), "%s " LV_SYMBOL_RIGHT " %s",
                         rrt->origin.iata[0] ? rrt->origin.iata : rrt->origin.icao,
                         rrt->destination.iata[0] ? rrt->destination.iata
                                                  : rrt->destination.icao);
                label_set_if_changed(route_lbl, rtxt);
                lv_obj_clear_flag(route_lbl, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(route_lbl, LV_OBJ_FLAG_HIDDEN);
            }

            lv_obj_t *logo_img = lv_obj_get_child(row, 4);
            const char *code = airline_code(ac, &s_shown[s_row_plane_idx[i]].route);
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
}


/* ---------- toast + physical input actions (#13) ---------- */

static lv_obj_t *s_toast;

static void toast_close(lv_timer_t *t)
{
    (void)t;
    if (lvgl_port_lock(200 / portTICK_PERIOD_MS)) {
        if (s_toast != NULL) {
            lv_obj_del(s_toast);
            s_toast = NULL;
        }
        lvgl_port_unlock();
    }
}

void ui_toast(const char *text)
{
    if (!lvgl_port_lock(200 / portTICK_PERIOD_MS)) {
        return;
    }
    if (s_toast != NULL) {
        lv_obj_del(s_toast);
        s_toast = NULL;
    }
    s_toast = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -UISY(24));
    lv_obj_set_style_bg_color(s_toast, COL_PANEL, 0);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    lv_obj_set_style_border_color(s_toast, COL_ACCENT, 0);
    lv_obj_set_style_radius(s_toast, UISY(10), 0);
    lv_obj_set_style_pad_all(s_toast, UISY(10), 0);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = make_label(s_toast, UIFONT(&font_pl_16, &font_pl_10), COL_TEXT);
    lv_label_set_text(l, text);
    lv_timer_t *t = lv_timer_create(toast_close, 2200, NULL);
    lv_timer_set_repeat_count(t, 1);
    lvgl_port_unlock();
}

/* UI-side actions for input_ctl. MUST be called while holding the LVGL
 * lock. Returns false for names it does not own. */
bool ui_input_action(const char *a)
{
    /* A button press or API call is user activity: without this the idle
     * watcher reopened the screensaver seconds after a "wake". The
     * actions that darken the screen on purpose must not count. */
    if (strcmp(a, "screensaver") != 0 && strcmp(a, "screen_off") != 0 &&
        !(strcmp(a, "screen_toggle") == 0 && !s_bl_off)) {
        lv_disp_trig_activity(NULL);
    }
    if (strcmp(a, "next_view") == 0 || strcmp(a, "prev_view") == 0) {
        int v = (s_view_mode + (a[0] == 'n' ? 1 : 4)) % 5;
        apply_view(v);
        return true;
    }
    if (strcmp(a, "next_ac") == 0 || strcmp(a, "prev_ac") == 0) {
        if (s_shown_count == 0) {
            return true;
        }
        int d = a[0] == 'n' ? 1 : s_shown_count - 1;
        s_selected = s_selected < 0 ? 0 : (s_selected + d) % s_shown_count;
        strlcpy(s_selected_hex, s_shown[s_selected].ac.hex, sizeof(s_selected_hex));
        s_sel_ship_mmsi = 0;
        render_list_selection();
        render_right();
        int row = row_of_shown(s_selected);
        if (row >= 0) {
            lv_obj_scroll_to_view(s_list_rows[row], LV_ANIM_ON);
        }
        if (s_cycle_timer != NULL) {
            lv_timer_reset(s_cycle_timer);
        }
        return true;
    }
    if (strcmp(a, "zoom_in") == 0 || strcmp(a, "zoom_out") == 0) {
        double z = s_radar_zoom * (strcmp(a, "zoom_in") == 0 ? 1.0 / 1.5 : 1.5);
        if (z < 0.18) z = 0.18;
        if (z > 3.4) z = 3.4;
        if (z > 0.95 && z < 1.06) z = 1.0;
        s_radar_zoom = z;
        radar_tiles_want();
        return true;
    }
    /* Screen control for HTTP (/api/input), MQTT and physical buttons */
    if (strcmp(a, "screensaver") == 0) {
        if (s_amb == NULL) {
            amb_show();
        }
        return true;
    }
    if (strcmp(a, "screen_off") == 0 ||
        (strcmp(a, "screen_toggle") == 0 && !s_bl_off)) {
        waveshare_rgb_lcd_bl_off();
        s_bl_off = true;
        s_bl_forced = true;
        return true;
    }
    if (strcmp(a, "screen_on") == 0 || strcmp(a, "screen_toggle") == 0) {
        if (s_bl_off) {
            waveshare_rgb_lcd_bl_on();
            ui_apply_brightness();
            s_bl_off = false;
        }
        s_bl_forced = false;
        return true;
    }
    if (strcmp(a, "wake") == 0) {
        s_bl_forced = false;
        if (s_bl_off) {
            waveshare_rgb_lcd_bl_on();
            ui_apply_brightness();
            s_bl_off = false;
        }
        if (s_amb != NULL) {
            amb_close();
        }
        return true;
    }
    if (strcmp(a, "list_planes") == 0) { ui_set_list_mode(0); return true; }
    if (strcmp(a, "list_ships") == 0)  { ui_set_list_mode(1); return true; }
    if (strcmp(a, "list_all") == 0)    { ui_set_list_mode(2); return true; }
    return false;
}
