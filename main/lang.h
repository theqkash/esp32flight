#pragma once

/* UI language (settings_get()->lang: 0 = English, 1 = Polski) */

typedef struct {
    /* statuses */
    const char *connecting;
    const char *setup_wifi;          /* takes the gear symbol via %s */
    const char *locating;
    const char *geo_retry;
    const char *no_data;
    const char *status_fmt;          /* city, count, km */
    /* views */
    const char *waiting_aircraft;
    const char *route_unknown;
    const char *km_to_go_fmt;        /* pct, km, [eta] */
    const char *ground;
    const char *map_btn;
    const char *ring_fmt;            /* ring km */
    /* stat tiles */
    const char *st_alt, *st_speed, *st_vrate, *st_dist, *st_track, *st_reg;
    /* settings screen */
    const char *settings_title;
    const char *wifi_ssid, *password;
    const char *auto_location, *hide_ground, *airline_only;
    const char *city_search, *latitude, *longitude;
    const char *search_radius, *theme_lbl, *language_lbl, *save;
    const char *ota_unlock;
    const char *dd_networks, *dd_results;
    const char *scanning, *no_networks, *not_found, *searching;
    const char *saved_restarting;
    /* photo */
    const char *loading_photo, *no_photo;
    /* weather (WMO code description) */
    const char *wx_clear, *wx_partly, *wx_overcast, *wx_fog, *wx_drizzle,
               *wx_rain, *wx_snow, *wx_showers, *wx_thunder;
} lang_t;

const lang_t *L(void);

/* Localized weather description for a WMO code. */
const char *lang_weather_desc(int code);

/* 8-sector compass label ("NW") for a bearing in degrees. */
const char *lang_compass(int deg);
