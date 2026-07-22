#pragma once

#include <stdbool.h>
#include "esp_err.h"

#include "esp_wifi.h"

/* Init netif + Wi-Fi STA (always) and start connecting if an SSID is
 * configured in settings (non-blocking). */
esp_err_t wifi_mgr_start(void);

/* Blocking scan (~2-4 s). Call from a worker task, never from the UI. */
esp_err_t wifi_mgr_scan(wifi_ap_record_t *records, uint16_t *count);

/* Block until connected with an IP, or timeout. */
bool wifi_mgr_wait_connected(int timeout_ms);

bool wifi_mgr_is_connected(void);
