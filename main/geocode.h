#pragma once

#include "esp_err.h"

typedef struct {
    char   name[40];
    char   region[32];
    char   country[8];      /* ISO code */
    double lat;
    double lon;
} geocode_result_t;

/* Search cities by name via the Open-Meteo geocoding API (free, no key).
 * Blocking; call from a worker task. */
esp_err_t geocode_search(const char *query, geocode_result_t *results, int max, int *count);
