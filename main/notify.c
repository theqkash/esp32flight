#include "notify.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "http_util.h"
#include "settings.h"

static const char *TAG = "notify";

void notify_send(const char *title, const char *message)
{
    const settings_t *cfg = settings_get();

    if (cfg->ntfy_topic[0] != '\0') {
        char url[96];
        snprintf(url, sizeof(url), "https://ntfy.sh/%s", cfg->ntfy_topic);
        if (http_post_text(url, message, "Title", title) == ESP_OK) {
            ESP_LOGI(TAG, "ntfy sent: %s", title);
        } else {
            ESP_LOGW(TAG, "ntfy failed: %s", title);
        }
    }

    if (cfg->webhook_url[0] != '\0') {
        char body[256];
        snprintf(body, sizeof(body),
                 "{\"source\":\"esp32flight\",\"title\":\"%s\",\"message\":\"%s\"}",
                 title, message);
        if (http_post_text(cfg->webhook_url, body,
                           "Content-Type", "application/json") != ESP_OK) {
            ESP_LOGW(TAG, "webhook failed");
        }
    }
}
