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
} settings_t;

/* Load from NVS (menuconfig values as first-boot defaults). Call once at
 * startup, after nvs_flash_init. */
void settings_load(void);

/* Persist current contents of settings_get() to NVS. */
esp_err_t settings_save(void);

settings_t *settings_get(void);
