#pragma once

#include "lvgl.h"

#define THEME_COUNT 7   /* Dark, Light, Black, Nord, Solarized, Purple, Forest */

typedef struct {
    lv_color_t bg;
    lv_color_t panel;
    lv_color_t row;
    lv_color_t row_sel;
    lv_color_t accent;
    lv_color_t text;
    lv_color_t dim;
} app_theme_t;

/* Active theme, resolved from settings_get()->theme. */
const app_theme_t *app_theme(void);

const char *theme_names_option_string(void);   /* "Dark\nLight\nBlack..." */

/* FR24-style altitude color: low = warm, cruise = cool. */
lv_color_t alt_color(int alt_ft, bool on_ground);
