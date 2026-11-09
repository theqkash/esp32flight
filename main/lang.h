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
    const char *ota_hint;
    /* spotter */
    const char *look_fmt;        /* compass, elevation */
    const char *look_short_fmt;  /* compact variant when CPA is shown */
    const char *cpa_fmt;         /* minutes, km */
    /* settings tabs + new options */
    const char *tab_net, *tab_place, *tab_filters, *tab_integr, *tab_system;
    const char *cpa_lbl, *night_lbl, *night_from, *night_to, *amb_idle_lbl;
    const char *lbl_ntfy, *lbl_mqtt, *lbl_fa, *lbl_watch, *lbl_webhook, *lbl_ladsb;
    const char *hint_ntfy, *hint_mqtt, *hint_fa, *hint_webhook, *hint_ladsb;
    const char *lbl_webpass;
    const char *cat_word;        /* ADS-B emitter category label */
    const char *sec_classes;             /* class filter section header */
    const char *cls_names[5];            /* flight_class_t display names */
    const char *sec_layers, *rain_lbl;   /* precipitation overlay */
    const char *map_light_lbl;
    const char *retro_map_lbl;
    const char *ladsb_use_lbl;
    const char *amb_style_lbl, *amb_style_opts;   /* screensaver style */
    const char *amb_loading;   /* screensaver placeholder before tiles land */
    const char *apr_fmt;         /* approach: city/icao, minutes */
    const char *lbl_fltapt, *lbl_altmin, *lbl_altmax;
    const char *sec_traffic, *sec_watch, *sec_alerts;
    const char *sec_look, *sec_screen, *sec_webpanel, *sec_updates;
    const char *cpa_scope_opts;  /* dropdown: interesting-only / all */
    const char *iss_lbl, *sonde_lbl, *ships_lbl, *airspace_lbl, *taf_lbl;
    const char *lbl_oaip, *hint_oaip, *lbl_ais, *hint_ais;
    const char *sec_maptiles, *lbl_carto, *hint_carto, *lbl_tileurl, *hint_tileurl;
    const char *route_lbl;
    const char *bright_lbl, *clk12_lbl, *brightctl_lbl;
    const char *sec_notify, *sec_datasrc, *sec_smart;
    const char *lm_planes, *lm_ships, *lm_all;   /* list content toggle */
    const char *ship_dest, *ship_pos;            /* ship detail tiles */
    const char *units_lbl, *units_opts;          /* aviation vs metric */
    const char *metar_lbl, *metar_opts;          /* raw vs decoded */
    const char *follow_lbl;                      /* stick to selected flight */
    const char *mtr_wind, *mtr_gust, *mtr_calm, *mtr_vis, *mtr_cavok;
    const char *wifi_no_ap, *wifi_badpass;
    const char *night_auto_lbl;
    const char *update_banner;   /* new release available, %s = tag */
    const char *apt_mode_opts;   /* dropdown: show only / hide */
    const char *arr_fmt;         /* local arrival time, %s = HH:MM */
    const char *avg_word, *best_word;
    /* stats view */
    const char *stats_title;
    const char *st_hourly;
    const char *st_top_airlines;
    const char *st_unique;
    const char *st_fastest, *st_farthest, *st_highest;
    const char *dd_networks, *dd_results;
    const char *scanning, *no_networks, *not_found, *searching;
    const char *saved_restarting;
    /* photo */
    const char *loading_photo, *no_photo;
    /* weather (WMO code description) */
    const char *pin_near_lbl, *amb_sched_lbl, *lmerge_lbl;
    const char *track_fmt, *track_wait_fmt, *track_lbl, *track_on_lbl;
    const char *wx_clear, *wx_partly, *wx_overcast, *wx_fog, *wx_drizzle,
               *wx_rain, *wx_snow, *wx_showers, *wx_thunder;
} lang_t;

const lang_t *L(void);

/* Localized weather description for a WMO code. */
const char *lang_weather_desc(int code);

/* 8-sector compass label ("NW") for a bearing in degrees. */
const char *lang_compass(int deg);

const char *lang_metar_wx(const char *code, int lang);  /* 0 en, 1 pl, 2 hu */
