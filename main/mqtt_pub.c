#include "mqtt_pub.h"

#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "mqtt_client.h"
#include "settings.h"

static const char *TAG = "mqtt";

#define STATE_TOPIC "esp32flight/state"

static esp_mqtt_client_handle_t s_client;
static bool s_connected;

static void publish_discovery(void)
{
    static const struct {
        const char *object_id, *name, *tpl, *unit;
    } sensors[] = {
        { "nearest",  "Nearest aircraft", "{{ value_json.nearest }}", NULL },
        { "count",    "Aircraft in range", "{{ value_json.count }}", "aircraft" },
        { "unique",   "Unique aircraft (session)", "{{ value_json.unique }}", "aircraft" },
        { "nearest_dist", "Nearest distance", "{{ value_json.nearest_km }}", "km" },
    };
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
                 "\"model\":\"ESP32-S3-Touch-LCD-7\"}}",
                 sensors[i].name, sensors[i].tpl,
                 sensors[i].unit ? "\"unit_of_measurement\":\"" : "",
                 sensors[i].unit ? sensors[i].unit : "",
                 sensors[i].unit ? "\"," : "",
                 sensors[i].object_id);
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);
    }
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    if (event_id == MQTT_EVENT_CONNECTED) {
        s_connected = true;
        ESP_LOGI(TAG, "connected");
        publish_discovery();
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        s_connected = false;
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
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "bad broker uri");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "connecting to %s", uri);
}

void mqtt_pub_state(const char *json)
{
    if (s_client == NULL || !s_connected) {
        return;
    }
    esp_mqtt_client_publish(s_client, STATE_TOPIC, json, 0, 0, 0);
}
