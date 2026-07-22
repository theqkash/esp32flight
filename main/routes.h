#pragma once

#include "flight_model.h"

/* Cached route lookup. Returns entry (valid or negative-cached) or NULL if
 * this callsign has not been fetched yet. */
const route_info_t *routes_get_cached(const char *callsign);

/* Fetch route for callsign and store in cache (also stores negative
 * results). Sources: adsbdb.com, then hexdb.io. When the aircraft position
 * is known, each source's answer is sanity-checked against it - a route the
 * aircraft can't plausibly be on triggers the next source instead of being
 * shown. Call from a network task, never from the UI. */
const route_info_t *routes_fetch(const char *callsign,
                                 double ac_lat, double ac_lon, bool has_pos);
