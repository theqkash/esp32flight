#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Web-mercator tile view composed from CARTO dark tiles (online).
 * Blocking (downloads tiles); call from a worker task, never from the UI. */

typedef struct {
    int    z;           /* tile zoom */
    double px0, py0;    /* view origin in global pixels at zoom z */
    int    w, h;
} tile_view_t;

/* Call once at startup (creates the render serialization mutex). */
void tilemap_init(void);

/* Compose a view around the bbox into dst (RGB565, dst_w x dst_h).
 * Returns false when tiles could not be fetched (offline etc.). */
bool tilemap_render(uint16_t *dst, int dst_w, int dst_h,
                    double lat_min, double lat_max,
                    double lon_min, double lon_max,
                    tile_view_t *out_view);

/* Project WGS84 to view pixel coordinates. */
void tilemap_project(const tile_view_t *v, double lat, double lon, int *x, int *y);
