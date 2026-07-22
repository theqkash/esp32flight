#include "logos.h"

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "logos";

#define LOGO_CACHE_SIZE 40

typedef struct {
    char         icao[4];
    uint8_t     *data;
    lv_img_dsc_t dsc;
    uint32_t     last_used;
    bool         used;
} logo_entry_t;

static logo_entry_t s_cache[LOGO_CACHE_SIZE];
static uint32_t s_tick;

esp_err_t logos_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/assets",
        .partition_label = "assets",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spiffs mount failed: %s", esp_err_to_name(err));
        return err;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info("assets", &total, &used);
    ESP_LOGI(TAG, "assets fs: %u/%u KB used", (unsigned)(used / 1024), (unsigned)(total / 1024));
    return ESP_OK;
}

const lv_img_dsc_t *logos_get(const char *airline_icao)
{
    if (airline_icao == NULL || strlen(airline_icao) != 3) {
        return NULL;
    }

    s_tick++;
    for (int i = 0; i < LOGO_CACHE_SIZE; i++) {
        if (s_cache[i].used && strcmp(s_cache[i].icao, airline_icao) == 0) {
            s_cache[i].last_used = s_tick;
            return &s_cache[i].dsc;
        }
    }

    char path[48];
    snprintf(path, sizeof(path), "/assets/logos/%s.png", airline_icao);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 64 * 1024) {
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

    /* Evict LRU slot */
    int slot = 0;
    uint32_t oldest = UINT32_MAX;
    for (int i = 0; i < LOGO_CACHE_SIZE; i++) {
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

    logo_entry_t *e = &s_cache[slot];
    memset(e, 0, sizeof(*e));
    strlcpy(e->icao, airline_icao, sizeof(e->icao));
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
