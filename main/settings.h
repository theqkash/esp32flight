#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char   wifi_ssid[33];
    char   wifi_pass[65];
    bool   use_fixed_loc;
    double lat;
    double lon;
    int    radius_nm;
    bool   hide_ground;
    bool   hide_private;    /* hide non-airline traffic (callsign not AAA123-style) */
    int    theme;           /* index into theme.c palettes */
    int    lang;            /* 0 = English, 1 = Polski */
    bool   ota_enabled;     /* volatile: always false after boot, armed from
                               the on-device settings screen only */
    /* integrations, edited via the web panel (/api/config) */
    char   ntfy_topic[48];  /* ntfy.sh topic for push notifications */
    char   mqtt_uri[96];    /* e.g. mqtt://user:pass@192.168.1.5:1883 */
    char   fa_key[48];      /* FlightAware AeroAPI key (IATA flight numbers) */
    char   watch_regs[96];  /* comma-separated watchlist (regs/callsign prefixes) */
    char   webhook_url[96]; /* generic JSON webhook for events */
    char   local_adsb[96];  /* dump1090/readsb aircraft.json URL (LAN receiver) */
    bool   local_adsb_use;  /* off = internet sources despite a saved URL (#17) */
    bool   temp_f;          /* header weather in Fahrenheit (#25) */
    char   web_pass[33];    /* HTTP Basic Auth for the web panel, empty = open */
    bool   cpa_alerts;      /* push when an interesting aircraft will pass close */
    bool   cpa_all;         /* flyover alerts for every aircraft, not only interesting */
    bool   night_enabled;
    int    night_start_min; /* minutes from midnight, local */
    int    night_end_min;
    int    ambient_idle_min;  /* full-screen map screensaver after N idle min, 0=off */
    char   filter_airport[8]; /* airport filter (ICAO/IATA), empty=off */
    bool   filter_apt_exclude; /* false: show only that airport, true: hide it */
    int    alt_min_ft;        /* altitude band filter, 0 = no bound */
    int    alt_max_ft;
    /* keep new fields at the end: the app build persists this struct as a
     * versioned blob and reads old files as a prefix */
    uint8_t show_classes;     /* bitmask of flight_class_t, FCLS_ALL_MASK = all */
    bool    rain_overlay;     /* RainViewer precipitation layer on maps/radar */
    bool    show_route;       /* route codes on list rows + cities in the map bubble (#28) */
    bool    brightness_ctl;  /* master enable for backlight dimming, off = stock behavior */
    uint8_t brightness;       /* 5..100, applied where the backlight can dim (#13/HA) */
    bool    clock_12h;        /* AM/PM clock everywhere a time is shown */
    char    input_map[176];   /* physical input mapping, see input_ctl.c (#13) */
    uint8_t amb_style;        /* screensaver: 0 = map, 1 = retro radar */
    bool    taf_enabled;      /* fetch TAF along with METAR */
    bool    iss_enabled;      /* ISS as an extra radar object */
    bool    sonde_enabled;    /* SondeHub radiosondes as radar objects */
    bool    ships_enabled;    /* AIS ships via aisstream.io (needs ais_key) */
    bool    airspace_enabled; /* openAIP airspace outlines (needs openaip_key) */
    char    openaip_key[64];  /* openAIP client id */
    char    ais_key[48];      /* aisstream.io API key */
    char    carto_key[48];    /* CARTO basemaps key; empty = OSM tiles + on-device dark style */
    char    tile_url[112];    /* custom tile URL template with {z}/{x}/{y}; overrides both */
    bool    metric_units;     /* m + km/h instead of ft + kt */
    bool    metar_decoded;    /* human-readable METAR instead of raw */
    bool    follow_mode;      /* stick to the selected aircraft, no auto-cycle */
    bool    night_auto;       /* night hours follow local sunset/sunrise */
    char    fav_name[3][24];  /* favorite locations, empty name = free slot */
    double  fav_lat[3], fav_lon[3];
    bool    map_light;        /* lift dark map tiles for readability (#10) */
    bool    retro_map;        /* green map underlay on the retro scope */
    /* 0.4.7 (app blob v6 reads a 0.4.6 file as the prefix up to fa_limit) */
    uint16_t fa_limit;        /* AeroAPI calls per month, 0 = FA_DEFAULT_MONTHLY (#37) */
    uint16_t fa_used;         /* calls made in fa_month, persisted on every call */
    uint16_t fa_month;        /* UTC year*12+month the counter belongs to */
    bool    pin_nearest;      /* detail view stays on the nearest aircraft (#44) */
    bool    amb_sched;        /* screensaver only inside the window below (#43) */
    int     amb_from_min;     /* minutes from midnight, local */
    int     amb_to_min;
    bool    local_merge;      /* local receiver plus internet aggregators */
    char    track_id[12];     /* flight to follow anywhere: callsign, reg or hex */
    bool    track_on;         /* track mode: the screen shows only that flight */
} settings_t;

/* Load from NVS (menuconfig values as first-boot defaults). Call once at
 * startup, after nvs_flash_init. */
void settings_load(void);

/* Persist current contents of settings_get() to NVS. */
esp_err_t settings_save(void);

/* Persist only the FlightAware usage counter (fa_used/fa_month); cheap
 * enough to run on every billed call. */
void settings_save_fa_usage(void);

settings_t *settings_get(void);
