#pragma once

/* Airline full name by 3-letter ICAO code (e.g. "TLJ" -> "Thalair").
 * Cached; fetch from adsbdb in a network task, read cache from the UI. */
const char *airlines_get_cached(const char *icao);   /* NULL if not fetched yet */
const char *airlines_fetch(const char *icao);        /* blocking; "" if unknown */
