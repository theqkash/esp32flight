/* Physical input interface (#13): see input_ctl.h for the mapping format.
 *
 * The expander is polled over the same legacy-driver I2C port the display
 * port already installed (S3 RGB family). On boards where that bus is not
 * up (P4 ports use the new i2c_master driver) detection fails cleanly and
 * only the HTTP action path stays active. */
#include "input_ctl.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#ifndef APKFLIGHT
#include "esp_timer.h"
#endif

#include "settings.h"
#include "mqtt_pub.h"
#include "ui.h"
#include "lvgl_port.h"
#ifndef APKFLIGHT
#include "waveshare_rgb_lcd_port.h"
#endif

static const char *TAG = "input";

#define POLL_MS        20
#define LONG_PRESS_MS  600
#define MAX_BTN_MAP    12
#define MAX_ENC_MAP    4

typedef struct {
    uint8_t pin;
    bool    long_press;
    char    action[20];
} btn_map_t;

typedef struct {
    uint8_t pin_a, pin_b;
    char    action_cw[20];
    char    action_ccw[20];
    int8_t  accum;
    uint8_t prev;
} enc_map_t;

#define MAX_GPIO_PINS  8
#define VPIN_GPIO_BASE 16   /* virtual pins 16.. map to s_map.gpio_num[] */

static struct {
    bool      pcf;            /* else MCP23017 */
    uint8_t   addr;           /* 0 = no expander declared */
    int       n_btn, n_enc;
    btn_map_t btn[MAX_BTN_MAP];
    enc_map_t enc[MAX_ENC_MAP];
    int       n_gpio;
    uint8_t   gpio_num[MAX_GPIO_PINS];
    bool      enabled;
} s_map;

/* Direct-GPIO whitelist (#13 layer 1, path 2): only pins that are truly
 * free on the active board. Strapping, RGB, PSRAM and peripheral pins
 * must never be poked, so unlisted boards allow none - the expander and
 * HTTP paths cover them. */
static bool gpio_pin_allowed(int pin)
{
#ifndef APKFLIGHT
    const char *b = waveshare_lcd_board_name();
    if (strstr(b, "JC8048W550") != NULL) {
        return pin == 17 || pin == 18;   /* broken out on the Guition header */
    }
#endif
    (void)pin;
    return false;
}

/* virtual pin for a GPIO, registering it on first use; -1 = full/denied */
static int vpin_for_gpio(unsigned gpio)
{
    if (!gpio_pin_allowed((int)gpio)) {
        ESP_LOGW(TAG, "input_map: GPIO %u not on this board's safe list", gpio);
        return -1;
    }
    for (int i = 0; i < s_map.n_gpio; i++) {
        if (s_map.gpio_num[i] == gpio) {
            return VPIN_GPIO_BASE + i;
        }
    }
    if (s_map.n_gpio >= MAX_GPIO_PINS) {
        return -1;
    }
    s_map.gpio_num[s_map.n_gpio] = (uint8_t)gpio;
    return VPIN_GPIO_BASE + s_map.n_gpio++;
}

static char     s_last_event[8];
static int64_t  s_last_event_us;

/* ---------- deferred settings save (encoder ticks must not hammer NVS) */

#ifdef APKFLIGHT
static void save_later(void)
{
    settings_save();   /* plain file write on Android, no debounce needed */
}
#else
static esp_timer_handle_t s_save_timer;

static void save_cb(void *arg)
{
    (void)arg;
    settings_save();
}

static void save_later(void)
{
    if (s_save_timer == NULL) {
        const esp_timer_create_args_t a = { .callback = save_cb, .name = "inpsave" };
        if (esp_timer_create(&a, &s_save_timer) != ESP_OK) {
            return;
        }
    }
    esp_timer_stop(s_save_timer);
    esp_timer_start_once(s_save_timer, 3 * 1000 * 1000);
}
#endif

/* ---------- actions ---------- */

static const char *k_actions =
    "next_view,prev_view,next_ac,prev_ac,zoom_in,zoom_out,wake,"
    "list_planes,list_ships,list_all,toggle_rain,follow_toggle,"
    "alt_min_up,alt_min_down,alt_max_up,alt_max_down,"
    "brightness_up,brightness_down,screensaver,screen_off,screen_on,screen_toggle,"
    "track_toggle";

