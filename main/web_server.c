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
#include "esp_timer.h"
#include "esp_system.h"
#include "mdns.h"

#include "cJSON.h"
#include "lvgl_port.h"
#include "obslog.h"
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
".sect{margin-top:22px;padding-top:12px;border-top:1px solid #24314f}"
".sect h3{margin:0 0 10px;font-size:16px;color:#e8edf5}"
".grid2{display:grid;grid-template-columns:repeat(auto-fill,minmax(230px,1fr));gap:10px}"
".grid2 label{font-size:12px;color:#8794ad;display:block;margin-bottom:2px}"
"input,select{width:100%;box-sizing:border-box;background:#141b2d;border:1px solid #24314f;color:#e8edf5;border-radius:6px;padding:7px}"
"#hq{width:200px;display:inline-block}"
"tr.gold td b{color:#ffd166}"
".sech{grid-column:1/-1;color:#4da3ff;font-size:12px;font-weight:700;margin-top:10px;text-transform:uppercase;letter-spacing:.6px}"
".help{font-size:12px;color:#66738c;margin-top:3px;line-height:1.4}"
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
"<a href='/screen.bmp'>screenshot</a> &middot; <a href='/api/state'>api</a> &middot; <a href='/api/log' download='esp32flight-log.tsv'>log CSV</a> &middot; <a href='/metrics'>metrics</a> &middot; "
"<a href='https://github.com/theqkash/esp32flight'>github.com/theqkash/esp32flight</a></div></div>"
"<div class='sect'><h3>Device settings</h3><div class='grid2'>"
"<div class='sech'>Network</div>"
"<div><label>Wi-Fi SSID</label><input id='c_ssid'>"
"<div class='help'>2.4 GHz networks only (ESP32 has no 5 GHz radio).</div></div>"
"<div><label>Wi-Fi password</label><input id='c_pass' type='password'>"
"<div class='help'>Leave empty to keep the current password.</div></div>"
"<div class='sech'>Location and filters</div>"
"<div><label>Location mode</label><select id='c_fixed'><option value='0'>auto (IP geolocation)</option><option value='1'>fixed coordinates</option></select>"
"<div class='help'>Auto locates the device by its internet address at boot. Pick fixed to watch a different area.</div></div>"
"<div><label>Latitude</label><input id='c_lat' type='number' step='0.0001'></div>"
"<div><label>Longitude</label><input id='c_lon' type='number' step='0.0001'></div>"
"<div><label>Radius (nautical miles, 5-250)</label><input id='c_radius_nm' type='number'>"
"<div class='help'>Search radius around the location. 54 nm is about 100 km.</div></div>"
"<div><label>Hide ground traffic</label><select id='c_hide_ground'><option value='0'>no</option><option value='1'>yes</option></select>"
"<div class='help'>Hides aircraft taxiing or parked at nearby airports.</div></div>"
"<div><label>Airline flights only</label><select id='c_airline_only'><option value='0'>no</option><option value='1'>yes</option></select>"
"<div class='help'>Hides private planes, air taxis and helicopters. Keeps scheduled airline traffic.</div></div>"
"<div><label>Watchlist</label><input id='c_watch_regs' placeholder='SP-LR,RCH,A388'>"
"<div class='help'>Comma-separated registration or callsign prefixes to highlight in gold and push-notify. Military and famous heavies (A380, AN-124, C-17...) are always highlighted.</div></div>"
"<div class='sech'>Display</div>"
"<div><label>Theme</label><select id='c_theme'><option value='0'>Dark</option><option value='1'>Light</option><option value='2'>Black</option><option value='3'>Nord</option><option value='4'>Solarized</option><option value='5'>Purple</option><option value='6'>Forest</option></select></div>"
"<div><label>Language</label><select id='c_lang'><option value='0'>English</option><option value='1'>Polski</option></select></div>"
"<div><label>Map screensaver after (minutes, 0 = off)</label><input id='c_ambient_idle_min' type='number'>"
"<div class='help'>After this many idle minutes the device shows a full-screen map with all nearby flights, clock and weather. Tap to return.</div></div>"
"<div><label>Night mode</label><select id='c_night_enabled'><option value='0'>off</option><option value='1'>on</option></select>"
"<div class='help'>During the quiet hours below the backlight turns off after a few extra idle minutes. First tap wakes the screen.</div></div>"
"<div><label>Night from</label><input id='c_night_start' type='time'></div>"
"<div><label>Night until</label><input id='c_night_end' type='time'></div>"
"<div class='sech'>Notifications</div>"
"<div><label>ntfy.sh topic (push to your phone)</label><input id='c_ntfy_topic' placeholder='my-secret-esp32flight-8341'>"
"<div class='help'>Free push notifications, no account needed. 1) Install the ntfy app (ntfy.sh). 2) Invent a unique topic name and subscribe to it in the app. 3) Enter the same name here. You will get alerts for emergency squawks (7700), watchlist aircraft and predicted flyovers.</div></div>"
"<div><label>Flyover (CPA) alerts</label><select id='c_cpa_alerts'><option value='0'>off</option><option value='1'>on</option></select>"
"<div class='help'>Predicts when an interesting aircraft will pass within 5 km of you and notifies minutes before it happens, so you can step outside in time.</div></div>"
"<div><label>Webhook URL</label><input id='c_webhook_url' placeholder='https://n8n.example.com/hook/abc'>"
"<div class='help'>On every alert the device POSTs JSON {source, title, message} to this address. Works with Node-RED, n8n, Discord/Slack bridges and similar.</div></div>"
"<div class='sech'>Integrations</div>"
"<div><label>MQTT broker (Home Assistant)</label><input id='c_mqtt_uri' placeholder='mqtt://user:pass@192.168.1.10:1883'>"
"<div class='help'>Connects to your MQTT broker. In Home Assistant the sensors appear automatically via MQTT discovery: nearest aircraft, aircraft count, session unique count and nearest distance.</div></div>"
"<div><label>FlightAware AeroAPI key</label><input id='c_fa_key'>"
"<div class='help'>Optional. Adds commercial flight numbers (FR4238) next to radio callsigns (RYR638T). Create a free Personal key at flightaware.com/aeroapi; the free monthly credit is plenty because results are cached.</div></div>"
"<div><label>Local ADS-B receiver (dump1090/readsb)</label><input id='c_local_adsb' placeholder='http://192.168.1.50:8080/data/aircraft.json'>"
"<div class='help'>If you run your own receiver (e.g. RTL-SDR on a Raspberry Pi), point this at its aircraft.json. The device then uses your antenna instead of internet APIs: faster updates, no limits, works without the cloud. Falls back to the internet automatically when unreachable.</div></div>"
"</div><div style='margin-top:14px'><button id='cfgsave' onclick='saveCfg()' disabled>Save and restart</button> <span id='cfgstat' class='dim'></span></div></div>"
"<div class='sect'><h3>Spotting history</h3>"
"<input id='hq' placeholder='filter (callsign, type...)'> <button onclick='loadHist()'>Load</button>"
"<table><tbody id='hrows'></tbody></table></div>"
"<script>"
"let map,layer,homeSet=false,routeLine=null,selHex=null;"
"function ensureMap(lat,lon,rkm){if(map||typeof L==='undefined')return;"
"map=L.map('map').setView([lat,lon],8);"
"L.tileLayer('https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png',"
"{attribution:'\\u00A9 OSM \\u00B7 CARTO',maxZoom:12}).addTo(map);"
"L.circle([lat,lon],{radius:rkm*1000,color:'#4da3ff',weight:1,fill:false}).addTo(map);"
"L.circleMarker([lat,lon],{radius:5,color:'#4da3ff',fillOpacity:1}).addTo(map);"
"map.on('click',()=>{if(routeLine){map.removeLayer(routeLine);routeLine=null;selHex=null;}});"
"layer=L.layerGroup().addTo(map);}"
"function showRoute(f){if(routeLine)map.removeLayer(routeLine);"
"routeLine=L.polyline([[f.route.from_lat,f.route.from_lon],[f.lat,f.lon],[f.route.to_lat,f.route.to_lon]],"
"{color:'#4da3ff',weight:2,dashArray:'4'}).addTo(map);selHex=f.hex;}"
"function drawMap(d){if(!d.lat)return;ensureMap(d.lat,d.lon,d.radius_km||100);"
"if(!layer)return;layer.clearLayers();"
"(d.flights||[]).forEach(f=>{if(f.lat===undefined)return;"
"const ic=L.divIcon({className:'plane',html:`<div style='transform:rotate(${(f.track||0)-45}deg)'>\\u2708</div>`});"
"if(f.trail&&f.trail.length>1)L.polyline(f.trail,{color:'#ffd166',weight:1,opacity:.5}).addTo(layer);"
"const m=L.marker([f.lat,f.lon],{icon:ic}).addTo(layer);"
"let t=`<b>${f.callsign}</b><br>${f.airline||''} ${f.type||''}<br>${f.alt_ft} ft \\u00B7 ${f.gs_kt} kt`;"
"if(f.route){t+=`<br>${f.route.from} \\u2192 ${f.route.to} (${f.route.progress}%)`;"
"m.on('click',()=>showRoute(f));"
"if(f.hex===selHex)showRoute(f);}"
"m.bindTooltip(f.callsign,{permanent:false}).bindPopup(t);});}"
"async function load(){try{"
"const r=await fetch('/api/state');const d=await r.json();"
"let w=d.weather?` &nbsp;|&nbsp; ${d.weather.temp_c}\\u00B0C ${d.weather.desc}, wind ${d.weather.wind_kmh} km/h`:'';"
"const n=d.net||{};"
"document.getElementById('hdr').innerHTML=`${d.city||''} (${(+d.lat).toFixed(3)}, ${(+d.lon).toFixed(3)}), radius ${d.radius_km} km${w}`;+((d.stats&&d.stats.metar)?`<br>${d.stats.metar}`:'');"
"const s=d.stats||{};document.getElementById('cards').innerHTML="
"`<div class='card'>Network<b>${n.ssid||'-'} ${n.rssi||''} dBm</b>${n.ip||''} \\u00B7 ${n.mdns||''}</div>`+"
"`<div class='card'>Unique aircraft<b>${s.unique_aircraft||0}</b></div>`+"
"`<div class='card'>Max altitude<b>${(s.max_alt_ft||0).toLocaleString()} ft</b></div>`+"
"`<div class='card'>Fastest<b>${s.max_gs_kt||0} kt</b></div>`+"
"`<div class='card'>Farthest<b>${s.max_dist_km||0} km (${s.max_dist_callsign||'-'})</b></div>`+"
"`<div class='card'>Uptime<b>${s.uptime_min||0} min</b></div>`+"
"((s.days&&s.days.length)?`<div class='card'>Last days<b style='display:flex;align-items:flex-end;gap:2px;height:28px'>${s.days.slice(-14).map(dd=>`<i title='${dd.d}: ${dd.u}' style='display:block;width:8px;background:#4da3ff;height:${Math.max(2,Math.round(28*dd.u/Math.max(...s.days.map(x=>x.u),1)))}px'></i>`).join('')}</b></div>`:'');"
"const ob=document.getElementById('otabtn');ob.disabled=!d.ota_enabled;"
"document.getElementById('otastat').textContent=d.ota_enabled?'':'locked - enable OTA in device settings';"
"drawMap(d);"
"document.getElementById('rows').innerHTML=(d.flights||[]).map(f=>{"
"const em=['7700','7600','7500'].includes(f.squawk);"
"const rt=f.route?`${f.route.from}${f.route.from_time?' '+f.route.from_time:''} \\u2192 ${f.route.to}${f.route.to_time?' '+f.route.to_time:''}<div class='dim'>${f.route.from_city||''}${f.route.from_cc?' ('+f.route.from_cc+')':''} \\u2192 ${f.route.to_city||''}${f.route.to_cc?' ('+f.route.to_cc+')':''}</div>`:'<span class=dim>?</span>';"
"const pr=f.route?`<div class='bar'><i style='width:${f.route.progress}%'></i></div>`:'';"
"const flag=f.cc?String.fromCodePoint(...[...f.cc].map(ch=>127397+ch.charCodeAt(0)))+' ':'';"
"return `<tr${em?' class=em':(f.interesting?' class=gold':'')}><td>${flag}<b>${f.callsign}</b>${f.flight_iata?` <span class=dim>${f.flight_iata}</span>`:''}${em?' \\u26A0 '+f.squawk:''}</td>"
"<td>${f.airline||'<span class=dim>-</span>'}</td><td>${f.type||''}</td><td>${rt}</td><td>${pr}</td>"
"<td>${f.alt_ft?f.alt_ft.toLocaleString()+' ft':'gnd'}</td><td>${f.gs_kt} kt</td><td>${f.dist_km} km</td></tr>`;"
"}).join('');}catch(e){}}"
"async function loadCfg(){try{const r=await fetch('/api/config');const c=await r.json();"
"for(const k in c){const el=document.getElementById('c_'+k);if(!el)continue;"
"if(el.tagName==='SELECT')el.value=(c[k]===true||c[k]===1||c[k]==='1')?1:( +c[k]||0);else el.value=c[k];}"
"document.getElementById('c_theme').value=c.theme;document.getElementById('c_lang').value=c.lang;"
"const mm=v=>`${String(Math.floor(v/60)).padStart(2,'0')}:${String(v%60).padStart(2,'0')}`;"
"document.getElementById('c_night_start').value=mm(c.night_start_min||1380);"
"document.getElementById('c_night_end').value=mm(c.night_end_min||390);"
"document.getElementById('cfgsave').disabled=false;}catch(e){}}"
"async function saveCfg(){const c={};"
"['ssid','pass','ntfy_topic','mqtt_uri','fa_key','watch_regs','webhook_url','local_adsb'].forEach(k=>{const v=document.getElementById('c_'+k).value;if(k!=='pass'||v)c[k]=v;});"
"c.cpa_alerts=document.getElementById('c_cpa_alerts').value==='1';"
"c.night_enabled=document.getElementById('c_night_enabled').value==='1';"
"c.ambient_idle_min=+document.getElementById('c_ambient_idle_min').value;"
"const pm=id=>{const v=document.getElementById(id).value.split(':');return (+v[0])*60+(+v[1]||0);};"
"c.night_start_min=pm('c_night_start');c.night_end_min=pm('c_night_end');"
"c.fixed=document.getElementById('c_fixed').value==='1';"
"c.hide_ground=document.getElementById('c_hide_ground').value==='1';"
"c.airline_only=document.getElementById('c_airline_only').value==='1';"
"c.lat=+document.getElementById('c_lat').value;c.lon=+document.getElementById('c_lon').value;"
"c.radius_nm=+document.getElementById('c_radius_nm').value;"
"c.theme=+document.getElementById('c_theme').value;c.lang=+document.getElementById('c_lang').value;"
"const st=document.getElementById('cfgstat');st.textContent='saving...';"
"try{const r=await fetch('/api/config',{method:'POST',body:JSON.stringify(c)});st.textContent=await r.text();}"
"catch(e){st.textContent='device restarting...'}}"
"async function loadHist(){const r=await fetch('/api/log');const t=await r.text();"
"const q=document.getElementById('hq').value.toLowerCase();"
"document.getElementById('hrows').innerHTML=t.trim().split('\\n').reverse()"
".filter(l=>l&&l.toLowerCase().includes(q)).slice(0,300).map(l=>{const p=l.split('\t');"
"return `<tr><td class=dim>${new Date(+p[0]*1000).toLocaleString()}</td><td><b>${p[2]||''}</b></td>"
"<td class=dim>${p[1]||''}</td><td>${p[3]||''}</td></tr>`;}).join('')||'<tr><td class=dim>empty</td></tr>';}"
"load();loadCfg();setInterval(load,5000);"
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

