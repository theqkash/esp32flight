#include "obslog.h"

#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "esp_log.h"

static const char *TAG = "obslog";

#define OBSLOG_MAX_BYTES (256 * 1024)

void obslog_append(const aircraft_t *ac, const char *airline)
{
    struct stat st;
    if (stat(OBSLOG_PATH, &st) == 0 && st.st_size > OBSLOG_MAX_BYTES) {
        unlink(OBSLOG_OLD_PATH);
        rename(OBSLOG_PATH, OBSLOG_OLD_PATH);
        ESP_LOGI(TAG, "rotated log");
    }

    FILE *f = fopen(OBSLOG_PATH, "a");
    if (f == NULL) {
        return;
    }
    fprintf(f, "%lld\t%s\t%s\t%s\t%s\n",
            (long long)time(NULL),
            ac->hex,
            ac->callsign[0] ? ac->callsign : "-",
            ac->type_icao[0] ? ac->type_icao : "-",
            airline != NULL && airline[0] ? airline : "-");
    fclose(f);
}