const char *input_ctl_actions(void)
{
    return k_actions;
}

static void alt_toast(void)
{
    const settings_t *c = settings_get();
    char t[48];
    if (c->alt_max_ft > 0) {
        snprintf(t, sizeof(t), "ALT %d - %d ft", c->alt_min_ft, c->alt_max_ft);
    } else {
        snprintf(t, sizeof(t), "ALT %d ft +", c->alt_min_ft);
    }
    ui_toast(t);
}

static bool alt_action(const char *a)
{
    settings_t *c = settings_get();
    const int step = 1000, cap = 45000;
    if (strcmp(a, "alt_min_up") == 0) {
        c->alt_min_ft += step;
        if (c->alt_min_ft > cap) c->alt_min_ft = cap;
        if (c->alt_max_ft > 0 && c->alt_min_ft > c->alt_max_ft) c->alt_min_ft = c->alt_max_ft;
    } else if (strcmp(a, "alt_min_down") == 0) {
        c->alt_min_ft -= step;
        if (c->alt_min_ft < 0) c->alt_min_ft = 0;
    } else if (strcmp(a, "alt_max_up") == 0) {
        c->alt_max_ft += step;
        if (c->alt_max_ft > cap) c->alt_max_ft = 0;   /* past the cap = no bound */
    } else if (strcmp(a, "alt_max_down") == 0) {
        if (c->alt_max_ft == 0) c->alt_max_ft = cap;
        else if (c->alt_max_ft > step) c->alt_max_ft -= step;
        if (c->alt_min_ft > c->alt_max_ft) c->alt_max_ft = c->alt_min_ft;
    } else {
        return false;
    }
    alt_toast();
    save_later();
    return true;
}

bool input_ctl_dispatch(const char *action)
{
    if (action == NULL || action[0] == '\0') {
        return false;
    }
    /* UI-owned actions run under the LVGL lock */
    static const char *k_ui[] = { "next_view", "prev_view", "next_ac", "prev_ac",
                                  "zoom_in", "zoom_out", "wake",
                                  "list_planes", "list_ships", "list_all",
                                  "screensaver", "screen_off", "screen_on",
                                  "screen_toggle" };
    for (size_t i = 0; i < sizeof(k_ui) / sizeof(k_ui[0]); i++) {
        if (strcmp(action, k_ui[i]) == 0) {
            bool ok = false;
            if (lvgl_port_lock(500 / portTICK_PERIOD_MS)) {
                ok = ui_input_action(action);
                lvgl_port_unlock();
            }
            return ok;
        }
    }
    if (alt_action(action)) {
        return true;
    }
    settings_t *c = settings_get();
    if (strcmp(action, "toggle_rain") == 0) {
        c->rain_overlay = !c->rain_overlay;
        ui_toast(c->rain_overlay ? "Rain: on" : "Rain: off");
        save_later();
        return true;
    }
    if (strcmp(action, "track_toggle") == 0) {
        if (c->track_id[0] == '\0') {
            ui_toast("Set a flight to track first (web panel)");
            return true;
        }
        c->track_on = !c->track_on;
        char t[48];
        snprintf(t, sizeof(t), c->track_on ? "Tracking %s" : "Tracking off", c->track_id);
        ui_toast(t);
        save_later();
        return true;
    }
    if (strcmp(action, "follow_toggle") == 0) {
        c->follow_mode = !c->follow_mode;
        ui_toast(c->follow_mode ? "Follow: on" : "Auto-cycle: on");
        save_later();
        return true;
    }
    if (strcmp(action, "brightness_up") == 0 || strcmp(action, "brightness_down") == 0) {
        if (!c->brightness_ctl) {
            ui_toast("Brightness control is off (settings)");
            return true;
        }
        int b = c->brightness + (action[11] == 'u' ? 10 : -10);
        if (b > 100) b = 100;
        if (b < 5) b = 5;
        c->brightness = (uint8_t)b;
#ifndef APKFLIGHT
        if (waveshare_rgb_lcd_bl_dimmable()) {
            waveshare_rgb_lcd_bl_pct(b);
        }
#endif
        char t[32];
        snprintf(t, sizeof(t), "Brightness %d%%", b);
        ui_toast(t);
        mqtt_pub_backlight_changed();
        save_later();
        return true;
    }
    return false;
}

