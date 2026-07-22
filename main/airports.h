#pragma once

#include <stdbool.h>
#include "flight_model.h"

/* Local airport database (OurAirports dataset in SPIFFS). Call after the
 * assets partition is mounted. */
void airports_init(void);

/* Fill iata/city/country/name/coordinates for an ICAO code from the local
 * database. Returns false if the airport is unknown. */
bool airports_lookup(const char *icao, airport_t *ap);

/* Nearest airport (ICAO) to a point; false when the database is missing. */
bool airports_nearest(double lat, double lon, char icao_out[5]);
