#pragma once

#include "esp_err.h"
#include "flight_model.h"

/* Fetch aircraft within radius_nm of lat/lon. Tries airplanes.live first,
 * falls back to adsb.lol (same JSON schema). Result sorted by distance. */
esp_err_t flight_fetch_nearby(double lat, double lon, int radius_nm, aircraft_list_t *out);
