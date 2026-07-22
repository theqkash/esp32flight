#include "trails.h"

#include <stdint.h>
#include <string.h>

#define TRAIL_SLOTS 64

typedef struct {
    char     hex[7];
    uint8_t  n;
    uint8_t  head;
    uint32_t last_cycle;
    float    lat[TRAIL_LEN];
    float    lon[TRAIL_LEN];
} trail_t;

static trail_t s_trails[TRAIL_SLOTS];
static uint32_t s_cycle;

static trail_t *slot_for(const char *hex)
{
    trail_t *oldest = &s_trails[0];
    for (int i = 0; i < TRAIL_SLOTS; i++) {
        if (s_trails[i].hex[0] != '\0' && strcmp(s_trails[i].hex, hex) == 0) {
            return &s_trails[i];
        }
        if (s_trails[i].last_cycle < oldest->last_cycle) {
            oldest = &s_trails[i];
        }
    }
    memset(oldest, 0, sizeof(*oldest));
    strlcpy(oldest->hex, hex, sizeof(oldest->hex));
    return oldest;
}

void trails_update(const aircraft_list_t *list)
{
    s_cycle++;
    for (int i = 0; i < list->count; i++) {
        const aircraft_t *ac = &list->ac[i];
        if (!ac->has_pos || ac->hex[0] == '\0') {
            continue;
        }
        trail_t *t = slot_for(ac->hex);
        t->last_cycle = s_cycle;
        /* skip duplicates when the position did not change */
        if (t->n > 0) {
            int last = (t->head + TRAIL_LEN - 1) % TRAIL_LEN;
            if (t->lat[last] == (float)ac->lat && t->lon[last] == (float)ac->lon) {
                continue;
            }
        }
        t->lat[t->head] = (float)ac->lat;
        t->lon[t->head] = (float)ac->lon;
        t->head = (t->head + 1) % TRAIL_LEN;
        if (t->n < TRAIL_LEN) {
            t->n++;
        }
    }
}

int trails_get(const char *hex, float *lat, float *lon, int max)
{
    for (int i = 0; i < TRAIL_SLOTS; i++) {
        if (s_trails[i].hex[0] != '\0' && strcmp(s_trails[i].hex, hex) == 0) {
            const trail_t *t = &s_trails[i];
            int n = t->n < max ? t->n : max;
            int start = (t->head + TRAIL_LEN - t->n) % TRAIL_LEN;
            for (int k = 0; k < n; k++) {
                int idx = (start + (t->n - n) + k) % TRAIL_LEN;
                lat[k] = t->lat[idx];
                lon[k] = t->lon[idx];
            }
            return n;
        }
    }
    return 0;
}
