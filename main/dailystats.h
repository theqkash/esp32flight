#pragma once

#include "cJSON.h"

/* Persist today's counters (call periodically; rewrites a small TSV kept in
 * the assets partition, max 30 days). */
void dailystats_update(const char *date, int unique, int max_alt_ft,
                       int max_gs_kt, int max_dist_km);

/* Append the stored days as a JSON array [{d,u,alt,gs,km}...]. */
void dailystats_to_json(cJSON *parent, const char *key);

/* Short device summary, e.g. "7d: avg 142/day, best 240". */
void dailystats_summary(char *dst, size_t n, const char *avg_word, const char *best_word);
