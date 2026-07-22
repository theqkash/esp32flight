#include "weather.h"

#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"
#include "esp_log.h"
#include "http_util.h"

static const char *TAG = "weather";

esp_err_t weather_fetch(double lat, double lon, weather_t *out)
{
    out->valid = false;

    char url[192];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,weather_code,wind_speed_10m,wind_direction_10m&wind_speed_unit=kmh",
             lat, lon);

    char *buf = malloc(4096);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = http_get_to_buffer(url, buf, 4096, NULL);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *cur = cJSON_GetObjectItem(root, "current");
    const cJSON *jt = cur ? cJSON_GetObjectItem(cur, "temperature_2m") : NULL;
    const cJSON *jc = cur ? cJSON_GetObjectItem(cur, "weather_code") : NULL;
    const cJSON *jw = cur ? cJSON_GetObjectItem(cur, "wind_speed_10m") : NULL;
    const cJSON *jd = cur ? cJSON_GetObjectItem(cur, "wind_direction_10m") : NULL;
    if (cJSON_IsNumber(jt)) {
        out->temp_c = (float)jt->valuedouble;
        out->code = cJSON_IsNumber(jc) ? (int)jc->valuedouble : -1;
        out->wind_kmh = cJSON_IsNumber(jw) ? (float)jw->valuedouble : 0;
        out->wind_dir_deg = cJSON_IsNumber(jd) ? (int)jd->valuedouble : -1;
        out->valid = true;
    }
    cJSON_Delete(root);
    if (!out->valid) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "%.1f C, code %d, wind %.0f km/h", out->temp_c, out->code, out->wind_kmh);
    return ESP_OK;
}

/* FontAwesome codepoints included in font_pl_* (see tools/gen_fonts.sh) */
#define ICON_SUN        "\xEF\x86\x85"  /* f185 */
#define ICON_CLOUD_SUN  "\xEF\x9B\x84"  /* f6c4 */
#define ICON_CLOUD      "\xEF\x83\x82"  /* f0c2 */
#define ICON_SMOG       "\xEF\x9D\x9F"  /* f75f */
#define ICON_RAIN       "\xEF\x9C\xBD"  /* f73d */
#define ICON_SHOWERS    "\xEF\x9D\x80"  /* f740 */
#define ICON_SNOW       "\xEF\x8B\x9C"  /* f2dc */
#define ICON_BOLT       "\xEF\x83\xA7"  /* f0e7 */

const char *weather_icon_str(int code)
{
    if (code == 0) return ICON_SUN;
    if (code <= 2) return ICON_CLOUD_SUN;
    if (code == 3) return ICON_CLOUD;
    if (code == 45 || code == 48) return ICON_SMOG;
    if (code >= 51 && code <= 67) return ICON_RAIN;
    if (code >= 71 && code <= 77) return ICON_SNOW;
    if (code >= 80 && code <= 82) return ICON_SHOWERS;
    if (code == 85 || code == 86) return ICON_SNOW;
    if (code >= 95) return ICON_BOLT;
    return ICON_CLOUD;
}

const char *weather_code_str(int code)
{
    if (code == 0) return "Clear";
    if (code <= 2) return "Partly cloudy";
    if (code == 3) return "Overcast";
    if (code == 45 || code == 48) return "Fog";
    if (code >= 51 && code <= 57) return "Drizzle";
    if (code >= 61 && code <= 67) return "Rain";
    if (code >= 71 && code <= 77) return "Snow";
    if (code >= 80 && code <= 82) return "Showers";
    if (code == 85 || code == 86) return "Snow showers";
    if (code >= 95) return "Thunderstorm";
    return "";
}
