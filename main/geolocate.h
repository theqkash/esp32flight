#pragma once

#include <stddef.h>
#include "esp_err.h"

/* Resolve device location: fixed coords from menuconfig if enabled,
 * otherwise IP-based geolocation. city may be NULL. */
esp_err_t geolocate_get(double *lat, double *lon, char *city, size_t city_len);