static esp_err_t config_get(httpd_req_t *req)
{
    const settings_t *c = settings_get();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", c->wifi_ssid);
    cJSON_AddBoolToObject(root, "fixed", c->use_fixed_loc);
    cJSON_AddNumberToObject(root, "lat", c->lat);
    cJSON_AddNumberToObject(root, "lon", c->lon);
    cJSON_AddNumberToObject(root, "radius_nm", c->radius_nm);
    cJSON_AddBoolToObject(root, "hide_ground", c->hide_ground);
    cJSON_AddBoolToObject(root, "airline_only", c->hide_private);
    cJSON_AddNumberToObject(root, "theme", c->theme);
    cJSON_AddNumberToObject(root, "lang", c->lang);
    cJSON_AddStringToObject(root, "ntfy_topic", c->ntfy_topic);
    cJSON_AddStringToObject(root, "mqtt_uri", c->mqtt_uri);
    cJSON_AddStringToObject(root, "fa_key", c->fa_key);
    cJSON_AddStringToObject(root, "watch_regs", c->watch_regs);
    cJSON_AddStringToObject(root, "webhook_url", c->webhook_url);
    cJSON_AddStringToObject(root, "local_adsb", c->local_adsb);
    cJSON_AddBoolToObject(root, "cpa_alerts", c->cpa_alerts);
    cJSON_AddBoolToObject(root, "night_enabled", c->night_enabled);
    cJSON_AddNumberToObject(root, "night_start_min", c->night_start_min);
    cJSON_AddNumberToObject(root, "night_end_min", c->night_end_min);
    cJSON_AddNumberToObject(root, "ambient_idle_min", c->ambient_idle_min);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, json ? json : "{}", HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

static void set_str_field(const cJSON *root, const char *key, char *dst, size_t n)
{
    const cJSON *j = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(j)) {
        strlcpy(dst, j->valuestring, n);
    }
}

