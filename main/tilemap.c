#include "tilemap.h"
#include "settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "geo_math.h"

#include "extra/libs/png/lodepng.h"
#include "rainviewer.h"
#include "settings.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "tilemap";

/* Only one tile render at a time: concurrent renders (radar + ambient +
 * full map) each open a TLS session and the combined memory peak starves
 * mbedTLS into alloc failures. */
static SemaphoreHandle_t s_render_mux;

static QueueHandle_t s_job_q;

static void tile_worker(void *arg)
{
    tile_job_fn job;
    for (;;) {
        if (xQueueReceive(s_job_q, &job, portMAX_DELAY) == pdTRUE && job != NULL) {
            job();
        }
    }
}

bool tilemap_worker_submit(tile_job_fn job)
{
    if (s_job_q == NULL) {
        return false;
    }
    return xQueueSend(s_job_q, &job, 0) == pdTRUE;
}

void tilemap_init(void)
{
    s_render_mux = xSemaphoreCreateMutex();
    s_job_q = xQueueCreate(8, sizeof(tile_job_fn));
    if (s_job_q != NULL &&
        xTaskCreatePinnedToCore(tile_worker, "tile_worker", 10240,
                                NULL, 3, NULL, 0) != pdPASS) {
        vQueueDelete(s_job_q);
        s_job_q = NULL;
        ESP_LOGE(TAG, "tile worker create FAILED");
    }
}

#define TILE_PX     256
#define MAX_ZOOM    11
#define TILE_BUF    (96 * 1024)

/* LRU cache of compressed tile PNGs in PSRAM: repeat map opens render
 * instantly and survive short network outages. Guarded by s_render_mux
 * (all access happens inside a render). */
#ifdef CONFIG_IDF_TARGET_ESP32P4
#define TCACHE_N       256
#define TCACHE_BUDGET  (4 * 1024 * 1024)
#else
#define TCACHE_N       128
#define TCACHE_BUDGET  (768 * 1024)   /* PSRAM got crowded; transient buffers need headroom */
#endif

typedef struct {
    uint32_t key;           /* (z << 26) | (tx << 13) | ty */
    uint8_t *data;
    uint32_t len;
    uint32_t used_at;
} tcache_entry_t;

static tcache_entry_t s_tc[TCACHE_N];
static size_t s_tc_bytes;
static uint32_t s_tc_tick;

static void tcache_drop_key(uint32_t key)
{
    for (int i = 0; i < TCACHE_N; i++) {
        if (s_tc[i].data != NULL && s_tc[i].key == key) {
            free(s_tc[i].data);
            s_tc_bytes -= s_tc[i].len;
            s_tc[i].data = NULL;
            s_tc[i].len = 0;
            return;
        }
    }
}

static uint8_t *tcache_get(uint32_t key, uint32_t *len)
{
    for (int i = 0; i < TCACHE_N; i++) {
        if (s_tc[i].data != NULL && s_tc[i].key == key) {
            s_tc[i].used_at = ++s_tc_tick;
            *len = s_tc[i].len;
            return s_tc[i].data;
        }
    }
    return NULL;
}

static void tcache_drop(int i)
{
    free(s_tc[i].data);
    s_tc_bytes -= s_tc[i].len;
    s_tc[i].data = NULL;
    s_tc[i].len = 0;
}

/* Emergency eviction on decode OOM: frees every cached tile except the
 * one being decoded (tcache_get hands out the internal pointer, freeing
 * it mid-decode would be use-after-free). */
static void tcache_drop_all_except(uint32_t key)
{
    for (int i = 0; i < TCACHE_N; i++) {
        if (s_tc[i].data != NULL && s_tc[i].key != key) {
            tcache_drop(i);
        }
    }
}

