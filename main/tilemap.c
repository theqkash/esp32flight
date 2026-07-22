#include "tilemap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "extra/libs/png/lodepng.h"

static const char *TAG = "tilemap";

#define TILE_PX     256
#define MAX_ZOOM    7
#define TILE_BUF    (96 * 1024)

/* WGS84 -> normalized web mercator (0..1) */
static void merc_norm(double lat, double lon, double *nx, double *ny)
{
    if (lat > 85.0511) lat = 85.0511;
    if (lat < -85.0511) lat = -85.0511;
    double rad = lat * M_PI / 180.0;
    *nx = (lon + 180.0) / 360.0;
    *ny = (1.0 - log(tan(rad) + 1.0 / cos(rad)) / M_PI) / 2.0;
}

void tilemap_project(const tile_view_t *v, double lat, double lon, int *x, int *y)
{
    double nx, ny;
    merc_norm(lat, lon, &nx, &ny);
    double world = (double)TILE_PX * (1 << v->z);
    *x = (int)(nx * world - v->px0);
    *y = (int)(ny * world - v->py0);
}

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
} tile_sink_t;

static esp_err_t tile_http_cb(esp_http_client_event_t *evt)
{
    tile_sink_t *s = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && s != NULL) {
        size_t n = evt->data_len;
        if (n > s->cap - s->len) {
            n = s->cap - s->len;
        }
        memcpy(s->buf + s->len, evt->data, n);
        s->len += n;
    }
    return ESP_OK;
}

/* Fetch one tile (reusing the keep-alive connection) and blit it into dst
 * at (ox, oy). Returns false on failure. */
static bool blit_tile(esp_http_client_handle_t client, tile_sink_t *sink,
                      uint16_t *dst, int dst_w, int dst_h,
                      int z, int tx, int ty, int ox, int oy)
{
    char url[96];
    snprintf(url, sizeof(url),
             "https://basemaps.cartocdn.com/dark_all/%d/%d/%d.png", z, tx, ty);
    sink->len = 0;
    esp_http_client_set_url(client, url);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (err != ESP_OK || status != 200 || sink->len < 8) {
        ESP_LOGW(TAG, "tile %d/%d/%d: %s http=%d len=%u",
                 z, tx, ty, esp_err_to_name(err), status, (unsigned)sink->len);
        return false;
    }
    const uint8_t *fetch_buf = sink->buf;
    size_t len = sink->len;

    unsigned char *rgba = NULL;
    unsigned w = 0, h = 0;
    if (lodepng_decode32(&rgba, &w, &h, fetch_buf, len) != 0 || rgba == NULL) {
        return false;
    }

    for (int y = 0; y < (int)h; y++) {
        int dy = oy + y;
        if (dy < 0 || dy >= dst_h) {
            continue;
        }
        const unsigned char *src = rgba + (size_t)y * w * 4;
        for (int x = 0; x < (int)w; x++) {
            int dx = ox + x;
            if (dx < 0 || dx >= dst_w) {
                continue;
            }
            const unsigned char *p = src + x * 4;
            dst[(size_t)dy * dst_w + dx] =
                ((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3);
        }
    }
    free(rgba);
    return true;
}

bool tilemap_render(uint16_t *dst, int dst_w, int dst_h,
                    double lat_min, double lat_max,
                    double lon_min, double lon_max,
                    tile_view_t *out_view)
{
    double nx0, ny0, nx1, ny1;
    merc_norm(lat_max, lon_min, &nx0, &ny0);   /* top-left */
    merc_norm(lat_min, lon_max, &nx1, &ny1);   /* bottom-right */
    if (nx1 <= nx0) {
        nx1 = nx0 + 1e-6;
    }
    if (ny1 <= ny0) {
        ny1 = ny0 + 1e-6;
    }

    /* Highest zoom at which the bbox still fits the destination */
    int z = MAX_ZOOM;
    while (z > 0) {
        double world = (double)TILE_PX * (1 << z);
        if ((nx1 - nx0) * world <= dst_w && (ny1 - ny0) * world <= dst_h) {
            break;
        }
        z--;
    }

    double world = (double)TILE_PX * (1 << z);
    double cx = (nx0 + nx1) / 2.0 * world;
    double cy = (ny0 + ny1) / 2.0 * world;
    double px0 = cx - dst_w / 2.0;
    double py0 = cy - dst_h / 2.0;
    if (py0 < 0) {
        py0 = 0;
    }
    if (py0 + dst_h > world) {
        py0 = world - dst_h;
    }
    if (px0 < 0) {
        px0 = 0;
    }
    if (px0 + dst_w > world) {
        px0 = world > dst_w ? world - dst_w : 0;
    }

    /* dark background for any gaps */
    for (size_t i = 0; i < (size_t)dst_w * dst_h; i++) {
        dst[i] = 0x10A2;    /* ~#101418 */
    }

    tile_sink_t sink = {
        .buf = heap_caps_malloc(TILE_BUF, MALLOC_CAP_SPIRAM),
        .cap = TILE_BUF,
        .len = 0,
    };
    if (sink.buf == NULL) {
        return false;
    }
    esp_http_client_config_t cfg = {
        .url = "https://basemaps.cartocdn.com/dark_all/0/0/0.png",
        .event_handler = tile_http_cb,
        .user_data = &sink,
        .timeout_ms = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .user_agent = "esp32flight/1.0",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        free(sink.buf);
        return false;
    }

    int tx0 = (int)floor(px0 / TILE_PX);
    int ty0 = (int)floor(py0 / TILE_PX);
    int tx1 = (int)floor((px0 + dst_w - 1) / TILE_PX);
    int ty1 = (int)floor((py0 + dst_h - 1) / TILE_PX);
    int tiles_max = 1 << z;
    int ok = 0, total = 0;
    bool offline = false;
    struct { int16_t tx, ty; } failed[32];
    int failed_n = 0;
    for (int ty = ty0; ty <= ty1 && !offline; ty++) {
        if (ty < 0 || ty >= tiles_max) {
            continue;
        }
        for (int tx = tx0; tx <= tx1; tx++) {
            if (tx < 0 || tx >= tiles_max) {
                continue;
            }
            total++;
            if (blit_tile(client, &sink, dst, dst_w, dst_h, z, tx, ty,
                          (int)(tx * TILE_PX - px0), (int)(ty * TILE_PX - py0))) {
                ok++;
            } else if (failed_n < 32) {
                failed[failed_n].tx = tx;
                failed[failed_n].ty = ty;
                failed_n++;
            }
            /* many straight failures with zero successes: likely offline,
             * stop the first pass but still give the retry pass a chance */
            if (total >= 5 && ok == 0) {
                offline = true;
                break;
            }
        }
    }
    /* one retry pass: early requests occasionally fail while the TLS
     * connection warms up */
    for (int i = 0; i < failed_n; i++) {
        if (blit_tile(client, &sink, dst, dst_w, dst_h, z, failed[i].tx, failed[i].ty,
                      (int)(failed[i].tx * TILE_PX - px0),
                      (int)(failed[i].ty * TILE_PX - py0))) {
            ok++;
        }
    }
    esp_http_client_cleanup(client);
    free(sink.buf);

    ESP_LOGI(TAG, "z%d: %d/%d tiles", z, ok, total);
    if (ok == 0) {
        return false;
    }
    out_view->z = z;
    out_view->px0 = px0;
    out_view->py0 = py0;
    out_view->w = dst_w;
    out_view->h = dst_h;
    return true;
}
