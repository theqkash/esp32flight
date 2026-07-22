#include "wifi_mgr.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "settings.h"

static const char *TAG = "wifi";

static EventGroupHandle_t s_events;
#define BIT_CONNECTED BIT0

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    bool have_ssid = settings_get()->wifi_ssid[0] != '\0';
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (have_ssid) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        if (have_ssid) {
            ESP_LOGW(TAG, "disconnected, retrying");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

esp_err_t wifi_mgr_start(void)
{
    const settings_t *st = settings_get();

    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (st->wifi_ssid[0] != '\0') {
        wifi_config_t wifi_config = { 0 };
        strlcpy((char *)wifi_config.sta.ssid, st->wifi_ssid, sizeof(wifi_config.sta.ssid));
        strlcpy((char *)wifi_config.sta.password, st->wifi_pass, sizeof(wifi_config.sta.password));
        wifi_config.sta.threshold.authmode = st->wifi_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_LOGI(TAG, "connecting to \"%s\"", st->wifi_ssid);
    } else {
        ESP_LOGW(TAG, "no SSID configured; Wi-Fi up for scanning only");
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

esp_err_t wifi_mgr_scan(wifi_ap_record_t *records, uint16_t *count)
{
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return err;
    }
    return esp_wifi_scan_get_ap_records(count, records);
}

bool wifi_mgr_wait_connected(int timeout_ms)
{
    if (s_events == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdTRUE,
                                           timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms));
    return (bits & BIT_CONNECTED) != 0;
}

bool wifi_mgr_is_connected(void)
{
    if (s_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_events) & BIT_CONNECTED) != 0;
}
