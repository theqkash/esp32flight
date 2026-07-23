#include "alertlog.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ALERTLOG_MAX_BYTES (64 * 1024)

void alertlog_append(const char *title, const char *message)
{
    struct stat st;
    if (stat(ALERTLOG_PATH, &st) == 0 && st.st_size > ALERTLOG_MAX_BYTES) {
        unlink(ALERTLOG_OLD_PATH);
        rename(ALERTLOG_PATH, ALERTLOG_OLD_PATH);
    }

    FILE *f = fopen(ALERTLOG_PATH, "a");
    if (f == NULL) {
        return;
    }
    /* tabs and newlines in the payload would break the TSV */
    char t[64], m[128];
    strlcpy(t, title != NULL ? title : "", sizeof(t));
    strlcpy(m, message != NULL ? message : "", sizeof(m));
    for (char *p = t; *p; p++) {
        if (*p == '\t' || *p == '\n') {
            *p = ' ';
        }
    }
    for (char *p = m; *p; p++) {
        if (*p == '\t' || *p == '\n') {
            *p = ' ';
        }
    }
    fprintf(f, "%lld\t%s\t%s\n", (long long)time(NULL), t, m);
    fclose(f);
}
