#include "metar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "http_util.h"
#include "lang.h"
#include "settings.h"

static const char *TAG = "metar";

static char s_metar[160];

bool metar_fetch(const char *icao)
{
    char url[128];
    snprintf(url, sizeof(url),
             "https://aviationweather.gov/api/data/metar?ids=%s&format=json", icao);
    char *buf = malloc(4096);
    if (buf == NULL) {
        return false;
    }
    bool ok = false;
    if (http_get_to_buffer_t(url, buf, 4096, NULL, 5000) == ESP_OK) {
        cJSON *root = cJSON_Parse(buf);
        const cJSON *first = cJSON_IsArray(root) ? cJSON_GetArrayItem(root, 0) : NULL;
        const cJSON *raw = first ? cJSON_GetObjectItem(first, "rawOb") : NULL;
        if (cJSON_IsString(raw)) {
            strlcpy(s_metar, raw->valuestring, sizeof(s_metar));
            ok = true;
            ESP_LOGI(TAG, "%s", s_metar);
        }
        if (root != NULL) {
            cJSON_Delete(root);
        }
    }
    free(buf);
    return ok;
}

const char *metar_get(void)
{
    return s_metar;
}

static char s_taf[400];

bool taf_fetch(const char *icao)
{
    char url[128];
    snprintf(url, sizeof(url),
             "https://aviationweather.gov/api/data/taf?ids=%s&format=raw", icao);
    char *buf = malloc(2048);
    if (buf == NULL) {
        return false;
    }
    bool ok = false;
    if (http_get_to_buffer_t(url, buf, 2048, NULL, 5000) == ESP_OK && buf[0] != '\0') {
        /* collapse the pretty-printed continuation lines into one string */
        int o = 0;
        bool sp = false;
        for (const char *c = buf; *c != '\0' && o < (int)sizeof(s_taf) - 1; c++) {
            if (*c == '\n' || *c == '\r' || *c == ' ') {
                sp = o > 0;
                continue;
            }
            if (sp) {
                s_taf[o++] = ' ';
                sp = false;
            }
            if (o < (int)sizeof(s_taf) - 1) {
                s_taf[o++] = *c;
            }
        }
        s_taf[o] = '\0';
        ok = o > 0;
        if (ok) {
            ESP_LOGI(TAG, "TAF: %s", s_taf);
        }
    }
    free(buf);
    return ok;
}

const char *taf_get(void)
{
    return s_taf;
}

/* Turn the raw METAR into a one-line human summary. Best-effort: unknown
 * groups are skipped, so odd stations degrade to fewer segments. */