static esp_err_t config_post(httpd_req_t *req)
{
    char body[1024];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    body[len] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }

    settings_t *c = settings_get();
    set_str_field(root, "ssid", c->wifi_ssid, sizeof(c->wifi_ssid));
    set_str_field(root, "pass", c->wifi_pass, sizeof(c->wifi_pass));
    set_str_field(root, "ntfy_topic", c->ntfy_topic, sizeof(c->ntfy_topic));
    set_str_field(root, "mqtt_uri", c->mqtt_uri, sizeof(c->mqtt_uri));
    set_str_field(root, "fa_key", c->fa_key, sizeof(c->fa_key));
    set_str_field(root, "watch_regs", c->watch_regs, sizeof(c->watch_regs));
    set_str_field(root, "webhook_url", c->webhook_url, sizeof(c->webhook_url));
    set_str_field(root, "local_adsb", c->local_adsb, sizeof(c->local_adsb));
    const cJSON *j;
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "fixed")))) {
        c->use_fixed_loc = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "hide_ground")))) {
        c->hide_ground = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "airline_only")))) {
        c->hide_private = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "cpa_alerts")))) {
        c->cpa_alerts = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "night_enabled")))) {
        c->night_enabled = cJSON_IsTrue(j);
    }
    if (cJSON_IsNumber((j = cJSON_GetObjectItem(root, "night_start_min")))) {
        c->night_start_min = (int)j->valuedouble;
    }
    if (cJSON_IsNumber((j = cJSON_GetObjectItem(root, "night_end_min")))) {
        c->night_end_min = (int)j->valuedouble;
    }
    if (cJSON_IsNumber((j = cJSON_GetObjectItem(root, "ambient_idle_min")))) {
        c->ambient_idle_min = (int)j->valuedouble;
    }
    if (cJSON_IsNumber((j = cJSON_GetObjectItem(root, "lat")))) {
        c->lat = j->valuedouble;
    }
    if (cJSON_IsNumber((j = cJSON_GetObjectItem(root, "lon")))) {
        c->lon = j->valuedouble;
    }
    if (cJSON_IsNumber((j = cJSON_GetObjectItem(root, "radius_nm")))) {
        int r = (int)j->valuedouble;
        if (r >= 5 && r <= 250) {
            c->radius_nm = r;
        }
    }
    if (cJSON_IsNumber((j = cJSON_GetObjectItem(root, "theme")))) {
        c->theme = (int)j->valuedouble;
    }
    if (cJSON_IsNumber((j = cJSON_GetObjectItem(root, "lang")))) {
        c->lang = (int)j->valuedouble;
    }
    cJSON_Delete(root);

    settings_save();
    httpd_resp_sendstr(req, "saved - restarting");
    ESP_LOGI(TAG, "config updated from web, restarting");
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return ESP_OK;
}

