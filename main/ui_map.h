#pragma once

#include "flight_model.h"

#include "lvgl.h"

/* Full-screen route map overlay: world map with the great-circle path,
 * origin/destination markers and current aircraft position. Works without a
 * route too (position only). Call from LVGL context. */
void ui_map_open(const aircraft_t *ac, const route_info_t *rt);

/* Lazily-loaded world map images (equirectangular), NULL if missing. */
const lv_img_dsc_t *ui_map_get_image(void);        /* 800x400 */
const lv_img_dsc_t *ui_map_get_image_small(void);  /* 490x245, for the embedded panel */
