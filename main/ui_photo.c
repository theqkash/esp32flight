#include "ui_photo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#include "fonts.h"
#include "http_util.h"
#include "jpeg_dec.h"
#include "lang.h"
#include "lvgl_port.h"
#include "theme.h"

#define COL_BG     (app_theme()->bg)
#define COL_PANEL  (app_theme()->panel)
#define COL_ACCENT (app_theme()->accent)
#define COL_TEXT   (app_theme()->text)
#define COL_DIM    (app_theme()->dim)

#define PHOTO_MAX_BYTES (512 * 1024)

static lv_obj_t *s_overlay;
static lv_obj_t *s_img;
static lv_obj_t *s_msg;
static lv_obj_t *s_credit;
static uint8_t *s_photo_buf;
static lv_img_dsc_t s_photo_dsc;
static bool s_busy;

static void close_cb(lv_event_t *e)
{
    if (s_overlay != NULL) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
    }
    /* keep s_photo_buf until next fetch: the img widget is gone with overlay */
}

typedef struct {
    char hex[8];
} photo_req_t;

static const char *TAG = "photo";

/* Source 1: planespotters.net public API (no key). Their terms ask for
 * the photographer's credit next to the picture. */
static bool from_planespotters(const char *hex, char *url, size_t un,
                               char *credit, size_t cn, char *json, size_t jn)
{
    char api[96];
    snprintf(api, sizeof(api), "https://api.planespotters.net/pub/photos/hex/%s", hex);
    esp_err_t err = http_get_to_buffer(api, json, jn, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "planespotters %s: %s", hex, esp_err_to_name(err));
        return false;
    }
    bool ok = false;
    cJSON *root = cJSON_Parse(json);
    const cJSON *photos = root ? cJSON_GetObjectItem(root, "photos") : NULL;
    const cJSON *ph = cJSON_IsArray(photos) ? cJSON_GetArrayItem(photos, 0) : NULL;
    if (ph != NULL) {
        const cJSON *tl = cJSON_GetObjectItem(ph, "thumbnail_large");
        if (tl == NULL) {
            tl = cJSON_GetObjectItem(ph, "thumbnail");
        }
        const cJSON *src = tl ? cJSON_GetObjectItem(tl, "src") : NULL;
        const cJSON *who = cJSON_GetObjectItem(ph, "photographer");
        if (cJSON_IsString(src)) {
            strlcpy(url, src->valuestring, un);
            snprintf(credit, cn, "photo: %s / planespotters.net",
                     cJSON_IsString(who) ? who->valuestring : "unknown");
            ok = true;
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "planespotters %s: %s", hex, ok ? "photo found" : "no photo");
    return ok;
}

/* Source 2: adsbdb's aircraft record, whose photo links point at
 * airport-data.com (thin coverage). Full photo first, then thumbnail. */
static int from_adsbdb(const char *hex, char urls[2][256], char *json, size_t jn)
{
    char api[80];
    snprintf(api, sizeof(api), "https://api.adsbdb.com/v0/aircraft/%s", hex);
    esp_err_t err = http_get_to_buffer(api, json, jn, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "adsbdb %s: %s", hex, esp_err_to_name(err));
        return 0;
    }
    int n = 0;
    cJSON *root = cJSON_Parse(json);
    const cJSON *resp = root ? cJSON_GetObjectItem(root, "response") : NULL;
    const cJSON *acj = resp ? cJSON_GetObjectItem(resp, "aircraft") : NULL;
    const char *keys[2] = { "url_photo", "url_photo_thumbnail" };
    for (int i = 0; i < 2 && acj != NULL; i++) {
        const cJSON *ju = cJSON_GetObjectItem(acj, keys[i]);
        if (cJSON_IsString(ju) && ju->valuestring[0]) {
            strlcpy(urls[n++], ju->valuestring, 256);
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "adsbdb %s: %d photo link(s)", hex, n);
    return n;
}

static uint16_t *fetch_decode(const char *url, uint8_t *raw, int *w, int *h)
{
    size_t len = 0;
    esp_err_t err = http_get_to_buffer(url, (char *)raw, PHOTO_MAX_BYTES, &len);
    if (err != ESP_OK || len <= 4) {
        ESP_LOGW(TAG, "image fetch failed (%s): %s", esp_err_to_name(err), url);
        return NULL;
    }
    uint16_t *px = jpeg_decode_rgb565(raw, len, 700, 380, w, h);
    ESP_LOGI(TAG, "image %u B -> %s", (unsigned)len, px ? "decoded" : "decode failed");
    return px;
}