static esp_err_t log_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    const char *paths[2] = { OBSLOG_OLD_PATH, OBSLOG_PATH };
    char buf[1024];
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(paths[i], "r");
        if (f == NULL) {
            continue;
        }
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
                fclose(f);
                httpd_resp_send_chunk(req, NULL, 0);
                return ESP_OK;
            }
        }
        fclose(f);
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
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

static esp_err_t metrics_get(httpd_req_t *req)
{
    char *copy = NULL;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_json != NULL) {
        copy = strdup(s_json);
    }
    xSemaphoreGive(s_mux);

    char out[512];
    int count = 0, unique = 0, alt = 0;
    double nearest = 0;
    if (copy != NULL) {
        cJSON *root = cJSON_Parse(copy);
        if (root != NULL) {
            const cJSON *fl = cJSON_GetObjectItem(root, "flights");
            count = cJSON_IsArray(fl) ? cJSON_GetArraySize(fl) : 0;
            const cJSON *st = cJSON_GetObjectItem(root, "stats");
            const cJSON *v;
            if (st) {
                if (cJSON_IsNumber((v = cJSON_GetObjectItem(st, "unique_aircraft")))) unique = (int)v->valuedouble;
                if (cJSON_IsNumber((v = cJSON_GetObjectItem(st, "max_alt_ft")))) alt = (int)v->valuedouble;
            }
            const cJSON *first = cJSON_IsArray(fl) ? cJSON_GetArrayItem(fl, 0) : NULL;
            if (first && cJSON_IsNumber((v = cJSON_GetObjectItem(first, "dist_km")))) nearest = v->valuedouble;
            cJSON_Delete(root);
        }
        free(copy);
    }
    snprintf(out, sizeof(out),
             "esp32flight_aircraft_count %d\n"
             "esp32flight_unique_aircraft %d\n"
             "esp32flight_max_alt_ft %d\n"
             "esp32flight_nearest_km %.1f\n"
             "esp32flight_heap_free_bytes %u\n"
             "esp32flight_uptime_seconds %lld\n",
             count, unique, alt, nearest,
             (unsigned)esp_get_free_heap_size(),
             (long long)(esp_timer_get_time() / 1000000LL));
    httpd_resp_set_type(req, "text/plain; version=0.0.4");
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
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
        { .uri = "/api/config", .method = HTTP_GET, .handler = config_get },
        { .uri = "/api/config", .method = HTTP_POST, .handler = config_post },
        { .uri = "/api/log", .method = HTTP_GET, .handler = log_get },
        { .uri = "/screen.bmp", .method = HTTP_GET, .handler = screen_get },
        { .uri = "/metrics", .method = HTTP_GET, .handler = metrics_get },
        { .uri = "/ota", .method = HTTP_POST, .handler = ota_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    ESP_LOGI(TAG, "web panel at http://esp32flight.local/");
}
