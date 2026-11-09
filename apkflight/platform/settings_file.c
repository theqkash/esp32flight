/* settings.c backend for desktop/Android: same settings.h as the device,
 * persisted to a small binary file next to the assets (via the /assets
 * path remap, so it lands in the app's private storage). */

#include "settings.h"
#include "faflight.h"

#include <stddef.h>
#include "flight_model.h"

#include <stdio.h>
#include <string.h>

#define SETTINGS_PATH "/assets/settings.bin"
#define SETTINGS_MAGIC 0x414B4631u   /* "AKF1" */
#define SETTINGS_VER   6

static settings_t s_settings;

settings_t *settings_get(void)
{
    return &s_settings;
}

static void set_defaults(void)
{
    memset(&s_settings, 0, sizeof(s_settings));
    /* wider default reach than the device: a phone on a desk wants to see
     * high-altitude overflights too, which matter most when local traffic
     * is thin (nights, small airports) */
    s_settings.radius_nm = 150;
    s_settings.hide_ground = true;
    s_settings.theme = 0;
    s_settings.lang = 0;   /* English out of the box; 1 = Polish */
    s_settings.cpa_alerts = true;
    s_settings.night_enabled = false;
    s_settings.night_start_min = 23 * 60;
    s_settings.night_end_min = 6 * 60 + 30;
    s_settings.ambient_idle_min = 10;
    s_settings.show_classes = FCLS_ALL_MASK;
    s_settings.rain_overlay = false;
    s_settings.amb_style = 0;
    s_settings.fa_limit = FA_DEFAULT_MONTHLY;
    s_settings.amb_from_min = 22 * 60;
    s_settings.amb_to_min = 7 * 60;
}

void settings_load(void)
{
    set_defaults();

    FILE *f = fopen(SETTINGS_PATH, "rb");
    if (f == NULL) {
        return;
    }
    uint32_t magic = 0, ver = 0;
    if (fread(&magic, sizeof(magic), 1, f) == 1 && magic == SETTINGS_MAGIC &&
        fread(&ver, sizeof(ver), 1, f) == 1) {
        settings_t tmp;
        if (ver == SETTINGS_VER && fread(&tmp, sizeof(tmp), 1, f) == 1) {
            s_settings = tmp;
        } else if (ver == 5) {
            /* v5 = 0.4.6 layout, a prefix up to the 0.4.7 fields. Earlier
             * v5 files (0.4.5) have another size and keep the defaults. */
            size_t v5 = offsetof(settings_t, fa_limit);
            if (fread(&tmp, 1, sizeof(tmp), f) == v5) {
                memcpy(&s_settings, &tmp, v5);
            }
        } else if (ver == 4) {
            /* v4 predates map_light; the old struct is a prefix */
            size_t v4 = offsetof(settings_t, map_light);
            if (fread(&tmp, 1, v4, f) == v4) {
                memcpy(&s_settings, &tmp, v4);
            }
        } else if (ver == 3) {
            /* v3 predates the 0.4.x additions (taf/iss/sondes/ships/keys,
             * units, favorites); the old struct is a prefix */
            size_t v3 = offsetof(settings_t, taf_enabled);
            if (fread(&tmp, 1, v3, f) == v3) {
                memcpy(&s_settings, &tmp, v3);
            }
        } else if (ver == 2) {
            /* v2 predates rain_overlay/amb_style */
            size_t v2 = offsetof(settings_t, rain_overlay);
            if (fread(&tmp, 1, v2, f) == v2) {
                memcpy(&s_settings, &tmp, v2);
            }
        } else if (ver == 1) {
            /* v1 predates show_classes; the old struct is a prefix */
            size_t v1 = offsetof(settings_t, show_classes);
            if (fread(&tmp, 1, v1, f) == v1) {
                memcpy(&s_settings, &tmp, v1);
            }
        }
    }
    if ((s_settings.show_classes & FCLS_ALL_MASK) == 0) {
        s_settings.show_classes = FCLS_ALL_MASK;
    }
    fclose(f);
    s_settings.ota_enabled = false;   /* volatile: never restored */
}

esp_err_t settings_save(void)
{
    FILE *f = fopen(SETTINGS_PATH, "wb");
    if (f == NULL) {
        return ESP_FAIL;
    }
    uint32_t magic = SETTINGS_MAGIC, ver = SETTINGS_VER;
    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&ver, sizeof(ver), 1, f);
    fwrite(&s_settings, sizeof(s_settings), 1, f);
    fclose(f);
    return ESP_OK;
}

void settings_save_fa_usage(void)
{
    settings_save();
}