static void tcache_put(uint32_t key, const uint8_t *data, uint32_t len)
{
    if (len == 0 || len > TCACHE_BUDGET / 8) {
        return;
    }
    /* The RAM cache yields to everything else: on big panels the pinned
     * framebuffers leave little headroom, and a full cache once starved
     * the aircraft fetch buffer and TLS on the 7B. The flash tile cache
     * makes RAM misses cheap (one SPIFFS read), so shrinking here is
     * nearly free. */
    while (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 1200 * 1024) {
        int lru = -1;
        for (int i = 0; i < TCACHE_N; i++) {
            if (s_tc[i].data != NULL &&
                (lru < 0 || s_tc[i].used_at < s_tc[lru].used_at)) {
                lru = i;
            }
        }
        if (lru < 0) {
            return;   /* cache empty and PSRAM still tight: skip caching */
        }
        tcache_drop(lru);
    }
    while (s_tc_bytes + len > TCACHE_BUDGET) {
        int lru = -1;
        for (int i = 0; i < TCACHE_N; i++) {
            if (s_tc[i].data != NULL &&
                (lru < 0 || s_tc[i].used_at < s_tc[lru].used_at)) {
                lru = i;
            }
        }
        if (lru < 0) {
            return;
        }
        tcache_drop(lru);
    }
    int slot = -1;
    for (int i = 0; i < TCACHE_N; i++) {
        if (s_tc[i].data == NULL) {
            slot = i;
            break;
        }
        if (slot < 0 || s_tc[i].used_at < s_tc[slot].used_at) {
            slot = i;
        }
    }
    if (s_tc[slot].data != NULL) {
        tcache_drop(slot);
    }
    uint8_t *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (copy == NULL) {
        return;
    }
    memcpy(copy, data, len);
    s_tc[slot].key = key;
    s_tc[slot].data = copy;
    s_tc[slot].len = len;
    s_tc[slot].used_at = ++s_tc_tick;
    s_tc_bytes += len;
}

/* Persistent tile cache on the assets partition: the home area is a
 * small, stable set (~20-40 tiles) and the OSM basemap barely changes,
 * so once fetched it survives restarts and renders offline. Guarded by a
 * free-space floor (the spotting logs and on-demand logos share the
 * partition) and wiped when the view center moves far away, so a
 * relocation cannot slowly fill the flash with stale areas. */
#define FTILE_DIR        "/assets/tiles"
#define FTILE_MIN_FREE   (700 * 1024)

static bool s_ftile_dir_ok;
static bool s_ftile_disabled;

static void ftile_path(char *out, size_t n, uint32_t key)
{
    snprintf(out, n, FTILE_DIR "/%08lx.png", (unsigned long)key);
}

