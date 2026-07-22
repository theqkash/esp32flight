#include "airports.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "airports";

static char *s_db;      /* whole TSV in PSRAM, newline-separated */
static size_t s_len;

void airports_init(void)
{
    FILE *f = fopen("/assets/airports.tsv", "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "airports.tsv missing; falling back to online lookups");
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return;
    }
    s_db = heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM);
    if (s_db == NULL) {
        fclose(f);
        return;
    }
    s_len = fread(s_db, 1, size, f);
    s_db[s_len] = '\0';
    fclose(f);

    int lines = 0;
    for (size_t i = 0; i < s_len; i++) {
        if (s_db[i] == '\n') {
            lines++;
        }
    }
    ESP_LOGI(TAG, "%d airports loaded (%u KB)", lines, (unsigned)(s_len / 1024));
}

bool airports_nearest(double lat, double lon, char icao_out[5])
{
    if (s_db == NULL) {
        return false;
    }
    double best = 1e18;
    icao_out[0] = '\0';
    const char *p = s_db;
    while (p != NULL && *p != '\0') {
        /* ICAO \t IATA \t CITY \t CC \t LAT \t LON \t NAME */
        const char *f = p;
        int tab = 0;
        const char *lat_s = NULL, *lon_s = NULL;
        while (*f != '\0' && *f != '\n') {
            if (*f == '\t') {
                tab++;
                if (tab == 4) {
                    lat_s = f + 1;
                } else if (tab == 5) {
                    lon_s = f + 1;
                }
            }
            f++;
        }
        if (lat_s != NULL && lon_s != NULL) {
            double alat = atof(lat_s);
            double alon = atof(lon_s);
            double dy = (alat - lat) * 110.57;
            double dx = (alon - lon) * 111.32 * 0.64; /* rough, fine for ranking */
            double d2 = dx * dx + dy * dy;
            if (d2 < best) {
                best = d2;
                snprintf(icao_out, 5, "%.4s", p);
            }
        }
        p = *f ? f + 1 : f;
    }
    return icao_out[0] != '\0';
}

bool airports_lookup(const char *icao, airport_t *ap)
{
    if (s_db == NULL || icao == NULL || strlen(icao) != 4) {
        return false;
    }

    /* Lines start with "ICAO\t"; scan line starts only */
    char key[6];
    snprintf(key, sizeof(key), "%s\t", icao);
    const char *p = s_db;
    while (p != NULL && *p != '\0') {
        if (strncmp(p, key, 5) == 0) {
            break;
        }
        p = strchr(p, '\n');
        if (p != NULL) {
            p++;
        }
    }
    if (p == NULL || *p == '\0') {
        return false;
    }

    /* ICAO IATA CITY COUNTRY LAT LON NAME */
    const char *fields[7];
    int n = 0;
    fields[n++] = p;
    for (const char *c = p; *c != '\0' && *c != '\n' && n < 7; c++) {
        if (*c == '\t') {
            fields[n++] = c + 1;
        }
    }
    if (n < 7) {
        return false;
    }

    size_t name_len = 0;
    {
        const char *e = fields[6];
        while (*e != '\0' && *e != '\n') {
            e++;
        }
        name_len = (size_t)(e - fields[6]);
    }

    strlcpy(ap->icao, icao, sizeof(ap->icao));
    size_t len;
    len = (size_t)(fields[2] - fields[1] - 1);
    snprintf(ap->iata, sizeof(ap->iata), "%.*s", (int)len, fields[1]);
    len = (size_t)(fields[3] - fields[2] - 1);
    snprintf(ap->city, sizeof(ap->city), "%.*s", (int)len, fields[2]);
    len = (size_t)(fields[4] - fields[3] - 1);
    snprintf(ap->country, sizeof(ap->country), "%.*s", (int)len, fields[3]);
    ap->lat = atof(fields[4]);
    ap->lon = atof(fields[5]);
    snprintf(ap->name, sizeof(ap->name), "%.*s", (int)name_len, fields[6]);
    return true;
}
