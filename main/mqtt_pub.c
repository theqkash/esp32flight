#include "mqtt_pub.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "settings.h"
#ifndef APKFLIGHT
#include "waveshare_rgb_lcd_port.h"
#include "ui.h"
#include "input_ctl.h"
#endif

static const char *TAG = "mqtt";

#define STATE_TOPIC     "esp32flight/state"
#define BL_CONFIG_TOPIC "homeassistant/light/esp32flight_backlight/config"
#define BL_STATE_TOPIC  "esp32flight/backlight/state"
#define BL_BRI_TOPIC    "esp32flight/backlight/brightness"
#define BL_SET_TOPIC    "esp32flight/backlight/set"
#define BL_BRI_SET      "esp32flight/backlight/brightness/set"
/* Screen on/off switch and a screensaver button: work on every board,
 * independent of the brightness master switch */
#define SCR_CONFIG_TOPIC "homeassistant/switch/esp32flight_screen/config"
#define SCR_STATE_TOPIC  "esp32flight/screen/state"
#define SCR_SET_TOPIC    "esp32flight/screen/set"
#define SAVER_CONFIG_TOPIC "homeassistant/button/esp32flight_screensaver/config"
#define SAVER_SET_TOPIC  "esp32flight/screensaver/set"

static esp_mqtt_client_handle_t s_client;
static bool s_connected;
static bool s_light_on = true;
static int s_scr_published = -1;   /* last screen state sent, -1 = none */

#ifndef APKFLIGHT
/* HA brightness sliders stream messages while dragged; batch the NVS write */
static esp_timer_handle_t s_bl_save;

static void bl_save_cb(void *arg)
{
    (void)arg;
    settings_save();
}

static void bl_save_later(void)
{
    if (s_bl_save == NULL) {
        const esp_timer_create_args_t a = { .callback = bl_save_cb, .name = "mqttbl" };
        if (esp_timer_create(&a, &s_bl_save) != ESP_OK) {
            return;
        }
    }
    esp_timer_stop(s_bl_save);
    esp_timer_start_once(s_bl_save, 3 * 1000 * 1000);
}

static bool bl_entity_active(void)
{
    return settings_get()->brightness_ctl && waveshare_rgb_lcd_bl_dimmable();
}

static void publish_bl_state(void)
{
    if (!bl_entity_active()) {
        return;
    }
    char bri[8];
    snprintf(bri, sizeof(bri), "%u", settings_get()->brightness);
    esp_mqtt_client_publish(s_client, BL_STATE_TOPIC, s_light_on ? "ON" : "OFF", 0, 1, 1);
    esp_mqtt_client_publish(s_client, BL_BRI_TOPIC, bri, 0, 1, 1);
}
#endif

static void publish_discovery(void)
{
    static const struct {
        const char *object_id, *name, *tpl, *unit;
    } sensors[] = {
        { "nearest",  "Nearest aircraft", "{{ value_json.nearest }}", NULL },
        { "count",    "Aircraft in range", "{{ value_json.count }}", "aircraft" },
        { "unique",   "Unique aircraft (session)", "{{ value_json.unique }}", "aircraft" },
        { "nearest_dist", "Nearest distance", "{{ value_json.nearest_km }}", "km" },
        { "fa_calls", "FlightAware calls this month", "{{ value_json.fa_calls }}", "calls" },
        { "fa_cost", "FlightAware cost this month (estimate)", "{{ value_json.fa_cost }}", "USD" },
    };
    const char *model =
#ifndef APKFLIGHT
        waveshare_lcd_board_name();
#else
        "Android";
#endif
    for (size_t i = 0; i < sizeof(sensors) / sizeof(sensors[0]); i++) {
        char topic[96], payload[512];
        snprintf(topic, sizeof(topic),
                 "homeassistant/sensor/esp32flight_%s/config", sensors[i].object_id);
        snprintf(payload, sizeof(payload),
                 "{\"name\":\"%s\",\"state_topic\":\"" STATE_TOPIC "\","
                 "\"value_template\":\"%s\","
                 "%s%s%s"
                 "\"unique_id\":\"esp32flight_%s\","
                 "\"device\":{\"identifiers\":[\"esp32flight\"],"
                 "\"name\":\"esp32flight\",\"manufacturer\":\"theqkash\","
                 "\"model\":\"%s\"}}",
                 sensors[i].name, sensors[i].tpl,
                 sensors[i].unit ? "\"unit_of_measurement\":\"" : "",
                 sensors[i].unit ? sensors[i].unit : "",
                 sensors[i].unit ? "\"," : "",
                 sensors[i].object_id, model);
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);
    }
