#include "jpeg_dec.h"

#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"

#include "extra/libs/sjpg/tjpgd.h"

typedef struct {
    const uint8_t *data;
    size_t         size;
    size_t         pos;
    uint16_t      *out;
    int            out_w;
} jpg_ctx_t;

static size_t jpg_in(JDEC *jd, uint8_t *buf, size_t n)
{
    jpg_ctx_t *c = jd->device;
    if (n > c->size - c->pos) {
        n = c->size - c->pos;
    }
    if (buf != NULL) {
        memcpy(buf, c->data + c->pos, n);
    }
    c->pos += n;
    return n;
}

static int jpg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpg_ctx_t *c = jd->device;
    const uint8_t *src = bitmap;
    for (int y = rect->top; y <= rect->bottom; y++) {
        uint16_t *dst = c->out + y * c->out_w + rect->left;
        for (int x = rect->left; x <= rect->right; x++) {
            uint8_t r = src[0], g = src[1], b = src[2];
            src += 3;
            *dst++ = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        }
    }
    return 1;
}

uint16_t *jpeg_decode_rgb565(const uint8_t *data, size_t len,
                             int max_w, int max_h, int *out_w, int *out_h)
{
    void *work = malloc(8192);
    if (work == NULL) {
        return NULL;
    }
    jpg_ctx_t ctx = { .data = data, .size = len, .pos = 0 };
    JDEC jd;
    if (jd_prepare(&jd, jpg_in, work, 8192, &ctx) != JDR_OK) {
        free(work);
        return NULL;
    }
    uint8_t scale = 0;
    while (scale < 3 && ((jd.width >> scale) > max_w || (jd.height >> scale) > max_h)) {
        scale++;
    }
    *out_w = jd.width >> scale;
    *out_h = jd.height >> scale;
    ctx.out = heap_caps_malloc((size_t)*out_w * *out_h * 2, MALLOC_CAP_SPIRAM);
    ctx.out_w = *out_w;
    if (ctx.out == NULL) {
        free(work);
        return NULL;
    }
    JRESULT rc = jd_decomp(&jd, jpg_out, scale);
    free(work);
    if (rc != JDR_OK) {
        free(ctx.out);
        return NULL;
    }
    return ctx.out;
}
