#include "dailystats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DAYS_PATH "/assets/days.tsv"
#define MAX_DAYS  30

typedef struct {
    char date[11];
    int  unique, alt, gs, km;
} day_t;

static int load_days(day_t *days)
{
    FILE *f = fopen(DAYS_PATH, "r");
    if (f == NULL) {
        return 0;
    }
    int n = 0;
    char line[80];
    while (n < MAX_DAYS && fgets(line, sizeof(line), f) != NULL) {
        day_t *d = &days[n];
        if (sscanf(line, "%10s %d %d %d %d",
                   d->date, &d->unique, &d->alt, &d->gs, &d->km) == 5) {
            n++;
        }
    }
    fclose(f);
    return n;
}

void dailystats_update(const char *date, int unique, int max_alt_ft,
                       int max_gs_kt, int max_dist_km)
{
    static day_t days[MAX_DAYS];
    int n = load_days(days);

    int idx = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(days[i].date, date) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (n == MAX_DAYS) {
            memmove(&days[0], &days[1], sizeof(day_t) * (MAX_DAYS - 1));
            n--;
        }
        idx = n++;
        strlcpy(days[idx].date, date, sizeof(days[idx].date));
        days[idx].unique = days[idx].alt = days[idx].gs = days[idx].km = 0;
    }
    days[idx].unique = unique;
    days[idx].alt = max_alt_ft;
    days[idx].gs = max_gs_kt;
    days[idx].km = max_dist_km;

    FILE *f = fopen(DAYS_PATH, "w");
    if (f == NULL) {
        return;
    }
    for (int i = 0; i < n; i++) {
        fprintf(f, "%s\t%d\t%d\t%d\t%d\n",
                days[i].date, days[i].unique, days[i].alt, days[i].gs, days[i].km);
    }
    fclose(f);
}

void dailystats_to_json(cJSON *parent, const char *key)
{
    static day_t days[MAX_DAYS];
    int n = load_days(days);
    cJSON *arr = cJSON_AddArrayToObject(parent, key);
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "d", days[i].date);
        cJSON_AddNumberToObject(e, "u", days[i].unique);
        cJSON_AddNumberToObject(e, "alt", days[i].alt);
        cJSON_AddNumberToObject(e, "gs", days[i].gs);
        cJSON_AddNumberToObject(e, "km", days[i].km);
        cJSON_AddItemToArray(arr, e);
    }
}

void dailystats_summary(char *dst, size_t n, const char *avg_word, const char *best_word)
{
    static day_t days[MAX_DAYS];
    int cnt = load_days(days);
    dst[0] = '\0';
    if (cnt == 0) {
        return;
    }
    int from = cnt > 7 ? cnt - 7 : 0;
    int sum = 0, best = 0;
    for (int i = from; i < cnt; i++) {
        sum += days[i].unique;
        if (days[i].unique > best) {
            best = days[i].unique;
        }
    }
    snprintf(dst, n, "7d: %s %d/d  \xC2\xB7  %s %d",
             avg_word, sum / (cnt - from), best_word, best);
}