bool input_ctl_last_event(char *buf, size_t n, unsigned *age_ms)
{
#ifdef APKFLIGHT
    (void)buf; (void)n; (void)age_ms;
    return false;   /* no physical inputs on Android */
#else
    if (s_last_event[0] == '\0') {
        return false;
    }
    strlcpy(buf, s_last_event, n);
    if (age_ms != NULL) {
        *age_ms = (unsigned)((esp_timer_get_time() - s_last_event_us) / 1000);
    }
    return true;
#endif
}

/* ---------- mapping parser ---------- */

static bool action_known(const char *a)
{
    const char *p = k_actions;
    size_t l = strlen(a);
    while ((p = strstr(p, a)) != NULL) {
        bool at_start = p == k_actions || p[-1] == ',';
        bool at_end = p[l] == ',' || p[l] == '\0';
        if (at_start && at_end) {
            return true;
        }
        p += l;
    }
    return false;
}

static void parse_map(const char *spec)
{
    memset(&s_map, 0, sizeof(s_map));
    if (spec[0] == '\0') {
        return;
    }
    char buf[sizeof(((settings_t *)0)->input_map)];
    strlcpy(buf, spec, sizeof(buf));
    char *save = NULL;
    char *tok = strtok_r(buf, ";", &save);
    if (tok == NULL) {
        return;
    }
    unsigned addr = 0;
    if (sscanf(tok, "pcf@%x", &addr) == 1) {
        s_map.pcf = true;
    } else if (sscanf(tok, "mcp@%x", &addr) == 1) {
        s_map.pcf = false;
    } else {
        addr = 0;   /* no expander: GPIO-only map, first token is an entry */
    }
    if (addr != 0 && (addr < 0x08 || addr > 0x77)) {
        ESP_LOGW(TAG, "input_map: bad address 0x%02x", addr);
        return;
    }
    s_map.addr = (uint8_t)addr;
    int maxpin = s_map.pcf ? 7 : 15;
    if (addr == 0) {
        maxpin = -1;   /* no expander pins available */
    }
    for (tok = addr == 0 ? tok : strtok_r(NULL, ";", &save); tok != NULL;
         tok = strtok_r(NULL, ";", &save)) {
        unsigned pin = 0, pin_b = 0;
        char kind = 0;
        char act[41];
        if (sscanf(tok, "g%u%c=%40s", &pin, &kind, act) == 3 &&
            (kind == 's' || kind == 'l')) {
            int vp = vpin_for_gpio(pin);
            if (vp >= 0 && s_map.n_btn < MAX_BTN_MAP && action_known(act)) {
                btn_map_t *b = &s_map.btn[s_map.n_btn++];
                b->pin = (uint8_t)vp;
                b->long_press = kind == 'l';
                strlcpy(b->action, act, sizeof(b->action));
            }
            continue;
        }
        if (sscanf(tok, "q%u,%u=%40s", &pin, &pin_b, act) == 3 && pin != pin_b) {
            int va = vpin_for_gpio(pin), vb = vpin_for_gpio(pin_b);
            char *comma = strchr(act, ',');
            if (va >= 0 && vb >= 0 && comma != NULL && s_map.n_enc < MAX_ENC_MAP) {
                *comma = '\0';
                if (action_known(act) && action_known(comma + 1)) {
                    enc_map_t *e = &s_map.enc[s_map.n_enc++];
                    e->pin_a = (uint8_t)va;
                    e->pin_b = (uint8_t)vb;
                    strlcpy(e->action_cw, act, sizeof(e->action_cw));
                    strlcpy(e->action_ccw, comma + 1, sizeof(e->action_ccw));
                }
            }
            continue;
        }
        if (sscanf(tok, "b%u%c=%40s", &pin, &kind, act) == 3 &&
            (kind == 's' || kind == 'l') && (int)pin <= maxpin) {
            if (s_map.n_btn < MAX_BTN_MAP && action_known(act)) {
                btn_map_t *b = &s_map.btn[s_map.n_btn++];
                b->pin = (uint8_t)pin;
                b->long_press = kind == 'l';
                strlcpy(b->action, act, sizeof(b->action));
            }
            continue;
        }
        if (sscanf(tok, "e%u,%u=%40s", &pin, &pin_b, act) == 3 &&
            (int)pin <= maxpin && (int)pin_b <= maxpin && pin != pin_b) {
            char *comma = strchr(act, ',');
            if (comma != NULL && s_map.n_enc < MAX_ENC_MAP) {
                *comma = '\0';
                if (action_known(act) && action_known(comma + 1)) {
                    enc_map_t *e = &s_map.enc[s_map.n_enc++];
                    e->pin_a = (uint8_t)pin;
                    e->pin_b = (uint8_t)pin_b;
                    strlcpy(e->action_cw, act, sizeof(e->action_cw));
                    strlcpy(e->action_ccw, comma + 1, sizeof(e->action_ccw));
                }
            }
            continue;
        }
        ESP_LOGW(TAG, "input_map: skipping '%s'", tok);
    }
    s_map.enabled = s_map.n_btn > 0 || s_map.n_enc > 0;
}

