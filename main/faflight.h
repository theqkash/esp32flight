#pragma once

#include <stdbool.h>

/* Commercial (IATA) flight numbers via FlightAware AeroAPI. Only active
 * when an API key is configured. Cached per callsign; empty string means
 * a negative result.
 *
 * Every AeroAPI call is billed, so lookups run under a monthly budget
 * (settings fa_limit, default FA_DEFAULT_MONTHLY) that is persisted with
 * the settings and spread evenly over the month (#37). */
#define FA_DEFAULT_MONTHLY 900
/* AeroAPI list price of one /flights/{ident} result set, USD. Estimate
 * only: FlightAware's invoice is what counts. */
#define FA_COST_PER_CALL   0.005

const char *faflight_get_cached(const char *callsign);   /* NULL = not fetched */

/* True when a billed lookup may run now: key set, budget left this month,
 * pacing allows it and no recent auth/rate-limit refusal. */
bool faflight_can_fetch(void);

/* Blocking lookup; returns the cached value, NULL when it did not run. */
const char *faflight_fetch(const char *callsign);

/* Usage for the web panel: calls made this UTC month and the limit. */
void faflight_usage(int *used, int *limit);
