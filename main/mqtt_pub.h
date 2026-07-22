#pragma once

/* MQTT publishing with Home Assistant discovery. No-op when no broker URI
 * is configured. */
void mqtt_pub_start(void);
void mqtt_pub_state(const char *json);
