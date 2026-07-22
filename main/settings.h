#pragma once

#include <stdbool.h>
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
    bool   cpa_alerts;      /* push when an interesting aircraft will pass close */
    bool   night_enabled;
    int    night_start_min; /* minutes from midnight, local */
    int    night_end_min;
    int    ambient_idle_min;  /* full-screen map screensaver after N idle min, 0=off */
} settings_t;

/* Load from NVS (menuconfig values as first-boot defaults). Call once at
 * startup, after nvs_flash_init. */
void settings_load(void);

/* Persist current contents of settings_get() to NVS. */
esp_err_t settings_save(void);

settings_t *settings_get(void);
