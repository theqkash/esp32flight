#pragma once

#include <stdbool.h>

/* UTC offset (seconds) for a location, resolved via the Open-Meteo
 * timezone=auto endpoint and cached per key (airport ICAO). Blocking on
 * cache miss - call from a network task. */
bool tz_offset_for(const char *key, double lat, double lon, int *offset_s);

/* Home (device location) timezone: set once after geolocation, used by the
 * header clock and ETA. Falls back to the TZ env (Europe/Warsaw) when
 * unknown. */
void tz_set_home_offset(int offset_s);
bool tz_home_known(void);
int  tz_home_offset(void);
