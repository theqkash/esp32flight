#pragma once

#include "flight_model.h"

#define TRAIL_LEN 24

/* Record current positions of all aircraft (call once per poll cycle). */
void trails_update(const aircraft_list_t *list);

/* Copy up to max points of a trail, oldest first. Returns point count. */
int trails_get(const char *hex, float *lat, float *lon, int max);
