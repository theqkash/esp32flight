#pragma once

#include <stddef.h>

#define ALERTLOG_PATH     "/assets/alerts.tsv"
#define ALERTLOG_OLD_PATH "/assets/alerts.old.tsv"

/* Append one alert (emergency squawk, watchlist hit, flyover...) to the
 * on-flash history: epoch \t title \t message. Rotates at ~64 KB. */
void alertlog_append(const char *title, const char *message);
