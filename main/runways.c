#include "runways.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "runways";

typedef struct {
    float le_lat, le_lon, he_lat, he_lon;
} runway_t;

static runway_t *s_rw;
static int s_count = -1;    /* -1 = not loaded yet */

static void load(void)
{
    s_count = 0;
    FILE *f = fopen("/assets/runways.tsv", "r");
    if (f == NULL) {
        ESP_LOGW(TAG, "no runway database");
        return;
    }
    int cap = 5000;
    s_rw = heap_caps_malloc(cap * sizeof(runway_t), MALLOC_CAP_SPIRAM);
    if (s_rw == NULL) {
        fclose(f);
        return;
    }
    char line[128];
    while (s_count < cap && fgets(line, sizeof(line), f) != NULL) {
        char icao[8];
        runway_t r;
        if (sscanf(line, "%7s\t%f\t%f\t%f\t%f",
                   icao, &r.le_lat, &r.le_lon, &r.he_lat, &r.he_lon) == 5) {
            s_rw[s_count++] = r;
        }
    }
    fclose(f);
    ESP_LOGI(TAG, "loaded %d runways", s_count);
}

#define RW_COLOR 0x4E19   /* muted teal, readable on the dark map */

static inline void put_px(uint16_t *fb, int w, int h, int x, int y)
{
    if (x >= 0 && x < w && y >= 0 && y < h) {
        fb[(size_t)y * w + x] = RW_COLOR;
    }
}

/* 2 px thick Bresenham segment */
static void draw_line(uint16_t *fb, int w, int h, int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_px(fb, w, h, x0, y0);
        put_px(fb, w, h, x0 + 1, y0);
        put_px(fb, w, h, x0, y0 + 1);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void runways_draw(uint16_t *fb, int w, int h, const tile_view_t *view)
{
    if (s_count < 0) {
        load();
    }
    for (int i = 0; i < s_count; i++) {
        int x0, y0, x1, y1;
        tilemap_project(view, s_rw[i].le_lat, s_rw[i].le_lon, &x0, &y0);
        tilemap_project(view, s_rw[i].he_lat, s_rw[i].he_lon, &x1, &y1);
        /* skip strips fully off screen or degenerate at this zoom */
        if ((x0 < 0 && x1 < 0) || (x0 >= w && x1 >= w) ||
            (y0 < 0 && y1 < 0) || (y0 >= h && y1 >= h)) {
            continue;
        }
        if (abs(x1 - x0) < 2 && abs(y1 - y0) < 2) {
            continue;
        }
        draw_line(fb, w, h, x0, y0, x1, y1);
    }
}
