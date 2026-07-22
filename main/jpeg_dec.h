#pragma once

#include <stddef.h>
#include <stdint.h>

/* Decode a baseline JPEG to RGB565 in PSRAM, auto-scaled (1/1..1/8) so the
 * result fits max_w x max_h. Caller frees the returned buffer. NULL on
 * failure (progressive JPEGs are not supported by tjpgd). */
uint16_t *jpeg_decode_rgb565(const uint8_t *data, size_t len,
                             int max_w, int max_h, int *out_w, int *out_h);
