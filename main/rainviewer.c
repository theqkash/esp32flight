#include "rainviewer.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "http_util.h"

static const char *TAG = "rain";

/* Just under the 10-minute view refresh (#46): with a TTL equal to it, a
 * frame list fetched a few seconds into one render was still "fresh" at
 * the next, so the weather advanced only every other refresh. */
#define FRAME_TTL_US (9LL * 60 * 1000 * 1000)

static char    s_path[160];
static int     s_generation;
static int64_t s_fetched_at = -1;

const char *rainviewer_frame_path(void)
{
    int64_t now = esp_timer_get_time();
    if (s_fetched_at >= 0 && now - s_fetched_at < FRAME_TTL_US) {
        return s_path;
    }

    char *buf = malloc(8192);
    if (buf == NULL) {
        return s_path;
    }
    size_t len = 0;
    esp_err_t err = http_get_to_buffer("https://api.rainviewer.com/public/weather-maps.json",
                                       buf, 8191, &len);
    if (err != ESP_OK || len == 0) {
        ESP_LOGW(TAG, "frame list fetch failed: %s", esp_err_to_name(err));
        free(buf);
        s_fetched_at = now;    /* don't hammer the API while offline */
        return s_path;
    }
    buf[len] = '\0';

    char fresh[160] = "";
    cJSON *root = cJSON_Parse(buf);
    if (root != NULL) {
        const cJSON *host = cJSON_GetObjectItem(root, "host");
        const cJSON *radar = cJSON_GetObjectItem(root, "radar");
        const cJSON *past = radar ? cJSON_GetObjectItem(radar, "past") : NULL;
        int n = past ? cJSON_GetArraySize(past) : 0;
        if (n > 0 && cJSON_IsString(host)) {
            const cJSON *last = cJSON_GetArrayItem(past, n - 1);
            const cJSON *path = last ? cJSON_GetObjectItem(last, "path") : NULL;
            if (cJSON_IsString(path)) {
                snprintf(fresh, sizeof(fresh), "%s%s",
                         host->valuestring, path->valuestring);
            }
        }
        cJSON_Delete(root);
    }
    free(buf);

    if (fresh[0] != '\0') {
        if (strcmp(fresh, s_path) != 0) {
            strlcpy(s_path, fresh, sizeof(s_path));
            s_generation++;
            ESP_LOGI(TAG, "frame %s (gen %d)", s_path, s_generation);
        }
    }
    s_fetched_at = now;
    return s_path;
}

int rainviewer_generation(void)
{
    return s_generation;
}
