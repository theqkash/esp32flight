#pragma once

#include "flight_model.h"

/* Observation log: one line per unique aircraft spotted, kept in SPIFFS.
 * Survives OTA updates (assets partition is untouched). */
void obslog_append(const aircraft_t *ac, const char *airline);

#define OBSLOG_PATH     "/assets/obslog.tsv"
#define OBSLOG_OLD_PATH "/assets/obslog.old.tsv"
