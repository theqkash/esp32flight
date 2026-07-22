#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    float temp_c;
    float wind_kmh;
    int   wind_dir_deg;
    int   code;         /* WMO weather code */
    bool  valid;
} weather_t;

/* Current conditions from the Open-Meteo forecast API (free, no key).
 * Blocking; call from a network task. */
esp_err_t weather_fetch(double lat, double lon, weather_t *out);

/* Short English description for a WMO weather code. */
const char *weather_code_str(int code);

/* FontAwesome glyph (UTF-8) for a WMO weather code; render with font_pl_*. */
const char *weather_icon_str(int code);
