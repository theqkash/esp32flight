#pragma once

/* Start HTTP server (port 80) + mDNS (http://canflight.local).
 * Endpoints: GET / (panel), GET /api/state (JSON), POST /ota (firmware). */
void web_server_start(void);

/* Publish a fresh JSON state snapshot (copied; caller keeps ownership). */
void web_state_publish(const char *json);