/* ---------- expander polling (legacy I2C driver, S3 boards) ---------- */

#if !defined(APKFLIGHT) && !CONFIG_IDF_TARGET_ESP32P4

#include "driver/i2c.h"
#include "driver/gpio.h"

static esp_err_t exp_read(uint16_t *bits)
{
    if (s_map.pcf) {
        uint8_t v = 0xFF;
        esp_err_t err = i2c_master_read_from_device(I2C_MASTER_NUM, s_map.addr,
                                                    &v, 1, pdMS_TO_TICKS(50));
        *bits = 0xFF00 | v;
        return err;
    }
    uint8_t reg = 0x12;   /* GPIOA, then GPIOB auto-increments */
    uint8_t v[2] = { 0xFF, 0xFF };
    esp_err_t err = i2c_master_write_read_device(I2C_MASTER_NUM, s_map.addr,
                                                 &reg, 1, v, 2, pdMS_TO_TICKS(50));
    *bits = (uint16_t)(v[1] << 8) | v[0];
    return err;
}

static esp_err_t mcp_init(void)
{
    /* IODIRA/B = all input, GPPUA/B = pull-ups on */
    static const uint8_t regs[][2] = { {0x00, 0xFF}, {0x01, 0xFF},
                                       {0x0C, 0xFF}, {0x0D, 0xFF} };
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        esp_err_t err = i2c_master_write_to_device(I2C_MASTER_NUM, s_map.addr,
                                                   regs[i], 2, pdMS_TO_TICKS(50));
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static void note_event(const char *ev)
{
    strlcpy(s_last_event, ev, sizeof(s_last_event));
    s_last_event_us = esp_timer_get_time();
}

static void vpin_label(uint8_t vp, char kind_suffix, char *ev, size_t n)
{
    if (vp >= VPIN_GPIO_BASE) {
        snprintf(ev, n, "g%u%c", s_map.gpio_num[vp - VPIN_GPIO_BASE], kind_suffix);
    } else {
        snprintf(ev, n, "b%u%c", vp, kind_suffix);
    }
}

static void fire(uint8_t pin, bool long_press)
{
    char ev[8];
    vpin_label(pin, long_press ? 'l' : 's', ev, sizeof(ev));
    note_event(ev);
    for (int i = 0; i < s_map.n_btn; i++) {
        if (s_map.btn[i].pin == pin && s_map.btn[i].long_press == long_press) {
            input_ctl_dispatch(s_map.btn[i].action);
        }
    }
}

/* quadrature full-step decode: +1/-1 per valid transition, a detent = 4 */
static const int8_t k_quad[16] = { 0, -1, 1, 0, 1, 0, 0, -1,
                                   -1, 0, 0, 1, 0, 1, -1, 0 };

static uint32_t frame_read(void)
{
    uint32_t bits = 0xFFFFFFFF;
    if (s_map.addr != 0) {
        uint16_t eb = 0xFFFF;
        if (exp_read(&eb) == ESP_OK) {
            bits = (bits & 0xFFFF0000) | eb;
        }
    }
    for (int i = 0; i < s_map.n_gpio; i++) {
        if (gpio_get_level(s_map.gpio_num[i]) == 0) {
            bits &= ~(1u << (VPIN_GPIO_BASE + i));
        }
    }
    return bits;
}

static void poll_task(void *arg)
{
    (void)arg;
    uint32_t stable = 0xFFFFFFFF, last_raw = 0xFFFFFFFF;
    int same = 0;
    int64_t press_at[32] = { 0 };
    bool long_fired[32] = { false };

    for (int i = 0; i < s_map.n_gpio; i++) {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << s_map.gpio_num[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        gpio_config(&io);
    }
    if (s_map.addr != 0 && !s_map.pcf && mcp_init() != ESP_OK) {
        ESP_LOGW(TAG, "MCP23017 at 0x%02x: init failed, expander disabled", s_map.addr);
        s_map.addr = 0;
    }
    if (s_map.addr != 0) {
        uint16_t probe;
        if (exp_read(&probe) != ESP_OK) {
            ESP_LOGW(TAG, "expander at 0x%02x not answering, disabled", s_map.addr);
            s_map.addr = 0;
        }
    }
    if (s_map.addr == 0 && s_map.n_gpio == 0) {
        vTaskDelete(NULL);
        return;
    }
    uint32_t bits = frame_read();
    stable = last_raw = bits;
    for (int i = 0; i < s_map.n_enc; i++) {
        s_map.enc[i].prev = (uint8_t)((((bits >> s_map.enc[i].pin_a) & 1) << 1) |
                                      ((bits >> s_map.enc[i].pin_b) & 1));
    }
    ESP_LOGI(TAG, "inputs: %s%s%d GPIO pin(s), %d button(s), %d encoder(s)",
             s_map.addr != 0 ? (s_map.pcf ? "PCF8574, " : "MCP23017, ") : "",
             s_map.addr != 0 ? "" : "no expander, ",
             s_map.n_gpio, s_map.n_btn, s_map.n_enc);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        bits = frame_read();

        /* encoders decode on raw samples (fast edges) */
        for (int i = 0; i < s_map.n_enc; i++) {
            enc_map_t *e = &s_map.enc[i];
            uint8_t cur = (uint8_t)((((bits >> e->pin_a) & 1) << 1) |
                                    ((bits >> e->pin_b) & 1));
            if (cur != e->prev) {
                e->accum += k_quad[(e->prev << 2) | cur];
                e->prev = cur;
                if (e->accum >= 4) {
                    e->accum = 0;
                    char ev[8];
                    vpin_label(e->pin_a, '+', ev, sizeof(ev));
                    note_event(ev);
                    input_ctl_dispatch(e->action_cw);
                } else if (e->accum <= -4) {
                    e->accum = 0;
                    char ev[8];
                    vpin_label(e->pin_a, '-', ev, sizeof(ev));
                    note_event(ev);
                    input_ctl_dispatch(e->action_ccw);
                }
            }
        }

        /* buttons debounce on 2 identical samples */
        if (bits == last_raw) {
            if (same < 2) {
                same++;
            }
        } else {
            last_raw = bits;
            same = 0;
            continue;
        }
        if (same != 2 || bits == stable) {
            /* steady state: check pending long presses */
            for (int p = 0; p < 32; p++) {
                if (press_at[p] != 0 && !long_fired[p] &&
                    esp_timer_get_time() - press_at[p] > LONG_PRESS_MS * 1000) {
                    long_fired[p] = true;
                    fire((uint8_t)p, true);
                }
            }
            continue;
        }
        uint32_t changed = stable ^ bits;
        stable = bits;
        for (int p = 0; p < 32; p++) {
            if (!(changed & (1u << p))) {
                continue;
            }
            bool enc_pin = false;
            for (int i = 0; i < s_map.n_enc; i++) {
                if (s_map.enc[i].pin_a == p || s_map.enc[i].pin_b == p) {
                    enc_pin = true;
                }
            }
            if (enc_pin) {
                continue;
            }
            bool pressed = !(bits & (1u << p));   /* active low */
            if (pressed) {
                press_at[p] = esp_timer_get_time();
                long_fired[p] = false;
            } else if (press_at[p] != 0) {
                if (!long_fired[p]) {
                    fire((uint8_t)p, false);
                }
                press_at[p] = 0;
            }
        }
    }
}
#endif /* !APKFLIGHT && !P4 */

void input_ctl_init(void)
{
    parse_map(settings_get()->input_map);
#if !defined(APKFLIGHT) && !CONFIG_IDF_TARGET_ESP32P4
    if (s_map.enabled) {
        xTaskCreate(poll_task, "input_poll", 3072, NULL, 4, NULL);
    }
#endif
}
