#include "web_server.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "mdns.h"

#include "lvgl_port.h"
#include "settings.h"
#include "waveshare_rgb_lcd_port.h"

static const char *TAG = "web";

static char *s_json;
static SemaphoreHandle_t s_mux;

static const char INDEX_HTML[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>esp32flight</title>"
"<link rel='stylesheet' href='https://unpkg.com/leaflet@1.9.4/dist/leaflet.css'>"
"<script src='https://unpkg.com/leaflet@1.9.4/dist/leaflet.js'></script>"
"<style>"
"body{background:#0b0f1a;color:#e8edf5;font-family:system-ui,sans-serif;margin:0;padding:16px}"
"h1{color:#4da3ff;font-size:22px;margin:0 0 4px}"
".dim{color:#8794ad;font-size:14px}"
"table{width:100%;border-collapse:collapse;margin-top:12px;font-size:14px}"
"th{color:#8794ad;text-align:left;padding:6px 8px;border-bottom:1px solid #24314f}"
"td{padding:7px 8px;border-bottom:1px solid #141b2d}"
"tr.em td{background:#4a1010}"
".bar{background:#1a2338;height:6px;border-radius:3px;min-width:70px}"
".bar i{display:block;background:#4da3ff;height:6px;border-radius:3px}"
".cards{display:flex;gap:10px;flex-wrap:wrap;margin-top:10px}"
".card{background:#141b2d;border-radius:10px;padding:10px 14px;font-size:13px;color:#8794ad}"
".card b{display:block;color:#e8edf5;font-size:17px}"
"#map{height:380px;border-radius:12px;margin-top:14px}"
".plane{font-size:20px;line-height:20px;text-shadow:0 0 3px #000}"
"#ota{margin-top:22px;padding-top:12px;border-top:1px solid #24314f}"
"button{background:#4da3ff;color:#06101f;border:0;border-radius:8px;padding:8px 14px;font-weight:600}"
"button:disabled{opacity:.35}"
"a{color:#4da3ff}"
"</style></head><body>"
"<h1>esp32flight</h1><div class='dim' id='hdr'>loading...</div>"
"<div class='cards' id='cards'></div>"
"<div id='map'></div>"
"<table><thead><tr><th>Flight</th><th>Airline</th><th>Aircraft</th><th>Route</th>"
"<th>Progress</th><th>Alt</th><th>Speed</th><th>Dist</th></tr></thead>"
"<tbody id='rows'></tbody></table>"
"<div id='ota'><span class='dim'>Firmware update (.bin): </span>"
"<input type='file' id='fw'> <button id='otabtn' onclick='ota()' disabled>Flash</button> <span id='otastat' class='dim'></span>"
"<div class='dim' style='margin-top:8px'>"
"<a href='/screen.bmp'>screenshot</a> &middot; <a href='/api/state'>api</a> &middot; "
"<a href='https://github.com/theqkash/esp32flight'>github.com/theqkash/esp32flight</a></div></div>"
"<script>"
"let map,layer,homeSet=false;"
"function ensureMap(lat,lon,rkm){if(map||typeof L==='undefined')return;"
"map=L.map('map').setView([lat,lon],8);"
"L.tileLayer('https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png',"
"{attribution:'\\u00A9 OSM \\u00B7 CARTO',maxZoom:12}).addTo(map);"
"L.circle([lat,lon],{radius:rkm*1000,color:'#4da3ff',weight:1,fill:false}).addTo(map);"
"L.circleMarker([lat,lon],{radius:5,color:'#4da3ff',fillOpacity:1}).addTo(map);"
"layer=L.layerGroup().addTo(map);}"
"function drawMap(d){if(!d.lat)return;ensureMap(d.lat,d.lon,d.radius_km||100);"
"if(!layer)return;layer.clearLayers();"
"(d.flights||[]).forEach(f=>{if(f.lat===undefined)return;"
"const ic=L.divIcon({className:'plane',html:`<div style='transform:rotate(${(f.track||0)-45}deg)'>\\u2708</div>`});"
"const m=L.marker([f.lat,f.lon],{icon:ic}).addTo(layer);"
"let t=`<b>${f.callsign}</b><br>${f.airline||''} ${f.type||''}<br>${f.alt_ft} ft \\u00B7 ${f.gs_kt} kt`;"
"if(f.route){t+=`<br>${f.route.from} \\u2192 ${f.route.to} (${f.route.progress}%)`;"
"m.on('click',()=>{L.polyline([[f.route.from_lat,f.route.from_lon],[f.lat,f.lon],[f.route.to_lat,f.route.to_lon]],"
"{color:'#4da3ff',weight:2,dashArray:'4'}).addTo(layer);});}"
"m.bindTooltip(f.callsign,{permanent:false}).bindPopup(t);});}"
"async function load(){try{"
"const r=await fetch('/api/state');const d=await r.json();"
"let w=d.weather?` &nbsp;|&nbsp; ${d.weather.temp_c}\\u00B0C ${d.weather.desc}, wind ${d.weather.wind_kmh} km/h`:'';"
"const n=d.net||{};"
"document.getElementById('hdr').innerHTML=`${d.city||''} (${(+d.lat).toFixed(3)}, ${(+d.lon).toFixed(3)}), radius ${d.radius_km} km${w}`;"
"const s=d.stats||{};document.getElementById('cards').innerHTML="
"`<div class='card'>Network<b>${n.ssid||'-'} ${n.rssi||''} dBm</b>${n.ip||''} \\u00B7 ${n.mdns||''}</div>`+"
"`<div class='card'>Unique aircraft<b>${s.unique_aircraft||0}</b></div>`+"
"`<div class='card'>Max altitude<b>${(s.max_alt_ft||0).toLocaleString()} ft</b></div>`+"
"`<div class='card'>Fastest<b>${s.max_gs_kt||0} kt</b></div>`+"
"`<div class='card'>Farthest<b>${s.max_dist_km||0} km (${s.max_dist_callsign||'-'})</b></div>`+"
"`<div class='card'>Uptime<b>${s.uptime_min||0} min</b></div>`;"
"const ob=document.getElementById('otabtn');ob.disabled=!d.ota_enabled;"
"document.getElementById('otastat').textContent=d.ota_enabled?'':'locked - enable OTA in device settings';"
"drawMap(d);"
"document.getElementById('rows').innerHTML=(d.flights||[]).map(f=>{"
"const em=['7700','7600','7500'].includes(f.squawk);"
"const rt=f.route?`${f.route.from}${f.route.from_time?' '+f.route.from_time:''} \\u2192 ${f.route.to}${f.route.to_time?' '+f.route.to_time:''}<div class='dim'>${f.route.from_city||''}${f.route.from_cc?' ('+f.route.from_cc+')':''} \\u2192 ${f.route.to_city||''}${f.route.to_cc?' ('+f.route.to_cc+')':''}</div>`:'<span class=dim>?</span>';"
"const pr=f.route?`<div class='bar'><i style='width:${f.route.progress}%'></i></div>`:'';"
"return `<tr${em?' class=em':''}><td><b>${f.callsign}</b>${em?' \\u26A0 '+f.squawk:''}</td>"
"<td>${f.airline||'<span class=dim>-</span>'}</td><td>${f.type||''}</td><td>${rt}</td><td>${pr}</td>"
"<td>${f.alt_ft?f.alt_ft.toLocaleString()+' ft':'gnd'}</td><td>${f.gs_kt} kt</td><td>${f.dist_km} km</td></tr>`;"
"}).join('');}catch(e){}}"
"load();setInterval(load,5000);"
"async function ota(){const f=document.getElementById('fw').files[0];if(!f)return;"
"const st=document.getElementById('otastat');st.textContent='uploading...';"
"try{const r=await fetch('/ota',{method:'POST',body:f});"
"st.textContent=await r.text();}catch(e){st.textContent='device rebooting...'}}"
"</script></body></html>";

/* Screenshot: RGB565 framebuffer as a 16bpp BI_BITFIELDS BMP (top-down) */
static esp_err_t screen_get(httpd_req_t *req)
{
    const int W = 800, H = 480;
    const uint32_t data_size = W * H * 2;
    uint8_t *fb = waveshare_lcd_get_fb();
    if (fb == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no framebuffer");
    }

    uint8_t *snap = heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM);
    if (snap == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    if (lvgl_port_lock(3000)) {
        memcpy(snap, fb, data_size);
        lvgl_port_unlock();
    }

    uint8_t hdr[66] = { 0 };
    uint32_t file_size = sizeof(hdr) + data_size;
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(&hdr[2], &file_size, 4);
    uint32_t off = sizeof(hdr);
    memcpy(&hdr[10], &off, 4);
    uint32_t dib = 40;
    memcpy(&hdr[14], &dib, 4);
    int32_t w = W, h = -H;                       /* negative = top-down */
    memcpy(&hdr[18], &w, 4);
    memcpy(&hdr[22], &h, 4);
    hdr[26] = 1;                                 /* planes */
    hdr[28] = 16;                                /* bpp */
    hdr[30] = 3;                                 /* BI_BITFIELDS */
    memcpy(&hdr[34], &data_size, 4);
    uint32_t masks[3] = { 0xF800, 0x07E0, 0x001F };
    memcpy(&hdr[54], masks, 12);

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_send_chunk(req, (char *)hdr, sizeof(hdr));
    for (uint32_t sent = 0; sent < data_size; sent += 64 * 1024) {
        uint32_t n = data_size - sent < 64 * 1024 ? data_size - sent : 64 * 1024;
        if (httpd_resp_send_chunk(req, (char *)snap + sent, n) != ESP_OK) {
            break;
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);
    free(snap);
    return ESP_OK;
}

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_get(httpd_req_t *req)
{
    char *copy = NULL;
    size_t len = 0;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_json != NULL) {
        len = strlen(s_json);
        copy = malloc(len + 32);
        if (copy != NULL) {
            memcpy(copy, s_json, len + 1);
        }
    }
    xSemaphoreGive(s_mux);

    /* Inject the volatile OTA state at request time so the panel always
     * sees the switch's current position, not the cached snapshot. */
    if (copy != NULL && len > 1 && copy[len - 1] == '}') {
        snprintf(copy + len - 1, 32, ",\"ota_enabled\":%s}",
                 settings_get()->ota_enabled ? "true" : "false");
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, copy ? copy : "{\"ota_enabled\":false}",
                                    HTTPD_RESP_USE_STRLEN);
    free(copy);
    return err;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!settings_get()->ota_enabled) {
        ESP_LOGW(TAG, "OTA rejected: updates locked");
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                                   "OTA locked - enable updates in the device settings");
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
    }
    ESP_LOGI(TAG, "OTA start: %d bytes -> %s", req->content_len, part->label);

    esp_ota_handle_t ota;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin failed");
    }

    char *buf = malloc(4096);
    int remaining = req->content_len;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < 4096 ? remaining : 4096);
        if (n <= 0) {
            free(buf);
            esp_ota_abort(ota);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        }
        if (esp_ota_write(ota, buf, n) != ESP_OK) {
            free(buf);
            esp_ota_abort(ota);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        }
        remaining -= n;
    }
    free(buf);

    if (esp_ota_end(ota) != ESP_OK ||
        esp_ota_set_boot_partition(part) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "validate failed");
    }
    httpd_resp_sendstr(req, "OK - rebooting");
    ESP_LOGI(TAG, "OTA done, restarting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

void web_state_publish(const char *json)
{
    if (s_mux == NULL) {
        return;
    }
    size_t len = strlen(json) + 1;
    char *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (copy == NULL) {
        return;
    }
    memcpy(copy, json, len);
    xSemaphoreTake(s_mux, portMAX_DELAY);
    free(s_json);
    s_json = copy;
    xSemaphoreGive(s_mux);
}

void web_server_start(void)
{
    s_mux = xSemaphoreCreateMutex();

    mdns_init();
    mdns_hostname_set("esp32flight");
    mdns_instance_name_set("esp32flight flight tracker");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_get },
        { .uri = "/api/state", .method = HTTP_GET, .handler = api_get },
        { .uri = "/screen.bmp", .method = HTTP_GET, .handler = screen_get },
        { .uri = "/ota", .method = HTTP_POST, .handler = ota_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    ESP_LOGI(TAG, "web panel at http://esp32flight.local/");
}
