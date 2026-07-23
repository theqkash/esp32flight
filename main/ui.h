#pragma once

#include "flight_model.h"

/* All ui_* functions must be called while holding the LVGL lock
 * (lvgl_port_lock/unlock), except from LVGL's own callbacks. */

void ui_init(void);

/* One-line status in the header (Wi-Fi / location / errors). */
void ui_set_status(const char *text);

/* Weather summary shown in place of the app title. */
void ui_set_weather(const char *text);

/* Emergency styling for the status line (red, larger). */
void ui_set_status_alert(bool alert);

/* Amber on-screen banner for an incoming flyover (auto-hides, tap to close).
 * Safe to call from any task. */
void ui_flyover_banner(const char *callsign, int eta_min, double cpa_km);

/* Highlight the gear icon when a newer release is on GitHub. */
void ui_set_update_available(bool available);

/* Home coordinates (after geolocation) - used by the radar map view. */
void ui_set_home(double lat, double lon);

/* Replace displayed flight data. Routes are looked up via routes_get_cached
 * and snapshotted, so later touch interactions don't race the network task. */
void ui_update(const aircraft_list_t *list);
