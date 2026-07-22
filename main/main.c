#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "waveshare_rgb_lcd_port.h"

#include "airports.h"
#include "flight_task.h"
#include "logos.h"
#include "settings.h"
#include "tilemap.h"
#include "ui.h"
#include "ui_map.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "canflight";

void app_main(void)
{
    /* Local time for the clock and ETAs (Europe/Warsaw with DST) */
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    settings_load();

    ESP_ERROR_CHECK(waveshare_esp32_s3_rgb_lcd_init());
    waveshare_rgb_lcd_bl_on();

    logos_init();
    airports_init();
    tilemap_init();
    /* pre-decode both world maps so map opens never decode PNGs at draw time */
    ui_map_get_image();
    ui_map_get_image_small();

    if (lvgl_port_lock(-1)) {
        ui_init();
        lvgl_port_unlock();
    }

    err = wifi_mgr_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi init failed: %s", esp_err_to_name(err));
    }

    web_server_start();
    flight_task_start();
}
