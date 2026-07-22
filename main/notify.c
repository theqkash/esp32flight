#include "notify.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "http_util.h"
#include "settings.h"

static const char *TAG = "notify";

void notify_send(const char *title, const char *message)
{
    const char *topic = settings_get()->ntfy_topic;
    if (topic[0] == '\0') {
        return;
    }
    char url[96];
    snprintf(url, sizeof(url), "https://ntfy.sh/%s", topic);
    if (http_post_text(url, message, "Title", title) == ESP_OK) {
        ESP_LOGI(TAG, "sent: %s", title);
    } else {
        ESP_LOGW(TAG, "failed: %s", title);
    }
}
