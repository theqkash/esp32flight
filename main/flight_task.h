#pragma once

#include <stdint.h>

/* Start the background task: Wi-Fi wait -> geolocate -> poll aircraft ->
 * fetch routes -> push updates into the UI. */
void flight_task_start(void);

/* Session statistics snapshot (for the stats view and the web panel). */
typedef struct {
    int      unique;
    int      max_alt_ft;
    float    max_gs_kt;
    float    max_dist_km;
    char     max_dist_cs[9];
    uint16_t hours[24];              /* new aircraft per local hour */
    struct {
        char     code[4];
        uint16_t n;
    } top[8];
    int      top_n;
} app_stats_t;

void flight_stats_get(app_stats_t *out);