static void photo_task(void *arg)
{
    photo_req_t *req = arg;
    char credit[96] = "";
    uint16_t *pixels = NULL;
    int w = 0, h = 0;

    char *json = malloc(8192);
    uint8_t *raw = heap_caps_malloc(PHOTO_MAX_BYTES, MALLOC_CAP_SPIRAM);
    if (json != NULL && raw != NULL) {
        char url[256];
        if (from_planespotters(req->hex, url, sizeof(url), credit, sizeof(credit),
                               json, 8192)) {
            pixels = fetch_decode(url, raw, &w, &h);
        }
        if (pixels == NULL) {
            char urls[2][256];
            int n = from_adsbdb(req->hex, urls, json, 8192);
            for (int i = 0; i < n && pixels == NULL; i++) {
                pixels = fetch_decode(urls[i], raw, &w, &h);
            }
            if (pixels != NULL) {
                strlcpy(credit, "photo: airport-data.com", sizeof(credit));
            }
        }
    }
    free(json);
    free(raw);
    if (pixels == NULL) {
        ESP_LOGI(TAG, "%s: no photo from any source", req->hex);
    }

    if (lvgl_port_lock(-1)) {
        if (s_overlay != NULL) {
            if (pixels != NULL) {
                free(s_photo_buf);
                s_photo_buf = (uint8_t *)pixels;
                pixels = NULL;
                memset(&s_photo_dsc, 0, sizeof(s_photo_dsc));
                s_photo_dsc.header.always_zero = 0;
                s_photo_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
                s_photo_dsc.header.w = w;
                s_photo_dsc.header.h = h;
                s_photo_dsc.data = s_photo_buf;
                s_photo_dsc.data_size = (uint32_t)w * h * 2;
                lv_img_set_src(s_img, &s_photo_dsc);
                if (w <= 390 && h <= 200) {
                    lv_img_set_zoom(s_img, 512);   /* tiny thumbnail: 2x */
                }
                lv_obj_clear_flag(s_img, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(s_credit, credit);
            } else {
                lv_label_set_text(s_msg, L()->no_photo);
            }
        }
        lvgl_port_unlock();
    }
    free(pixels);
    free(req);
    s_busy = false;
    vTaskDelete(NULL);
}

void ui_photo_open(const char *hex, const char *callsign)
{
    if (s_overlay != NULL || s_busy) {
        return;
    }

    s_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(title, UIFONT(&lv_font_montserrat_24, &lv_font_montserrat_14), 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_label_set_text_fmt(title, "%s  -  %s", callsign, hex);
    lv_obj_set_pos(title, UISX(16), UISY(12));

    lv_obj_t *btn_close = lv_btn_create(s_overlay);
    lv_obj_set_size(btn_close, UISX(52), UISY(40));
    lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -UISX(12), UISY(8));
    lv_obj_set_style_bg_color(btn_close, COL_PANEL, 0);
    lv_obj_set_style_border_color(btn_close, COL_DIM, 0);
    lv_obj_set_style_border_width(btn_close, 1, 0);
    lv_obj_add_event_cb(btn_close, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(btn_close);
    lv_obj_set_style_text_font(xl, UIFONT(&lv_font_montserrat_16, &lv_font_montserrat_10), 0);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_center(xl);

    s_msg = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_msg, UIFONT(&font_pl_20, &font_pl_12), 0);
    lv_obj_set_style_text_color(s_msg, COL_DIM, 0);
    lv_label_set_text(s_msg, L()->loading_photo);
    lv_obj_center(s_msg);

    s_img = lv_img_create(s_overlay);
    lv_obj_center(s_img);
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);

    s_credit = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_credit, UIFONT(&font_pl_14, &font_pl_8), 0);
    lv_obj_set_style_text_color(s_credit, COL_DIM, 0);
    lv_label_set_text(s_credit, "");
    lv_obj_align(s_credit, LV_ALIGN_BOTTOM_LEFT, UISX(16), -UISY(8));

    photo_req_t *req = malloc(sizeof(photo_req_t));
    if (req != NULL) {
        strlcpy(req->hex, hex, sizeof(req->hex));
        s_busy = true;
        xTaskCreate(photo_task, "photo", 8192, req, 3, NULL);
    }
}
