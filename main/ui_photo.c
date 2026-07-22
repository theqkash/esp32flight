#include "ui_photo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
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

static void photo_task(void *arg)
{
    photo_req_t *req = arg;
    char url_full[256] = "";
    char url_thumb[256] = "";

    char *json = malloc(8192);
    if (json != NULL) {
        char api[80];
        snprintf(api, sizeof(api), "https://api.adsbdb.com/v0/aircraft/%s", req->hex);
        if (http_get_to_buffer(api, json, 8192, NULL) == ESP_OK) {
            cJSON *root = cJSON_Parse(json);
            if (root != NULL) {
                const cJSON *resp = cJSON_GetObjectItem(root, "response");
                const cJSON *acj = resp ? cJSON_GetObjectItem(resp, "aircraft") : NULL;
                const cJSON *ju = acj ? cJSON_GetObjectItem(acj, "url_photo") : NULL;
                if (cJSON_IsString(ju)) {
                    strlcpy(url_full, ju->valuestring, sizeof(url_full));
                }
                ju = acj ? cJSON_GetObjectItem(acj, "url_photo_thumbnail") : NULL;
                if (cJSON_IsString(ju)) {
                    strlcpy(url_thumb, ju->valuestring, sizeof(url_thumb));
                }
                cJSON_Delete(root);
            }
        }
        free(json);
    }

    /* Prefer the full photo (scaled down at decode), fall back to thumbnail */
    uint16_t *pixels = NULL;
    int w = 0, h = 0;
    uint8_t *raw = heap_caps_malloc(PHOTO_MAX_BYTES, MALLOC_CAP_SPIRAM);
    if (raw != NULL) {
        const char *urls[2] = { url_full, url_thumb };
        for (int i = 0; i < 2 && pixels == NULL; i++) {
            size_t len = 0;
            if (urls[i][0] == '\0') {
                continue;
            }
            if (http_get_to_buffer(urls[i], (char *)raw, PHOTO_MAX_BYTES, &len) == ESP_OK && len > 4) {
                pixels = jpeg_decode_rgb565(raw, len, 700, 380, &w, &h);
            }
        }
        free(raw);
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
                lv_label_set_text(s_credit, "photo: planespotters.net");
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
    lv_obj_set_size(s_overlay, 800, 480);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, COL_BG, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_label_set_text_fmt(title, "%s  -  %s", callsign, hex);
    lv_obj_set_pos(title, 16, 12);

    lv_obj_t *btn_close = lv_btn_create(s_overlay);
    lv_obj_set_size(btn_close, 52, 40);
    lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_obj_set_style_bg_color(btn_close, COL_PANEL, 0);
    lv_obj_add_event_cb(btn_close, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(btn_close);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_center(xl);

    s_msg = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_msg, &font_pl_20, 0);
    lv_obj_set_style_text_color(s_msg, COL_DIM, 0);
    lv_label_set_text(s_msg, L()->loading_photo);
    lv_obj_center(s_msg);

    s_img = lv_img_create(s_overlay);
    lv_obj_center(s_img);
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);

    s_credit = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_credit, &font_pl_14, 0);
    lv_obj_set_style_text_color(s_credit, COL_DIM, 0);
    lv_label_set_text(s_credit, "");
    lv_obj_align(s_credit, LV_ALIGN_BOTTOM_LEFT, 16, -8);

    photo_req_t *req = malloc(sizeof(photo_req_t));
    if (req != NULL) {
        strlcpy(req->hex, hex, sizeof(req->hex));
        s_busy = true;
        xTaskCreate(photo_task, "photo", 8192, req, 3, NULL);
    }
}