static uint8_t *ftile_get(uint32_t key, size_t *len)
{
    char path[48];
    ftile_path(path, sizeof(path), key);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = NULL;
    if (sz > 8 && sz <= TILE_BUF) {
        buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
        if (buf != NULL && fread(buf, 1, sz, f) != (size_t)sz) {
            free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    if (buf != NULL) {
        *len = (size_t)sz;
    }
    return buf;
}

static void ftile_put(uint32_t key, const uint8_t *data, size_t len)
{
    if (s_ftile_disabled) {
        return;
    }
    size_t total = 0, used = 0;
    /* SPIFFS GC stalls CPU0 for seconds and fails writes once occupancy
     * climbs past ~75% on a crowded partition. Skip flash caching and rely
     * on the PSRAM cache when headroom is tight. */
    if (esp_spiffs_info("assets", &total, &used) != ESP_OK ||
        total - used < FTILE_MIN_FREE + len ||
        used > (total * 3) / 4) {
        ESP_LOGI(TAG, "flash tile cache paused (assets %u/%u KB used); using RAM cache",
                 (unsigned)(used / 1024), (unsigned)(total / 1024));
        s_ftile_disabled = true;
        return;
    }
    if (!s_ftile_dir_ok) {
        mkdir(FTILE_DIR, 0775);
        s_ftile_dir_ok = true;
    }
    char path[48];
    ftile_path(path, sizeof(path), key);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGW(TAG, "ftile open-for-write failed: %s (disabling flash tile cache)", path);
        s_ftile_disabled = true;
        return;
    }
    {
        size_t wr = fwrite(data, 1, len, f);
        fclose(f);
        if (wr != len) {
            ESP_LOGW(TAG, "ftile short write %u/%u: %s (disabling flash tile cache)",
                     (unsigned)wr, (unsigned)len, path);
            /* short write (full/failing FS): a truncated PNG poisons the
             * cache - every later render would hit it, fail the decode
             * and, before the retry logic below existed, never recover */
            unlink(path);
            s_ftile_disabled = true;
        }
    }
}

static void ftile_unlink(uint32_t key)
{
    char path[48];
    ftile_path(path, sizeof(path), key);
    unlink(path);
}

/* Wipe the flash tiles when the render center moves far from the stored
 * one (the device moved home): stale areas would otherwise linger. */
/* One-time wipe when the basemap source changes: devices hold the old
 * provider's tiles (including CARTO's watermarked ones, #31) on flash
 * and would serve them forever. The marker file names the source. */
#define FTILE_SRC_MARKER  FTILE_DIR "/src2"

static void ftile_wipe_all(void)
{
    s_ftile_disabled = false;
    DIR *d = opendir(FTILE_DIR);
    if (d != NULL) {
        struct dirent *e;
        char path[320];
        while ((e = readdir(d)) != NULL) {
            snprintf(path, sizeof(path), FTILE_DIR "/%s", e->d_name);
            unlink(path);
        }
        closedir(d);
    }
}

static void tile_source_url(char *out, size_t n, int z, int tx, int ty);

static uint32_t tile_source_sig(void)
{
    char base[192];
    tile_source_url(base, sizeof(base), 0, 0, 0);
    uint32_t h = 2166136261u;
    for (const char *p = base; *p != '\0'; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    /* the OSM path also restyles per map_light at decode time */
    return h ^ (settings_get()->map_light ? 1u : 0u);
}

static void ftile_check_source(void)
{
    static bool checked;
    if (checked) {
        return;
    }
    checked = true;
    unsigned long old_sig = 0;
    uint32_t sig = tile_source_sig();
    FILE *f = fopen(FTILE_SRC_MARKER, "rb");
    if (f != NULL) {
        int got = fscanf(f, "%lx", &old_sig);
        fclose(f);
        if (got == 1 && (uint32_t)old_sig == sig) {
            return;
        }
    }
    ftile_wipe_all();
    ESP_LOGI(TAG, "basemap source changed: flash tile cache wiped");
    mkdir(FTILE_DIR, 0775);
    f = fopen(FTILE_SRC_MARKER, "wb");
    if (f != NULL) {
        fprintf(f, "%lx", (unsigned long)sig);
        fclose(f);
    }
}

static void ftile_check_center(double lat, double lon)
{
    static bool checked;
    double clat = 0, clon = 0;
    FILE *f;
    if (!checked) {
        checked = true;
        f = fopen(FTILE_DIR "/center", "rb");
        if (f != NULL) {
            if (fscanf(f, "%lf %lf", &clat, &clon) == 2 &&
                geo_haversine_km(clat, clon, lat, lon) > 150.0) {
                fclose(f);
                ftile_wipe_all();
                FILE *mf = fopen(FTILE_SRC_MARKER, "wb");
                if (mf != NULL) { fprintf(mf, "%lx", (unsigned long)tile_source_sig()); fclose(mf); }
                ESP_LOGI(TAG, "flash tiles wiped (moved %d km)",
                         (int)geo_haversine_km(clat, clon, lat, lon));
            } else {
                fclose(f);
                return;   /* same area: keep everything */
            }
        }
        f = fopen(FTILE_DIR "/center", "wb");
        if (f == NULL) {
            mkdir(FTILE_DIR, 0775);
            f = fopen(FTILE_DIR "/center", "wb");
        }
        if (f != NULL) {
            fprintf(f, "%.4f %.4f", lat, lon);
            fclose(f);
        }
    }
}

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
    /* views that cross the antimeridian have px0 outside [0, world);
       pick the copy of the world that lands nearest the view center */
    double px = nx * world - v->px0;
    double center = v->w / 2.0;
    if (px - center > world / 2.0) {
        px -= world;
    } else if (center - px > world / 2.0) {
        px += world;
    }
    *x = (int)px;
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
/* Tile source selection (#31 follow-up): a custom {z}/{x}/{y} template
 * wins, then CARTO with the user's own key (native dark_all/light_all),
 * else OSM standard tiles restyled on the device. */
static bool tile_source_is_osm(void)
{
    const settings_t *c = settings_get();
    return c->tile_url[0] == '\0' && c->carto_key[0] == '\0';
}

static void tile_source_url(char *out, size_t n, int z, int tx, int ty)
{
    const settings_t *c = settings_get();
    if (c->tile_url[0] != '\0') {
        size_t o = 0;
        for (const char *p = c->tile_url; *p != '\0' && o + 12 < n; p++) {
            if (p[0] == '{' && p[2] == '}') {
                int v = p[1] == 'z' ? z : p[1] == 'x' ? tx : p[1] == 'y' ? ty : -1;
                if (v >= 0) {
                    o += snprintf(out + o, n - o, "%d", v);
                    p += 2;
                    continue;
                }
            }
            out[o++] = *p;
        }
        out[o] = '\0';
        return;
    }
    if (c->carto_key[0] != '\0') {
        snprintf(out, n, "https://basemaps.cartocdn.com/%s/%d/%d/%d.png?key=%s",
                 c->map_light ? "light_all" : "dark_all", z, tx, ty, c->carto_key);
        return;
    }
    snprintf(out, n, "https://tile.openstreetmap.org/%d/%d/%d.png", z, tx, ty);
}

const char *tilemap_source_credit(void)
{
    const settings_t *c = settings_get();
    if (c->tile_url[0] != '\0') {
        return "\xC2\xA9 map data providers";
    }
    if (c->carto_key[0] != '\0') {
        return "\xC2\xA9 OSM \xC2\xB7 \xC2\xA9 CARTO";
    }
    return "\xC2\xA9 OpenStreetMap contributors";
}

static bool blit_tile(esp_http_client_handle_t client, tile_sink_t *sink,
                      uint16_t *dst, int dst_w, int dst_h,
                      int z, int tx, int ty, int ox, int oy)
{
    /* One tick between tiles keeps IDLE0 fed: OSM tiles decode 3-10x
     * longer than the old CARTO ones, and a full-screen render used to
     * starve the task watchdog mid-pass (visible shake, and a crash on
     * the 7B where the pass is longest). */
    vTaskDelay(1);
    uint32_t key = ((uint32_t)z << 26) | ((uint32_t)tx << 13) | (uint32_t)ty;
    for (int attempt = 0; attempt < 2; attempt++) {
    uint32_t clen = 0;
    const uint8_t *fetch_buf = NULL;
    size_t len = 0;
    bool from_cache = false;
    uint8_t *flash_buf = NULL;

    if (attempt == 0) {
        fetch_buf = tcache_get(key, &clen);
        len = clen;
        from_cache = fetch_buf != NULL;
        if (!from_cache) {
            /* persisted on the assets partition? renders offline forever */
            flash_buf = ftile_get(key, &len);
            if (flash_buf != NULL) {
                tcache_put(key, flash_buf, len);
                fetch_buf = flash_buf;
                from_cache = true;
            }
        }
    }

    if (!from_cache) {
        char url[192];
        tile_source_url(url, sizeof(url), z, tx, ty);
        sink->len = 0;
        esp_http_client_set_url(client, url);
        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        if (err != ESP_OK || status != 200 || sink->len < 8) {
            ESP_LOGW(TAG, "tile %d/%d/%d: %s http=%d len=%u",
                     z, tx, ty, esp_err_to_name(err), status, (unsigned)sink->len);
            /* after a TLS-level error the session context may be gone; reusing
             * the connection crashes inside mbedtls_ssl_write. Force a clean
             * reconnect for the next tile. */
            esp_http_client_close(client);
            return false;
        }
        tcache_put(key, sink->buf, sink->len);
        ftile_put(key, sink->buf, sink->len);
        fetch_buf = sink->buf;
        len = sink->len;
    }

    /* Decode in the PNG's native format: OSM basemap tiles are almost
     * always 8-bit palette, which decodes into 64 KB instead of 256 KB
     * RGBA and skips the conversion pass entirely - the RGBA blow-up is
     * what fragmented PSRAM on the route-map view (decode OOM with a
     * ~320 KB largest block) and made tiles go missing. */
    unsigned char *rgba = NULL;
    unsigned w = 0, h = 0;
    LodePNGState pngst;
    lodepng_state_init(&pngst);
    pngst.decoder.color_convert = 0;
    unsigned lret = lodepng_decode(&rgba, &w, &h, &pngst, fetch_buf, len);
    int bpp = 4;
    const unsigned char *pal = NULL;
    size_t paln = 0;
    if (lret == 0) {
        const LodePNGColorMode *cm = &pngst.info_png.color;
        if (cm->colortype == LCT_PALETTE && cm->bitdepth == 8) {
            bpp = 1;
            pal = cm->palette;
            paln = cm->palettesize;
        } else if (cm->colortype == LCT_RGB && cm->bitdepth == 8) {
            bpp = 3;
        } else if (cm->colortype == LCT_RGBA && cm->bitdepth == 8) {
            bpp = 4;
        } else {
            /* odd format (16-bit, sub-byte palette, grey): take the
             * classic converted path, those tiles are rare and small */
            free(rgba);
            rgba = NULL;
            lodepng_state_cleanup(&pngst);
            lodepng_state_init(&pngst);
            lret = lodepng_decode32(&rgba, &w, &h, fetch_buf, len);
            bpp = 4;
        }
    }
    if (lret == 83) {
        /* lodepng alloc failure: PSRAM too fragmented for the 256 KB RGBA
         * buffer (big view buffers eat the headroom on the route map).
         * NOT corruption - purging flash and refetching from the network
         * changes nothing and used to spin a repaint loop that starved
         * IDLE0. Dump the RAM tile cache instead and retry once. */
        ESP_LOGW(TAG, "tile %d/%d/%d decode OOM, spiram free %u largest %u: evicting RAM cache",
                 z, tx, ty, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        free(rgba);
        rgba = NULL;
        w = h = 0;
        tcache_drop_all_except(key);
        lodepng_state_cleanup(&pngst);
        lodepng_state_init(&pngst);
        pngst.decoder.color_convert = 0;
        lret = lodepng_decode(&rgba, &w, &h, &pngst, fetch_buf, len);
        const LodePNGColorMode *cm2 = &pngst.info_png.color;
        if (lret == 0 && cm2->colortype == LCT_PALETTE && cm2->bitdepth == 8) {
            bpp = 1;
            pal = cm2->palette;
            paln = cm2->palettesize;
        } else if (lret == 0 && cm2->colortype == LCT_RGB && cm2->bitdepth == 8) {
            bpp = 3;
        } else if (lret == 0 && cm2->colortype == LCT_RGBA && cm2->bitdepth == 8) {
            bpp = 4;
        } else if (lret == 0) {
            lret = 82;   /* unusual format under OOM: give up this pass */
        }
        if (lret != 0 || rgba == NULL) {
            /* still no room: let this render pass go without the tile,
             * the periodic retry picks it up once buffers settle */
            free(rgba);
            free(flash_buf);
            lodepng_state_cleanup(&pngst);
            return false;
        }
    } else if (lret != 0 || rgba == NULL) {
        ESP_LOGW(TAG, "tile %d/%d/%d decode err=%u, free spiram %u", z, tx, ty,
                 lret, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        free(rgba);   /* lodepng may allocate even when it reports an error */
        free(flash_buf);
        lodepng_state_cleanup(&pngst);
        if (!from_cache) {
            return false;   /* fresh network data undecodable: give up */
        }
        /* poisoned cache (e.g. a tile truncated by a short flash write
         * during memory pressure): purge both layers, refetch */
        ESP_LOGW(TAG, "tile %d/%d/%d: cached copy undecodable, refetching", z, tx, ty);
        tcache_drop_key(key);
        ftile_unlink(key);
        continue;
    }

    const bool light = settings_get()->map_light;
    /* hoisted: calling tile_source_is_osm() per pixel multiplied the
     * per-tile cost on every render (0.4.6 regression) */
    const bool darken = !light && tile_source_is_osm();
    /* clip the x-range once per tile instead of testing every pixel */
    int x0 = ox < 0 ? -ox : 0;
    int x1 = ox + (int)w > dst_w ? dst_w - ox : (int)w;
    for (int y = 0; y < (int)h; y++) {
        int dy = oy + y;
        if (dy < 0 || dy >= dst_h) {
            continue;
        }
        const unsigned char *src = rgba + (size_t)y * w * bpp;
        for (int x = x0; x < x1; x++) {
            int r, g, b;
            if (bpp == 1) {
                unsigned idx = src[x];
                const unsigned char *p = idx < paln ? pal + idx * 4 : pal;
                r = p[0]; g = p[1]; b = p[2];
            } else {
                const unsigned char *p = src + x * bpp;
                r = p[0]; g = p[1]; b = p[2];
            }
            if (darken) {
                /* OSM serves a light map; the dark style is made here:
                 * luminance inverted into a 16..128 band with a slight
                 * blue cast (land goes near-black, labels and borders
                 * come out bright, water sits a notch above land) -
                 * CARTO's dark_all became watermarked (#31), so the
                 * dark basemap is now our own pixel transform. */
                int lum = (r * 77 + g * 150 + b * 29) >> 8;
                int v = 16 + (((255 - lum) * 112) >> 8);
                r = v - (v >> 4);
                g = v;
                b = v + (v >> 3) > 255 ? 255 : v + (v >> 3);
            }
            dst[(size_t)dy * dst_w + (ox + x)] =
                ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        }
    }
    free(rgba);
    free(flash_buf);
    lodepng_state_cleanup(&pngst);
    return true;
    }
    return false;
}

/* Fetch one RainViewer tile and alpha-blend it over dst. Failures are
 * non-fatal: rain is a garnish, not the map. */
/* RainViewer serves real data only up to this zoom; above it the API
 * returns a "Zoom Level Not Supported" placeholder tile (verified
 * empirically: z8+ responses are a constant 1.4 KB plate). For closer
 * views we fetch the z7 parent and magnify the right quadrant. */
#define RAIN_MAX_Z 7

static void blit_rain_tile(esp_http_client_handle_t client, tile_sink_t *sink,
                           const char *frame, uint16_t *dst, int dst_w, int dst_h,
                           int z, int tx, int ty, int ox, int oy)
{
    vTaskDelay(1);   /* same watchdog courtesy as blit_tile */
    int shift = z > RAIN_MAX_Z ? z - RAIN_MAX_Z : 0;
    int zc = z - shift;
    int ptx = tx >> shift;
    int pty = ty >> shift;
    /* this tile's quadrant inside the parent tile, in parent pixels */
    int sub_x = (tx - (ptx << shift)) * (TILE_PX >> shift);
    int sub_y = (ty - (pty << shift)) * (TILE_PX >> shift);

    uint32_t key = 0x80000000u |
                   (((uint32_t)rainviewer_generation() & 0xF) << 27) |
                   (((uint32_t)zc & 0xF) << 23) |
                   (((uint32_t)ptx & 0x7FF) << 12) | ((uint32_t)pty & 0xFFF);
    uint32_t clen = 0;
    const uint8_t *fetch_buf = tcache_get(key, &clen);
    size_t len = clen;

    if (fetch_buf == NULL) {
        char url[224];
        snprintf(url, sizeof(url), "%s/256/%d/%d/%d/2/1_1.png", frame, zc, ptx, pty);
        sink->len = 0;
        esp_http_client_set_url(client, url);
        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        if (err != ESP_OK || status != 200 || sink->len < 8) {
            esp_http_client_close(client);
            return;
        }
        tcache_put(key, sink->buf, sink->len);
        fetch_buf = sink->buf;
        len = sink->len;
    }

    unsigned char *rgba = NULL;
    unsigned w = 0, h = 0;
    if (lodepng_decode32(&rgba, &w, &h, fetch_buf, len) != 0 || rgba == NULL) {
        free(rgba);   /* lodepng may allocate even when it reports an error */
        return;
    }
    for (int y = 0; y < TILE_PX; y++) {
        int dy = oy + y;
        if (dy < 0 || dy >= dst_h) {
            continue;
        }
        /* source row in parent pixels, 8.8 fixed point for bilinear */
        int fy = (sub_y << 8) + ((y << 8) >> shift);
        int sy = fy >> 8;
        if (sy >= (int)h) {
            continue;
        }
        int wy = fy & 0xFF;
        int sy1 = sy + 1 < (int)h ? sy + 1 : sy;
        const unsigned char *row0 = rgba + (size_t)sy * w * 4;
        const unsigned char *row1 = rgba + (size_t)sy1 * w * 4;
        for (int x = 0; x < TILE_PX; x++) {
            int dx = ox + x;
            if (dx < 0 || dx >= dst_w) {
                continue;
            }
            int fx = (sub_x << 8) + ((x << 8) >> shift);
            int sx = fx >> 8;
            if (sx >= (int)w) {
                continue;
            }
            int a, cr, cg, cb;
            if (shift == 0) {
                const unsigned char *pc = row0 + sx * 4;
                a = pc[3];
                cr = pc[0];
                cg = pc[1];
                cb = pc[2];
            } else {
                /* bilinear with premultiplied color so transparent
                   neighbours do not darken the echo edges */
                int wx = fx & 0xFF;
                int sx1 = sx + 1 < (int)w ? sx + 1 : sx;
                const unsigned char *p00 = row0 + sx * 4;
                const unsigned char *p01 = row0 + sx1 * 4;
                const unsigned char *p10 = row1 + sx * 4;
                const unsigned char *p11 = row1 + sx1 * 4;
                int w00 = (256 - wx) * (256 - wy);
                int w01 = wx * (256 - wy);
                int w10 = (256 - wx) * wy;
                int w11 = wx * wy;
                a = (p00[3] * w00 + p01[3] * w01 + p10[3] * w10 + p11[3] * w11) >> 16;
                if (a > 0) {
                    cr = (p00[0] * p00[3] * (w00 >> 4) + p01[0] * p01[3] * (w01 >> 4) +
                          p10[0] * p10[3] * (w10 >> 4) + p11[0] * p11[3] * (w11 >> 4)) /
                         ((a << 12) + 1);
                    cg = (p00[1] * p00[3] * (w00 >> 4) + p01[1] * p01[3] * (w01 >> 4) +
                          p10[1] * p10[3] * (w10 >> 4) + p11[1] * p11[3] * (w11 >> 4)) /
                         ((a << 12) + 1);
                    cb = (p00[2] * p00[3] * (w00 >> 4) + p01[2] * p01[3] * (w01 >> 4) +
                          p10[2] * p10[3] * (w10 >> 4) + p11[2] * p11[3] * (w11 >> 4)) /
                         ((a << 12) + 1);
                    if (cr > 255) cr = 255;
                    if (cg > 255) cg = 255;
                    if (cb > 255) cb = 255;
                } else {
                    cr = cg = cb = 0;
                }
            }
            a = a * 3 / 4;      /* keep the basemap readable */
            if (a < 10) {
                continue;
            }
            uint16_t bg = dst[(size_t)dy * dst_w + dx];
            int br = (bg >> 11) << 3, bgc = ((bg >> 5) & 0x3F) << 2, bb = (bg & 0x1F) << 3;
            int r = (cr * a + br * (255 - a)) / 255;
            int g = (cg * a + bgc * (255 - a)) / 255;
            int b = (cb * a + bb * (255 - a)) / 255;
            dst[(size_t)dy * dst_w + dx] =
                ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        }
    }
    free(rgba);
}

#define TM_LAYER_BASE 1
#define TM_LAYER_RAIN 2

static bool render_impl(uint16_t *dst, int dst_w, int dst_h,
                        double lat_min, double lat_max,
                        double lon_min, double lon_max,
                        tile_view_t *out_view, int layers)
{
    if (layers & TM_LAYER_BASE) {
        ftile_check_source();
    ftile_check_center((lat_min + lat_max) / 2.0, (lon_min + lon_max) / 2.0);
    }
    /* Re-render of the same area into a live canvas: skip the background
     * flood so the old frame morphs into the new one as tiles land,
     * instead of flashing dark mid-retry. */
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
    /* no X clamp: longitude wraps, and antimeridian views need px0
       outside [0, world); the tile loop wraps tile indexes instead */

    /* dark background for any gaps (pure black in rain-only mode) */
    uint16_t bgcol = (layers & TM_LAYER_BASE) ? 0x10A2 : 0x0000;
    for (size_t i = 0; i < (size_t)dst_w * dst_h; i++) {
        dst[i] = bgcol;
    }

    /* Persistent: renders serialize on s_render_mux, and a per-render
     * malloc used to fail under PSRAM pressure - which silenced the
     * screensaver exactly when memory was busiest. */
    static uint8_t *s_sink_buf;
    if (s_sink_buf == NULL) {
        s_sink_buf = heap_caps_malloc(TILE_BUF, MALLOC_CAP_SPIRAM);
    }
    tile_sink_t sink = {
        .buf = s_sink_buf,
        .cap = TILE_BUF,
        .len = 0,
    };
    if (sink.buf == NULL) {
        return false;
    }
    esp_http_client_config_t cfg = {
        .url = "https://tile.openstreetmap.org/0/0/0.png",
        .event_handler = tile_http_cb,
        .user_data = &sink,
        .timeout_ms = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .user_agent = "esp32flight/0.4 (+https://github.com/theqkash/esp32flight)",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return false;
    }

    int tx0 = (int)floor(px0 / TILE_PX);
    int ty0 = (int)floor(py0 / TILE_PX);
    int tx1 = (int)floor((px0 + dst_w - 1) / TILE_PX);
    int ty1 = (int)floor((py0 + dst_h - 1) / TILE_PX);
    int tiles_max = 1 << z;
    int ok = 0, total = 0;
    bool offline = false;
    struct { int16_t tx, txw, ty; } failed[32];
    int failed_n = 0;
    if (!(layers & TM_LAYER_BASE)) {
        ok = 1;         /* rain-only: nothing mandatory below */
        total = 1;
        goto rain_pass;
    }
    for (int ty = ty0; ty <= ty1 && !offline; ty++) {
        if (ty < 0 || ty >= tiles_max) {
            continue;
        }
        for (int tx = tx0; tx <= tx1; tx++) {
            /* wrap around the antimeridian instead of skipping */
            int txw = ((tx % tiles_max) + tiles_max) % tiles_max;
            total++;
            if (blit_tile(client, &sink, dst, dst_w, dst_h, z, txw, ty,
                          (int)(tx * TILE_PX - px0), (int)(ty * TILE_PX - py0))) {
                ok++;
            } else if (failed_n < 32) {
                failed[failed_n].tx = tx;
                failed[failed_n].txw = txw;
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
        if (blit_tile(client, &sink, dst, dst_w, dst_h, z, failed[i].txw, failed[i].ty,
                      (int)(failed[i].tx * TILE_PX - px0),
                      (int)(failed[i].ty * TILE_PX - py0))) {
            ok++;
        }
    }
rain_pass:
    /* optional precipitation layer on top (RainViewer, same tile grid) */
    if ((layers & TM_LAYER_RAIN) && ok > 0) {
        const char *frame = rainviewer_frame_path();
        if (frame[0] != '\0') {
            for (int ty = ty0; ty <= ty1; ty++) {
                if (ty < 0 || ty >= tiles_max) {
                    continue;
                }
                for (int tx = tx0; tx <= tx1; tx++) {
                    int txw = ((tx % tiles_max) + tiles_max) % tiles_max;
                    blit_rain_tile(client, &sink, frame, dst, dst_w, dst_h, z, txw, ty,
                                   (int)(tx * TILE_PX - px0), (int)(ty * TILE_PX - py0));
                }
            }
        }
    }

    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "z%d: %d/%d tiles (dst %dx%d, bbox %.3f..%.3f / %.3f..%.3f)",
             z, ok, total, dst_w, dst_h, lat_min, lat_max, lon_min, lon_max);
    if (ok == 0) {
        return false;
    }
    out_view->z = z;
    out_view->px0 = px0;
    out_view->py0 = py0;
    out_view->w = dst_w;
    out_view->h = dst_h;
    out_view->missing = (layers & TM_LAYER_BASE) ? total - ok : 0;
    return true;
}

bool tilemap_render(uint16_t *dst, int dst_w, int dst_h,
                    double lat_min, double lat_max,
                    double lon_min, double lon_max,
                    tile_view_t *out_view)
{
    if (s_render_mux != NULL) {
        xSemaphoreTake(s_render_mux, portMAX_DELAY);
    }
    int layers = TM_LAYER_BASE;
    if (settings_get()->rain_overlay) {
        layers |= TM_LAYER_RAIN;
    }
    bool ok = render_impl(dst, dst_w, dst_h,
                          lat_min, lat_max, lon_min, lon_max, out_view, layers);
    if (s_render_mux != NULL) {
        xSemaphoreGive(s_render_mux);
    }
    return ok;
}

bool tilemap_render_rain(uint16_t *dst, int dst_w, int dst_h,
                         double lat_min, double lat_max,
                         double lon_min, double lon_max,
                         tile_view_t *out_view)
{
    if (s_render_mux != NULL) {
        xSemaphoreTake(s_render_mux, portMAX_DELAY);
    }
    bool ok = render_impl(dst, dst_w, dst_h,
                          lat_min, lat_max, lon_min, lon_max, out_view,
                          TM_LAYER_RAIN);
    if (s_render_mux != NULL) {
        xSemaphoreGive(s_render_mux);
    }
    return ok;
}
