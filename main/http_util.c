#include "http_util.h"

#include <string.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

static const char *TAG = "http";

typedef struct {
    char   *buf;
    size_t  cap;
    size_t  len;
    bool    overflow;
} sink_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    sink_t *s = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && s != NULL) {
        size_t space = s->cap - 1 - s->len;
        size_t n = evt->data_len;
        if (n > space) {
            n = space;
            s->overflow = true;
        }
        memcpy(s->buf + s->len, evt->data, n);
        s->len += n;
    }
    return ESP_OK;
}

esp_err_t http_get_to_buffer(const char *url, char *buf, size_t buf_size, size_t *out_len)
{
    return http_get_to_buffer_hdr(url, buf, buf_size, out_len, NULL, NULL);
}

esp_err_t http_get_to_buffer_hdr(const char *url, char *buf, size_t buf_size, size_t *out_len,
                                 const char *hdr_key, const char *hdr_val)
{
    sink_t sink = { .buf = buf, .cap = buf_size, .len = 0, .overflow = false };

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_cb,
        .user_data = &sink,
        .timeout_ms = 12000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
        .user_agent = "canflight-esp32/1.0",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }
    if (hdr_key != NULL && hdr_val != NULL) {
        esp_http_client_set_header(client, hdr_key, hdr_val);
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    buf[sink.len] = '\0';
    if (out_len != NULL) {
        *out_len = sink.len;
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GET %s failed: %s", url, esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "GET %s -> HTTP %d", url, status);
        return ESP_ERR_HTTP_BASE + status;
    }
    if (sink.overflow) {
        ESP_LOGW(TAG, "GET %s: response truncated at %u bytes", url, (unsigned)sink.len);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t http_post_text(const char *url, const char *body,
                         const char *hdr_key, const char *hdr_val)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "esp32flight/1.0",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }
    if (hdr_key != NULL && hdr_val != NULL) {
        esp_http_client_set_header(client, hdr_key, hdr_val);
    }
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err == ESP_OK && (status < 200 || status >= 300)) {
        err = ESP_FAIL;
    }
    return err;
}
