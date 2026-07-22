#pragma once

/* Start the background task: Wi-Fi wait -> geolocate -> poll aircraft ->
 * fetch routes -> push updates into the UI. */
void flight_task_start(void);
