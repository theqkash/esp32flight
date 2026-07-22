#pragma once

#include "esp_err.h"
#include "lvgl.h"

/* Mount the "assets" SPIFFS partition (airline logo PNGs). */
esp_err_t logos_init(void);

/* Return an LVGL image descriptor for /assets/logos/<ICAO>.png, or NULL if
 * missing. Descriptors are cached; pointers stay valid until evicted from a
 * small LRU, so set on an lv_img promptly after lookup. */
const lv_img_dsc_t *logos_get(const char *airline_icao);
