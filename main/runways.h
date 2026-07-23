#pragma once

#include <stdint.h>
#include "tilemap.h"

/* Draw runway strips of scheduled-service airports into an RGB565 map
 * buffer rendered with the given view. Cheap enough to call after every
 * tilemap_render; does nothing when the runway database is absent. */
void runways_draw(uint16_t *fb, int w, int h, const tile_view_t *view);