const char *metar_decoded(char *out, size_t n)
{
    out[0] = '\0';
    const settings_t *cfg = settings_get();
    int pl = cfg->lang;   /* language index for lang_metar_wx */
    size_t o = 0;
    char tok[24];
    const char *c = s_metar;
    int idx = 0;
    bool had_wx = false;
    int cover_max = -1, cover_ft = 0;
    static const char *cover_txt[] = { "1-2/8", "3-4/8", "5-7/8", "8/8" };

    while (*c != '\0' && o + 24 < n) {
        int tl = 0;
        while (*c == ' ') {
            c++;
        }
        while (*c != ' ' && *c != '\0' && tl < (int)sizeof(tok) - 1) {
            tok[tl++] = *c++;
        }
        tok[tl] = '\0';
        if (tl == 0) {
            break;
        }
        idx++;
        if (idx <= 2 || strcmp(tok, "AUTO") == 0) {
            continue;   /* station + time */
        }
        if (strcmp(tok, "RMK") == 0) {
            break;
        }
        int d, sp, gu;
        char un[4];
        if (sscanf(tok, "%3d%2dG%2d%3s", &d, &sp, &gu, un) == 4 &&
            (strcmp(un, "KT") == 0 || strcmp(un, "MPS") == 0)) {
            float kt = strcmp(un, "MPS") == 0 ? sp * 1.944f : (float)sp;
            float gkt = strcmp(un, "MPS") == 0 ? gu * 1.944f : (float)gu;
            o += snprintf(out + o, n - o, "%s %s %.0f%s, %s %.0f",
                          L()->mtr_wind, lang_compass(d),
                          cfg->metric_units ? kt * 1.852f : kt,
                          cfg->metric_units ? " km/h" : " kt",
                          L()->mtr_gust,
                          cfg->metric_units ? gkt * 1.852f : gkt);
            continue;
        }
        if (strncmp(tok, "VRB", 3) == 0 && sscanf(tok + 3, "%2d%3s", &sp, un) == 2 &&
            (strcmp(un, "KT") == 0 || strcmp(un, "MPS") == 0)) {
            float kt = strcmp(un, "MPS") == 0 ? sp * 1.944f : (float)sp;
            o += snprintf(out + o, n - o, "%s VRB %.0f%s", L()->mtr_wind,
                          cfg->metric_units ? kt * 1.852f : kt,
                          cfg->metric_units ? " km/h" : " kt");
            continue;
        }
        if (sscanf(tok, "%3d%2d%3s", &d, &sp, un) == 3 &&
            (strcmp(un, "KT") == 0 || strcmp(un, "MPS") == 0)) {
            if (sp == 0) {
                o += snprintf(out + o, n - o, "%s %s", L()->mtr_wind, L()->mtr_calm);
            } else {
                float kt = strcmp(un, "MPS") == 0 ? sp * 1.944f : (float)sp;
                o += snprintf(out + o, n - o, "%s %s %.0f%s",
                              L()->mtr_wind, lang_compass(d),
                              cfg->metric_units ? kt * 1.852f : kt,
                              cfg->metric_units ? " km/h" : " kt");
            }
            continue;
        }
        if (strcmp(tok, "CAVOK") == 0) {
            o += snprintf(out + o, n - o, " \xC2\xB7 %s", L()->mtr_cavok);
            continue;
        }
        if (tl == 4 && sscanf(tok, "%4d", &d) == 1 && strcmp(tok, "9999") != 0 &&
            idx <= 6) {
            o += snprintf(out + o, n - o, " \xC2\xB7 %s %.1f km", L()->mtr_vis,
                          d / 1000.0);
            continue;
        }
        if (strcmp(tok, "9999") == 0) {
            o += snprintf(out + o, n - o, " \xC2\xB7 %s >10 km", L()->mtr_vis);
            continue;
        }
        /* weather groups, e.g. -SHRA, +TSRA, VCFG */
        {
            const char *w = tok;
            char pfx = 0;
            if (*w == '+' || *w == '-') {
                pfx = *w++;
            }
            char seg[64] = "";
            int sl = 0;
            bool ok = *w != '\0';
            while (*w != '\0' && ok) {
                const char *word = lang_metar_wx(w, pl);
                if (word == NULL) {
                    ok = false;
                    break;
                }
                sl += snprintf(seg + sl, sizeof(seg) - sl, "%s%s",
                               sl ? " " : "", word);
                w += 2;
            }
            if (ok && sl > 0) {
                o += snprintf(out + o, n - o, " \xC2\xB7 %s%s%s",
                              pfx == '+' ? "+" : pfx == '-' ? "-" : "", "", seg);
                had_wx = true;
                continue;
            }
        }
        int hh;
        if ((strncmp(tok, "FEW", 3) == 0 || strncmp(tok, "SCT", 3) == 0 ||
             strncmp(tok, "BKN", 3) == 0 || strncmp(tok, "OVC", 3) == 0) &&
            sscanf(tok + 3, "%3d", &hh) == 1) {
            int cv = strncmp(tok, "FEW", 3) == 0 ? 0 :
                     strncmp(tok, "SCT", 3) == 0 ? 1 :
                     strncmp(tok, "BKN", 3) == 0 ? 2 : 3;
            if (cv > cover_max) {
                cover_max = cv;
                cover_ft = hh * 100;
            }
            continue;
        }
        int t1, t2;
        if (sscanf(tok, "%d/%d", &t1, &t2) == 2 && tl <= 7 && strchr(tok, '/')) {
            o += snprintf(out + o, n - o, " \xC2\xB7 %d\xC2\xB0/%d\xC2\xB0", t1, t2);
            continue;
        }
        if (sscanf(tok, "M%d/M%d", &t1, &t2) == 2) {
            o += snprintf(out + o, n - o, " \xC2\xB7 -%d\xC2\xB0/-%d\xC2\xB0", t1, t2);
            continue;
        }
        if (sscanf(tok, "%d/M%d", &t1, &t2) == 2) {
            o += snprintf(out + o, n - o, " \xC2\xB7 %d\xC2\xB0/-%d\xC2\xB0", t1, t2);
            continue;
        }
        if (tok[0] == 'Q' && sscanf(tok + 1, "%4d", &t1) == 1) {
            o += snprintf(out + o, n - o, " \xC2\xB7 %d hPa", t1);
            continue;
        }
    }
    if (cover_max >= 0 && o + 24 < n) {
        char ub[20];
        if (cfg->metric_units) {
            snprintf(ub, sizeof(ub), "%d m", (int)(cover_ft * 0.3048 + 0.5));
        } else {
            snprintf(ub, sizeof(ub), "%d ft", cover_ft);
        }
        o += snprintf(out + o, n - o, " \xC2\xB7 %s %s", cover_txt[cover_max], ub);
    }
    (void)had_wx;
    return out;
}