#ifndef APKFLIGHT
    /* Backlight as an HA light with brightness (lim3ra's request). Only
     * when the master brightness switch is on and the panel can dim; an
     * empty retained payload removes a previously discovered entity. */
    if (bl_entity_active()) {
        char payload[560];
        snprintf(payload, sizeof(payload),
                 "{\"name\":\"Screen backlight\","
                 "\"command_topic\":\"" BL_SET_TOPIC "\","
                 "\"state_topic\":\"" BL_STATE_TOPIC "\","
                 "\"brightness_command_topic\":\"" BL_BRI_SET "\","
                 "\"brightness_state_topic\":\"" BL_BRI_TOPIC "\","
                 "\"brightness_scale\":100,"
                 "\"unique_id\":\"esp32flight_backlight\","
                 "\"device\":{\"identifiers\":[\"esp32flight\"],"
                 "\"name\":\"esp32flight\",\"manufacturer\":\"theqkash\","
                 "\"model\":\"%s\"}}", model);
        esp_mqtt_client_publish(s_client, BL_CONFIG_TOPIC, payload, 0, 1, 1);
        esp_mqtt_client_subscribe(s_client, BL_SET_TOPIC, 0);
        esp_mqtt_client_subscribe(s_client, BL_BRI_SET, 0);
        publish_bl_state();
    } else {
        esp_mqtt_client_publish(s_client, BL_CONFIG_TOPIC, "", 0, 1, 1);
    }
    {
        char payload[480];
        snprintf(payload, sizeof(payload),
                 "{\"name\":\"Screen\",\"icon\":\"mdi:monitor\","
                 "\"command_topic\":\"" SCR_SET_TOPIC "\","
                 "\"state_topic\":\"" SCR_STATE_TOPIC "\","
                 "\"unique_id\":\"esp32flight_screen\","
                 "\"device\":{\"identifiers\":[\"esp32flight\"],"
                 "\"name\":\"esp32flight\",\"manufacturer\":\"theqkash\","
                 "\"model\":\"%s\"}}", model);
        esp_mqtt_client_publish(s_client, SCR_CONFIG_TOPIC, payload, 0, 1, 1);
        snprintf(payload, sizeof(payload),
                 "{\"name\":\"Start screensaver\",\"icon\":\"mdi:radar\","
                 "\"command_topic\":\"" SAVER_SET_TOPIC "\","
                 "\"unique_id\":\"esp32flight_screensaver\","
                 "\"device\":{\"identifiers\":[\"esp32flight\"],"
                 "\"name\":\"esp32flight\",\"manufacturer\":\"theqkash\","
                 "\"model\":\"%s\"}}", model);
        esp_mqtt_client_publish(s_client, SAVER_CONFIG_TOPIC, payload, 0, 1, 1);
        esp_mqtt_client_subscribe(s_client, SCR_SET_TOPIC, 0);
        esp_mqtt_client_subscribe(s_client, SAVER_SET_TOPIC, 0);
        s_scr_published = -1;   /* resend state after (re)connect */
    }
#endif
}

#ifndef APKFLIGHT
static void publish_screen_state(void)
{
    int dark = ui_screen_dark() ? 1 : 0;
    if (dark != s_scr_published) {
        s_scr_published = dark;
        esp_mqtt_client_publish(s_client, SCR_STATE_TOPIC, dark ? "OFF" : "ON", 0, 1, 1);
    }
}

static bool topic_is(const char *topic, size_t tlen, const char *want)
{
    return tlen == strlen(want) && strncmp(topic, want, tlen) == 0;
}

static void handle_bl_command(const char *topic, size_t tlen,
                              const char *data, size_t dlen)
{
    if (topic_is(topic, tlen, SAVER_SET_TOPIC)) {
        input_ctl_dispatch("screensaver");
        return;
    }
    if (topic_is(topic, tlen, SCR_SET_TOPIC)) {
        bool on = dlen >= 2 && strncmp(data, "ON", 2) == 0;
        input_ctl_dispatch(on ? "screen_on" : "screen_off");
        publish_screen_state();
        return;
    }
    if (!bl_entity_active()) {
        return;
    }
    char val[16];
    if (dlen >= sizeof(val)) {
        dlen = sizeof(val) - 1;
    }
    memcpy(val, data, dlen);
    val[dlen] = '\0';
    if (tlen == strlen(BL_SET_TOPIC) && strncmp(topic, BL_SET_TOPIC, tlen) == 0) {
        if (strcmp(val, "ON") == 0) {
            s_light_on = true;
            waveshare_rgb_lcd_bl_on();
            ui_apply_brightness();
        } else if (strcmp(val, "OFF") == 0) {
            s_light_on = false;
            waveshare_rgb_lcd_bl_off();
        }
        publish_bl_state();
        return;
    }
    if (tlen == strlen(BL_BRI_SET) && strncmp(topic, BL_BRI_SET, tlen) == 0) {
        int b = atoi(val);
        if (b < 5) b = 5;
        if (b > 100) b = 100;
        settings_get()->brightness = (uint8_t)b;
        if (s_light_on) {
            waveshare_rgb_lcd_bl_pct(b);
        }
        bl_save_later();
        publish_bl_state();
    }
}
#endif

static void mqtt_event(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    if (event_id == MQTT_EVENT_CONNECTED) {
        s_connected = true;
        ESP_LOGI(TAG, "connected");
        publish_discovery();
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        s_connected = false;
#ifndef APKFLIGHT
    } else if (event_id == MQTT_EVENT_DATA) {
        esp_mqtt_event_handle_t ev = data;
        handle_bl_command(ev->topic, (size_t)ev->topic_len,
                          ev->data, (size_t)ev->data_len);
#endif
    }
}

void mqtt_pub_start(void)
{
    const char *uri = settings_get()->mqtt_uri;
    if (uri[0] == '\0') {
        return;
    }
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .task.priority = 4,
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "bad broker uri");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    esp_mqtt_client_start(s_client);
    /* never print credentials (user:pass@) into a log people share (#35) */
    const char *at = strchr(uri, '@');
    const char *sch = strstr(uri, "://");
    if (at != NULL && sch != NULL && sch < at) {
        ESP_LOGI(TAG, "connecting to %.*s***@%s", (int)(sch + 3 - uri), uri, at + 1);
    } else {
        ESP_LOGI(TAG, "connecting to %s", uri);
    }
}

void mqtt_pub_backlight_changed(void)
{
#ifndef APKFLIGHT
    if (s_client != NULL && s_connected) {
        publish_bl_state();
    }
#endif
}

void mqtt_pub_state(const char *json)
{
    if (s_client == NULL || !s_connected) {
        return;
    }
    esp_mqtt_client_publish(s_client, STATE_TOPIC, json, 0, 0, 0);
#ifndef APKFLIGHT
    publish_screen_state();   /* night schedule and taps change it too */
#endif
}
