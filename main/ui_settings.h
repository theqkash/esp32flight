#pragma once

/* Open the full-screen settings overlay (call from LVGL context). Saving
 * persists to NVS and restarts the device to apply cleanly. */
void ui_settings_open(void);
