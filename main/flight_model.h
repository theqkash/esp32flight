#pragma once

#include <stdbool.h>

#define MAX_AIRCRAFT      80
#define CALLSIGN_LEN      9    /* 8 chars + NUL */
#define ICAO_HEX_LEN      7
#define REG_LEN           12
#define TYPE_ICAO_LEN     8
#define TYPE_DESC_LEN     48
#define AIRPORT_CODE_LEN  5    /* ICAO 4 + NUL */
#define IATA_CODE_LEN     4
#define NAME_LEN          56
#define CITY_LEN          48
#define AIRLINE_ICAO_LEN  4

typedef struct {
    char   hex[ICAO_HEX_LEN];
    char   callsign[CALLSIGN_LEN];      /* trimmed */
    char   reg[REG_LEN];                /* may be empty */
    char   type_icao[TYPE_ICAO_LEN];    /* e.g. "A20N", may be empty */
    char   type_desc[TYPE_DESC_LEN];    /* e.g. "AIRBUS A-320neo", airplanes.live only */
    double lat, lon;
    bool   has_pos;
    bool   on_ground;
    int    alt_baro_ft;
    float  gs_kts;
    float  track_deg;
    int    baro_rate_fpm;
    float  dist_nm;                     /* distance from query point ("dst") */
    float  dir_deg;                     /* bearing from query point ("dir") */
    char   category[4];                 /* ADS-B emitter category, e.g. "A3" */
    char   squawk[5];
    bool   military;                    /* airplanes.live dbFlags bit 0 */
} aircraft_t;

typedef struct {
    char   icao[AIRPORT_CODE_LEN];
    char   iata[IATA_CODE_LEN];
    char   name[NAME_LEN];
    char   city[CITY_LEN];
    char   country[4];              /* ISO code, e.g. "PL" */
    double lat, lon;
    int    tz_offset_s;             /* local UTC offset */
    bool   tz_known;
} airport_t;

typedef struct {
    char      callsign[CALLSIGN_LEN];
    bool      valid;                    /* false => negative cache entry */
    long long fetched_ms;               /* negative entries expire and retry */
    char      airline_name[NAME_LEN];
    char      airline_icao[AIRLINE_ICAO_LEN];
    airport_t origin;
    airport_t destination;
} route_info_t;

typedef struct {
    aircraft_t ac[MAX_AIRCRAFT];
    int        count;
    long long  fetched_at_ms;
} aircraft_list_t;

/* Scheduled airline traffic: callsign like "RYR76ZJ" (3-letter ICAO airline
 * code + flight id). Registration-style callsigns count as private. */
bool flight_is_airline(const aircraft_t *ac);

/* Worth highlighting: military, notable heavy types (A380, AN-124, C-17...)
 * or a match against the user's comma-separated watchlist. */
bool flight_is_interesting(const aircraft_t *ac, const char *watchlist);
