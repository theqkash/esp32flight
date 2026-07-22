#include "flags.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"

#define FLAG_CACHE_SIZE 24

typedef struct {
    char         cc[3];
    uint8_t     *data;
    lv_img_dsc_t dsc;
    uint32_t     last_used;
    bool         used;
} flag_entry_t;

static flag_entry_t s_cache[FLAG_CACHE_SIZE];
static uint32_t s_tick;

const lv_img_dsc_t *flags_get(const char *cc)
{
    if (cc == NULL || strlen(cc) != 2) {
        return NULL;
    }
    char lc[3] = { (char)tolower((unsigned char)cc[0]),
                   (char)tolower((unsigned char)cc[1]), '\0' };

    s_tick++;
    for (int i = 0; i < FLAG_CACHE_SIZE; i++) {
        if (s_cache[i].used && strcmp(s_cache[i].cc, lc) == 0) {
            s_cache[i].last_used = s_tick;
            return &s_cache[i].dsc;
        }
    }

    char path[40];
    snprintf(path, sizeof(path), "/assets/flags/%s.png", lc);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 8 * 1024) {
        fclose(f);
        return NULL;
    }

    uint8_t *data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (data == NULL) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(data, 1, size, f);
    fclose(f);
    if (rd != (size_t)size) {
        free(data);
        return NULL;
    }

    int slot = 0;
    uint32_t oldest = UINT32_MAX;
    for (int i = 0; i < FLAG_CACHE_SIZE; i++) {
        if (!s_cache[i].used) {
            slot = i;
            break;
        }
        if (s_cache[i].last_used < oldest) {
            oldest = s_cache[i].last_used;
            slot = i;
        }
    }
    if (s_cache[slot].used && s_cache[slot].data != NULL) {
        free(s_cache[slot].data);
    }

    flag_entry_t *e = &s_cache[slot];
    memset(e, 0, sizeof(*e));
    strlcpy(e->cc, lc, sizeof(e->cc));
    e->data = data;
    e->used = true;
    e->last_used = s_tick;
    /* Raw PNG bytes; LVGL's PNG decoder reads dimensions from the header. */
    e->dsc.header.always_zero = 0;
    e->dsc.header.cf = LV_IMG_CF_RAW_ALPHA;
    e->dsc.data = e->data;
    e->dsc.data_size = size;
    return &e->dsc;
}
