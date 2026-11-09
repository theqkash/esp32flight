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
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "mbedtls/base64.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "mdns.h"

#include "cJSON.h"
#include "lvgl_port.h"
#include "obslog.h"
#include "alertlog.h"
#include "settings.h"
#include "faflight.h"
#include "input_ctl.h"
#include "airports.h"
#include "ui.h"
#include "ui_settings.h"
#include "flight_model.h"
#include "waveshare_rgb_lcd_port.h"

static const char *TAG = "web";

/* HTTP Basic Auth against the panel password (user "admin"). Empty
 * password in settings means the panel and API are open. */
static bool auth_ok(httpd_req_t *req)
{
    const char *pw = settings_get()->web_pass;
    if (pw[0] == '\0') {
        return true;
    }
    char hdr[128] = "";
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    char cred[100];
    int n = snprintf(cred, sizeof(cred), "admin:%s", pw);
    unsigned char b64[140];
    size_t olen = 0;
    if (n <= 0 || mbedtls_base64_encode(b64, sizeof(b64), &olen,
                                        (const unsigned char *)cred, n) != 0) {
        return false;
    }
    char expect[160];
    snprintf(expect, sizeof(expect), "Basic %.*s", (int)olen, b64);
    return strcmp(hdr, expect) == 0;
}

static esp_err_t require_auth(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"esp32flight\"");
    return httpd_resp_sendstr(req, "auth required");
}

#define AUTH_GUARD(req) do { if (!auth_ok(req)) return require_auth(req); } while (0)


static char *s_json;
static SemaphoreHandle_t s_mux;

static const char INDEX_HTML[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>esp32flight</title>"
"<link rel='stylesheet' href='https://unpkg.com/leaflet@1.9.4/dist/leaflet.css'>"
"<script src='https://unpkg.com/leaflet@1.9.4/dist/leaflet.js'></script>"
"<style>"
":root{--bg:#0b1110;--bg2:#0f1816;--card:#121d1a;--card2:#16231f;--line:#1f302b;--line2:#2a4038;"
"--text:#e8f1ed;--dim:#8ea69b;--dim2:#6a8278;--acc:#4fd69c;--acc2:#2fb57d;--accink:#04140c;"
"--warn:#ffcf66;--bad:#ff6b6b;--shadow:0 1px 2px rgba(0,0,0,.25),0 8px 24px rgba(0,0,0,.18);color-scheme:dark}"
"@media (prefers-color-scheme:light){:root{--bg:#f4f7f6;--bg2:#eef3f1;--card:#ffffff;--card2:#f6faf8;--line:#e1e9e5;--line2:#cfdcd6;"
"--text:#13201b;--dim:#5b7168;--dim2:#7b8f87;--acc:#13a46b;--acc2:#0e8a59;--accink:#ffffff;--warn:#b7791f;--bad:#c53030;"
"--shadow:0 1px 2px rgba(16,40,30,.06),0 8px 24px rgba(16,40,30,.06);color-scheme:light}}"
"*{box-sizing:border-box}"
"body{background:radial-gradient(1200px 600px at 10% -10%,var(--bg2),var(--bg));background-attachment:fixed;color:var(--text);"
"font:15px/1.45 ui-sans-serif,system-ui,-apple-system,'Segoe UI',Roboto,Inter,sans-serif;margin:0;-webkit-font-smoothing:antialiased}"
".top{position:sticky;top:0;z-index:1000;background:color-mix(in srgb,var(--bg) 82%,transparent);backdrop-filter:blur(12px);"
"-webkit-backdrop-filter:blur(12px);border-bottom:1px solid var(--line)}"
".topin{max-width:1240px;margin:0 auto;padding:12px 20px;display:flex;align-items:center;gap:16px;flex-wrap:wrap}"
".brand{display:flex;align-items:center;gap:10px;min-width:0}"
"h1{font-size:19px;margin:0;font-weight:750;letter-spacing:-.2px;color:var(--text)}"
"h1 .mark{display:inline-grid;place-items:center;width:30px;height:30px;border-radius:9px;background:linear-gradient(135deg,var(--acc),var(--acc2));"
"color:var(--accink);font-size:16px;margin-right:2px;vertical-align:-6px}"
"h1 .mark i{display:inline-block;transform:rotate(-45deg);font-style:normal}"
"#hdr{font-size:13px;color:var(--dim);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:52vw}"
".wrap{max-width:1240px;margin:0 auto;padding:18px 20px 90px}"
".dim{color:var(--dim);font-size:14px}"
"a{color:var(--acc);text-decoration:none}a:hover{text-decoration:underline}"
".tabs{display:flex;gap:4px;margin-left:auto;background:var(--card);border:1px solid var(--line);border-radius:12px;padding:4px}"
".tab{background:transparent;color:var(--dim);font-weight:650;border-radius:9px;padding:7px 14px;border:0;box-shadow:none}"
".tab:hover{color:var(--text);background:var(--card2);filter:none}"
".tab.on{background:var(--acc);color:var(--accink)}"
".tabpane{display:none}.tabpane.on{display:block;animation:fade .18s ease}"
"@keyframes fade{from{opacity:0;transform:translateY(4px)}to{opacity:1;transform:none}}"
"button{background:var(--acc);color:var(--accink);border:0;border-radius:10px;padding:9px 16px;font:inherit;font-weight:700;cursor:pointer;"
"transition:filter .15s,transform .05s}"
"button:hover:not(:disabled){filter:brightness(1.08)}button:active:not(:disabled){transform:translateY(1px)}"
"button:disabled{opacity:.4;cursor:default}"
"button.ghost{background:var(--card2);color:var(--text);border:1px solid var(--line2)}"
".cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px}"
".card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:12px 14px;font-size:12px;color:var(--dim);"
"box-shadow:var(--shadow);font-weight:600;line-height:1.5}"
".card b{display:block;color:var(--text);font-size:20px;letter-spacing:-.2px;text-transform:none;font-weight:700;margin-top:2px;"
"font-variant-numeric:tabular-nums}"
"#map{height:clamp(380px,60vh,720px);border-radius:16px;margin-top:14px;border:1px solid var(--line);box-shadow:var(--shadow)}"
".darktiles{filter:invert(1) hue-rotate(180deg) brightness(.75) contrast(1.1) saturate(.4)}"
"@media (prefers-color-scheme:light){.darktiles{filter:none}}"
".plane{font-size:20px;line-height:20px;text-shadow:0 0 3px #000}"
".tw{overflow-x:auto;-webkit-overflow-scrolling:touch;max-width:100%;margin-top:14px;background:var(--card);border:1px solid var(--line);"
"border-radius:16px;box-shadow:var(--shadow)}"
".tw table{min-width:640px}"
"table{width:100%;border-collapse:collapse;font-size:14px}"
"th{color:var(--dim);text-align:left;padding:10px 12px;border-bottom:1px solid var(--line);font-size:11px;text-transform:uppercase;"
"letter-spacing:.6px;font-weight:650;position:sticky;top:0;background:var(--card)}"
"th.r,td.r{text-align:right}td.r{font-variant-numeric:tabular-nums;white-space:nowrap}"
"td{padding:9px 12px;border-bottom:1px solid var(--line)}"
"tbody tr:last-child td{border-bottom:0}"
"tbody tr:hover td{background:var(--card2)}"
"#rows tr{cursor:pointer}#rows tr.sel td{background:color-mix(in srgb,var(--acc) 16%,var(--card))}"
"tr.em td{background:color-mix(in srgb,var(--bad) 28%,var(--card))}"
"tr.gold td b{color:var(--warn)}"
".bar{background:var(--line);height:6px;border-radius:3px;min-width:70px}"
".bar i{display:block;background:var(--acc);height:6px;border-radius:3px}"
".sect{margin-top:22px}"
".sect h3{margin:0 0 12px;font-size:17px;font-weight:700}"
".cfgcard{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:16px 18px;margin-top:14px;box-shadow:var(--shadow)}"
".cfgcard h4{margin:0 0 12px;color:var(--text);font-size:14px;font-weight:700;display:flex;align-items:center;gap:8px}"
".cfgcard h4:before{content:'';width:8px;height:8px;border-radius:3px;background:var(--acc)}"
".grid2{display:grid;grid-template-columns:repeat(auto-fill,minmax(250px,1fr));gap:14px 16px}"
".grid2 label{font-size:12px;color:var(--dim);display:block;margin-bottom:5px;font-weight:600}"
"input,select{width:100%;background:var(--bg);border:1px solid var(--line2);color:var(--text);border-radius:10px;padding:9px 11px;font:inherit;"
"font-size:14px;transition:border-color .15s,box-shadow .15s}"
"input[type=checkbox]{width:auto;accent-color:var(--acc)}"
"input[type=file]{padding:7px}"
"input:focus,select:focus{outline:none;border-color:var(--acc);box-shadow:0 0 0 3px color-mix(in srgb,var(--acc) 22%,transparent)}"
".help{font-size:12px;color:var(--dim2);margin-top:5px;line-height:1.45}"
".sech{grid-column:1/-1;color:var(--acc);font-size:12px;font-weight:700;margin-top:10px;text-transform:uppercase;letter-spacing:.6px}"
"#hq{width:240px;display:inline-block}"
"#ota{margin-top:4px}"
".savebar{position:sticky;bottom:0;margin-top:18px;padding:12px 16px;background:color-mix(in srgb,var(--card) 92%,transparent);"
"backdrop-filter:blur(10px);border:1px solid var(--line);border-radius:14px;box-shadow:var(--shadow);display:flex;align-items:center;gap:12px}"
".trk{display:flex;gap:8px;align-items:center;flex-wrap:wrap}"
".trk input{max-width:220px;text-transform:uppercase}"
".pill{display:inline-block;padding:3px 10px;border-radius:999px;font-size:12px;font-weight:650;background:var(--card2);border:1px solid var(--line2);color:var(--dim)}"
".pill.on{background:color-mix(in srgb,var(--acc) 18%,var(--card));color:var(--acc);border-color:color-mix(in srgb,var(--acc) 45%,var(--line))}"
".api dt{color:var(--acc);font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:14px;margin-top:18px;font-weight:600}"
".api dd{margin:5px 0 0;color:var(--dim);font-size:13px;line-height:1.55}"
"code{background:var(--card2);border:1px solid var(--line);border-radius:6px;padding:1px 6px;font-size:12px;color:var(--text)}"
"pre{background:var(--bg);border:1px solid var(--line);border-radius:10px;padding:10px 12px;overflow-x:auto;font-size:12px;color:var(--text)}"
".leaflet-popup-content-wrapper,.leaflet-popup-tip{background:var(--card);color:var(--text)}"
"@media (max-width:720px){.topin{padding:10px 14px}.wrap{padding:14px 14px 90px}.tabs{margin-left:0;width:100%}.tab{flex:1;padding:7px 6px}#hdr{max-width:100%}}"
"</style></head><body>"
"<div class='top'><div class='topin'><div class='brand'><h1><span class='mark'><i>\xe2\x9c\x88</i></span> esp32flight</h1>"
"<div id='hdr'>loading...</div></div>"
"<div class='tabs'>"
"<button class='tab on' id='tb_live' onclick=\"showTab('live')\">Live</button>"
"<button class='tab' id='tb_hist' onclick=\"showTab('hist')\">History</button>"
"<button class='tab' id='tb_set' onclick=\"showTab('set')\">Settings</button>"
"<button class='tab' id='tb_api' onclick=\"showTab('api')\">API</button>"
"</div></div></div><div class='wrap'>"
"<div id='t_live' class='tabpane on'>"
"<div class='cfgcard' style='margin:0 0 14px'><h4>Track a flight</h4><div class='trk'>"
"<input id='trk_id' placeholder='callsign, reg or hex' maxlength='11'>"
"<button type='button' onclick='trkSet(true)'>Track</button>"
"<button type='button' class='ghost' onclick='trkSel()'>Use selected</button>"
"<button type='button' class='ghost' onclick='trkSet(false)'>Stop</button>"
"<span id='trk_st' class='pill'>off</span></div>"
"<div class='help'>Follow one flight anywhere in the world, e.g. family on board. While it is in the air the screen shows only that flight; Stop returns to your sky. Applies at once, no restart.</div></div>"
"<div class='cards' id='cards'></div>"
"<div id='map'></div>"
"<div class='tw'><table><thead><tr><th>Flight</th><th>Airline</th><th>Aircraft</th><th>Route</th>"
"<th>Progress</th><th class='r'>Alt</th><th class='r'>Speed</th><th class='r'>Dist</th></tr></thead>"
"<tbody id='rows'></tbody></table></div>"
"</div>"
"<div id='t_set' class='tabpane'>"
"<div id='ota'><div class='cfgcard' style='margin:0 0 12px'><h4>Which file do I need?</h4>"
"<div id='otahelp' class='help'>checking this device...</div></div>"
"<span class='dim'>Firmware update (.bin): </span>"
"<input type='file' id='fw'> <button id='otabtn' onclick='ota()' disabled>Flash</button> <span id='otastat' class='dim'></span>"
"<div class='dim' style='margin-top:8px'>"
"<a href='/screen.bmp'>screenshot</a> &middot; <a href='/api/state'>api</a> &middot; <a href='/api/log' download='esp32flight-log.tsv'>log CSV</a> &middot; <a href='/metrics'>metrics</a> &middot; "
"<a href='https://github.com/theqkash/esp32flight'>github.com/theqkash/esp32flight</a></div></div>"
"<div class='sect'><h3>Device settings</h3>"
"<div class='cfgcard'><h4>Network</h4><div class='grid2'>"
"<div><label>Wi-Fi SSID</label><input id='c_ssid'>"
"<div class='help'>2.4 GHz networks only (ESP32 has no 5 GHz radio).</div></div>"
"<div><label>Wi-Fi password</label><input id='c_pass' type='password'>"
"<div class='help'>Leave empty to keep the current password.</div></div>"
"<div><label>Panel password</label><input id='c_web_pass' type='password' placeholder='empty = keep current'>"
"<div class='help'>Protects this panel and the whole API (HTTP Basic Auth, user: admin). To remove it, clear the field in the on-device settings, System tab.</div></div>"
"</div></div>"
"<div class='cfgcard'><h4>Location</h4><div class='grid2'>"
"<div><label>Location mode</label><select id='c_fixed'><option value='0'>auto (IP geolocation)</option><option value='1'>fixed coordinates</option></select>"
"<div class='help'>Auto locates the device by its internet address at boot. Pick fixed to watch a different area.</div></div>"
"<div><label>City or airport code</label><input id='c_city' placeholder='city name, EPGD or GDN, Enter'>"
"<div id='c_cityres' class='help'></div></div>"
"<div><label>Favorite locations</label><div id='favbox' class='help'></div>"
"<div class='help'>Save the coordinates above under a name and switch with one click. Three slots.</div></div>"
"<div><label>Latitude</label><input id='c_lat' inputmode='decimal' placeholder='51.1079'></div>"
"<div><label>Longitude</label><input id='c_lon' inputmode='decimal' placeholder='17.0385'></div>"
"<div><label>Radius (nautical miles, 1-250)</label><input id='c_radius_nm' type='number'>"
"<div class='help'>Search radius around the location. 54 nm is about 100 km; a radius of 1-2 nm shows just what passes overhead.</div></div>"
"</div></div>"
"<div class='cfgcard'><h4>Filters</h4><div class='grid2'>"
"<div><label>Hide ground traffic</label><select id='c_hide_ground'><option value='0'>no</option><option value='1'>yes</option></select>"
"<div class='help'>Hides aircraft taxiing or parked at nearby airports.</div></div>"
"<div><label>Aircraft classes</label>"
"<div style='display:flex;gap:10px;flex-wrap:wrap;padding:6px 0'>"
"<label><input type='checkbox' id='c_cls0'> airliners</label>"
"<label><input type='checkbox' id='c_cls1'> light</label>"
"<label><input type='checkbox' id='c_cls2'> helicopters</label>"
"<label><input type='checkbox' id='c_cls3'> military</label>"
"<label><input type='checkbox' id='c_cls4'> other</label>"
"</div><div class='help'>Show only the selected classes. Military comes from the community aircraft database; class of others from the ADS-B emitter category.</div></div>"
"<div><label>Rain radar overlay</label><select id='c_rain_overlay'><option value='0'>off</option><option value='1'>on</option></select>"
"<div class='help'>Precipitation from RainViewer blended over the radar and maps.</div></div>"
"<div><label>Route on list and radar</label><select id='c_show_route'><option value='0'>off</option><option value='1'>on</option></select>"
"<div class='help'>Origin and destination codes on list rows, cities in the tapped-aircraft bubble.</div></div>"
"<div><label>Screensaver style</label><select id='c_amb_style'><option value='0'>sky map</option><option value='1'>retro radar</option></select></div>"
"<div><label>Altitude from (ft, 0 = off)</label><input id='c_alt_min_ft' type='number'></div>"
"<div><label>Altitude to (ft, 0 = off)</label><input id='c_alt_max_ft' type='number'></div>"
"<div><label>Airport (ICAO or IATA)</label><input id='c_filter_airport' placeholder='KRK'>"
"<div class='help'>Flights departing from or arriving at this airport, filtered by the mode on the right. Applies once a flight's route is resolved.</div></div>"
"<div><label>Airport filter mode</label><select id='c_filter_apt_exclude'><option value='0'>show only this airport</option><option value='1'>hide this airport</option></select></div>"
"</div></div>"
"<div class='cfgcard'><h4>Layers and extra objects</h4><div class='grid2'>"
"<div><label>Airspace zones (openAIP)</label><select id='c_airspace_enabled'><option value='0'>off</option><option value='1'>on</option></select>"
"<div class='help'>CTR, TMA and danger zone outlines on the radar map. Needs an openAIP key (Integrations).</div></div>"
"<div><label>ISS on the radar</label><select id='c_iss_enabled'><option value='0'>off</option><option value='1'>on</option></select>"
"<div class='help'>The Space Station as a radar object when its track crosses your area, with an above-horizon indicator.</div></div>"
"<div><label>Weather balloons (SondeHub)</label><select id='c_sonde_enabled'><option value='0'>off</option><option value='1'>on</option></select>"
"<div class='help'>Radiosondes tracked by the SondeHub community within 250 km.</div></div>"
"<div><label>Ships (AIS)</label><select id='c_ships_enabled'><option value='0'>off</option><option value='1'>on</option></select>"
"<div class='help'>Vessels near you via aisstream.io. Needs a free key (Integrations).</div></div>"
"<div><label>TAF airport forecast</label><select id='c_taf_enabled'><option value='0'>off</option><option value='1'>on</option></select>"
"<div class='help'>Terminal forecast for the nearest airport, shown next to the METAR.</div></div>"
"</div></div>"
"<div class='cfgcard'><h4>Alerts</h4><div class='grid2'>"
"<div><label>ntfy.sh topic (push to your phone)</label><input id='c_ntfy_topic' placeholder='my-secret-esp32flight-8341'>"
"<div class='help'>Free push notifications, no account needed. 1) Install the ntfy app (ntfy.sh). 2) Subscribe to a topic name you invent. 3) Enter the same name here. Alerts: emergency squawks, watchlist, flyovers.</div></div>"
"<div><label>Flyover (CPA) alerts</label><select id='c_cpa_alerts'><option value='0'>off</option><option value='1'>on</option></select>"
"<div class='help'>Screen banner plus push a few minutes before an aircraft passes within 5 km of you.</div></div>"
"<div><label>Flyover alert scope</label><select id='c_cpa_all'><option value='0'>interesting aircraft only</option><option value='1'>all aircraft</option></select>"
"<div class='help'>Interesting = military, notable heavies and your watchlist (default). All = every aircraft; can be noisy under a busy approach path.</div></div>"
"<div><label>Watchlist</label><input id='c_watch_regs' placeholder='SP-LR,RCH,A388'>"
"<div class='help'>Comma-separated registration or callsign prefixes: highlighted in gold and push-notified.</div></div>"
"<div><label>Webhook URL</label><input id='c_webhook_url' placeholder='https://n8n.example.com/hook/abc'>"
"<div class='help'>Every alert POSTs JSON {source, title, message} to this address (Node-RED, n8n, Discord/Slack bridges).</div></div>"
"</div></div>"
"<div class='cfgcard'><h4>Display</h4><div class='grid2'>"
"<div><label>Brighter map tiles</label><select id='c_map_light'><option value='0'>off</option><option value='1'>on</option></select>"
"<div class='help'>Lifts the dark map style for readability. New tiles only; the device reloads its map after a restart.</div></div>"
"<div><label>Map on retro radar</label><select id='c_retro_map'><option value='0'>off</option><option value='1'>on</option></select>"
"<div class='help'>Your map under the sweep: coastlines on CARTO tiles, a phosphor rendering on OpenStreetMap or a custom server.</div></div>"
"<div><label>12-hour clock (AM/PM)</label><select id='c_clock_12h'><option value='0'>24 h</option><option value='1'>12 h AM/PM</option></select></div>"
"<div><label>Brightness control</label><select id='c_brightness_ctl'><option value='0'>off (default)</option><option value='1'>on</option></select>"
"<div class='help'>Master switch for backlight dimming. Off keeps stock full-brightness behavior on every board.</div></div>"
"<div><label>Brightness (5-100)</label><input id='c_brightness' type='number' min='5' max='100'>"
"<div class='help'>Needs brightness control on and a dimmable backlight; CH422G boards (Waveshare 800x480) are on/off.</div></div>"
"<div><label>Theme</label><select id='c_theme'><option value='0'>Dark</option><option value='1'>Light</option><option value='2'>Black</option><option value='3'>Nord</option><option value='4'>Solarized</option><option value='5'>Purple</option><option value='6'>Forest</option></select></div>"
"<div><label>Language</label><select id='c_lang'><option value='0'>English</option><option value='1'>Polski</option><option value='2'>Magyar</option></select></div>"
"<div><label>Units</label><select id='c_metric_units'><option value='0'>aviation (ft, kt)</option><option value='1'>metric (m, km/h)</option></select></div>"
"<div><label>Temperature</label><select id='c_temp_f'><option value='0'>\u00b0C</option><option value='1'>\u00b0F</option></select></div>"
"<div><label>METAR style</label><select id='c_metar_decoded'><option value='0'>raw</option><option value='1'>decoded</option></select></div>"
"<div><label>Auto-cycle flights</label><select id='c_follow_mode'><option value='0'>on (default)</option><option value='1'>off, follow selection</option></select>"
"<div class='help'>Off keeps the selected flight on screen until you pick another one.</div></div>"
"<div><label>Always show the nearest aircraft</label><select id='c_pin_nearest'><option value='0'>off</option><option value='1'>on</option></select>"
"<div class='help'>The detail view follows whichever aircraft is closest. A tap on another one shows it for 30 s.</div></div>"
"<div><label>Map screensaver after (minutes, 0 = off)</label><input id='c_ambient_idle_min' type='number'>"
"<div class='help'>Full-screen map of your area after this many idle minutes. Tap to return.</div></div>"
"<div><label>Screensaver schedule</label><select id='c_amb_sched'><option value='0'>any time (default)</option><option value='1'>only between the hours below</option></select>"
"<div class='help'>Outside these hours the radar stays up, and the screensaver closes when the window ends.</div></div>"
"<div><label>Screensaver from</label><input id='c_amb_from' type='time'></div>"
"<div><label>Screensaver until</label><input id='c_amb_to' type='time'></div>"
"<div><label>Night mode</label><select id='c_night_enabled'><option value='0'>off</option><option value='1'>on</option></select>"
"<div class='help'>Backlight turns off during the quiet hours below; first tap wakes the screen.</div></div>"
"<div><label>Night hours</label><select id='c_night_auto'><option value='0'>fixed hours below</option><option value='1'>auto: sunset to sunrise</option></select></div>"
"<div><label>Night from</label><input id='c_night_start' type='time'></div>"
"<div><label>Night until</label><input id='c_night_end' type='time'></div>"
"</div></div>"
"<div class='cfgcard'><h4>Integrations</h4><div class='grid2'>"
"<div><label>MQTT broker (Home Assistant)</label><input id='c_mqtt_uri' placeholder='mqtt://user:pass@192.168.1.10:1883'>"
"<div class='help'>Sensors appear automatically in Home Assistant via MQTT discovery.</div></div>"
"<div><label>FlightAware AeroAPI key</label><input id='c_fa_key'>"
"<div class='help'>Optional and billed per call by FlightAware. Adds exact flight numbers and live routes for the selected or nearest airliner.</div></div>"
"<div><label>FlightAware calls per month (budget)</label><input id='c_fa_limit' type='number' min='10' max='20000'>"
"<div class='help' id='fa_usage'>Lookups stop when the budget is used and spread evenly over the month. 900 stays inside the free Personal credit.</div></div>"
"<div><label>openAIP API key</label><input id='c_openaip_key'>"
"<div class='help'>Free account at openaip.net; powers the airspace zone layer.</div></div>"
"<div><label>aisstream.io API key</label><input id='c_ais_key'>"
"<div class='help'>Free key at aisstream.io; powers the ship layer.</div></div>"
"<div><label>CARTO basemaps key</label><input id='c_carto_key' placeholder='blank = OSM with built-in dark style'>"
"<div class='help'>Blank: OpenStreetMap tiles restyled dark on the device. With a free key from carto.com/basemaps/apikey: CARTO&#39;s native dark_all / light_all.</div></div>"
"<div><label>Custom tile URL template</label><input id='c_tile_url' placeholder='https://tiles.example.com/{z}/{x}/{y}.png'>"
"<div class='help'>Overrides both. Any 256px {z}/{x}/{y} PNG tile server; tiles are shown as served, no restyling.</div></div>"
"<div><label>Local ADS-B receiver (dump1090/readsb)</label><input id='c_local_adsb' placeholder='http://192.168.1.50:8080/data/aircraft.json'>"
"<label>Use local receiver</label><select id='c_local_adsb_use'><option value='1'>on</option><option value='2'>on, plus internet sources</option><option value='0'>off (internet sources)</option></select>"
"<div class='help'>Reads aircraft straight from your antenna instead of internet APIs; falls back automatically.</div></div>"
"</div></div>"
"<div class='cfgcard'><h4>Physical controls</h4>"
"<div class='help'>Buttons and rotary encoders on a PCF8574 or MCP23017 I2C expander wired to the display bus, or on free GPIO pins where the board has any (Guition JC8048W550: 17 and 18) - issue #13. Custom decks can also call <code>POST /api/input {\"action\":\"zoom_in\"}</code> directly.</div>"
"<div class='grid2'><div><label>Expander chip</label><select id='in_chip'><option value='none'>none</option><option value='pcf'>PCF8574</option><option value='mcp'>MCP23017</option></select></div>"
"<div><label>I2C address (hex)</label><input id='in_addr' placeholder='20' maxlength='2'></div></div>"
"<div id='inrows'></div>"
"<div style='margin-top:8px'><button type='button' onclick='inLearn()'>Add mapping (learn: press or turn now)</button> <button type='button' onclick='inManual()'>Add manually</button> <span id='instat' class='dim'></span></div>"
"<div class='help'>Learn listens for 6 seconds: press a button once (short or long chosen in the row) or turn the encoder one click. GPIO entries (g17s, q17,18) are added manually - unmapped GPIOs are not polled, so learn cannot see them. Save and restart applies the map.</div>"
"</div>"
"<div class='cfgcard'><h4>Backup</h4><div class='grid2'>"
"<div><label>Download</label><a href='/api/backup' download><button type='button'>Backup settings (JSON)</button></a>"
"<div class='help'>Everything including Wi-Fi and API keys. Keep it private.</div></div>"
"<div><label>Restore</label><input type='file' id='bkfile' accept='.json'> <button type='button' onclick='restoreBk()'>Restore and restart</button> <span id='bkstat' class='dim'></span>"
"<div class='help'>Loads a backup JSON and restarts the device with those settings.</div></div>"
"</div></div>"
"<div class='savebar'><button id='cfgsave' onclick='saveCfg()' disabled>Save and restart</button> <span id='cfgstat' class='dim'></span></div></div>"
"</div>"
"<div id='t_hist' class='tabpane'>"
"<div class='sect'><h3>Spotting history</h3>"
"<input id='hq' placeholder='filter (callsign, type...)'> <button onclick='loadHist()'>Load</button>"
"<div class='tw'><table><tbody id='hrows'></tbody></table></div></div>"
"<div class='sect'><h3>Alert history</h3>"
"<div class='help'>Emergency squawks, watchlist hits and flyover predictions that triggered a notification.</div>"
"<div class='tw'><table><tbody id='arows'></tbody></table></div></div>"
"</div>"
"<div id='t_api' class='tabpane'>"
"<div class='sect api'><h3>HTTP API</h3>"
"<p class='dim'>Base URL: <code>http://esp32flight.local</code> (or the device IP). When a panel password is set, every endpoint requires HTTP Basic Auth with user <code>admin</code>: <code>curl -u admin:PASSWORD ...</code></p>"
"<dl>"
"<dt>GET /api/state</dt>"
"<dd>Live state JSON, refreshed with every poll cycle (about 8 s). Fields: <code>flights[]</code> with hex, callsign, reg, cc (country), type, airline, flight_iata, lat, lon, alt_ft, gs_kt, track, dist_km, squawk, interesting, trail (list of [lat,lon]) and route {from, to, from_lat, from_lon, to_lat, to_lon, progress}; plus <code>weather</code>, <code>net</code> (ssid, rssi, ip, mdns), <code>stats</code> (unique_aircraft, max_alt_ft, hours[], top_airlines[], days[], metar, version) and the live <code>ota_enabled</code> flag."
"<pre>curl http://esp32flight.local/api/state | jq '.flights[0]'</pre></dd>"
"<dt>GET /api/config</dt>"
"<dd>Current settings as JSON. Passwords are never returned; <code>web_pass_set</code> only tells whether one is active.</dd>"
"<dt>POST /api/config</dt>"
"<dd>JSON body with any subset of the config fields; omitted fields keep their value. The device saves to flash and restarts. Field names match the GET response, plus write-only <code>pass</code> (Wi-Fi) and <code>web_pass</code> (panel)."
"<pre>curl -X POST http://esp32flight.local/api/config -d '{\"radius_nm\":80,\"theme\":3}'</pre></dd>"
"<dt>GET /api/log</dt>"
"<dd>Spotting history, one aircraft per line, tab-separated: unix epoch, ICAO hex, callsign, type, airline. Rotates at about 256 KB (current + previous file are concatenated).</dd>"
"<dt>GET /api/airport?code=EPGD</dt>"
"<dd>Airport lookup in the onboard OurAirports database, ICAO or IATA: <code>{icao, iata, city, lat, lon}</code>. Device build only.</dd>"
"<dt>GET /screen.bmp</dt>"
"<dd>Live screenshot of the display as a BMP.</dd>"
"<dt>GET /metrics</dt>"
"<dd>Prometheus text format: <code>esp32flight_aircraft_count</code>, <code>esp32flight_unique_aircraft</code>, <code>esp32flight_max_alt_ft</code>, <code>esp32flight_nearest_km</code>, <code>esp32flight_heap_free_bytes</code>, <code>esp32flight_uptime_seconds</code>.</dd>"
"<dt>POST /ota</dt>"
"<dd>Firmware update: send the raw OTA .bin as the request body. Returns 403 unless updates are unlocked in the on-device settings (System tab; the switch re-locks on every restart)."
"<pre>curl --data-binary @esp32flight-ota.bin http://esp32flight.local/ota</pre></dd>"
"<dt>Webhook payload</dt>"
"<dd>On every alert (emergency squawk, watchlist, flyover) the device POSTs to your webhook URL: <code>{\"source\":\"esp32flight\",\"title\":\"...\",\"message\":\"...\"}</code></dd>"
"</dl></div>"
"</div>"
"</div>"
"<script>"
"let map,layer,homeSet=false,routeLine=null,selHex=null,metric=false;"
"const grp=v=>String(Math.round(v)).replace(/\\B(?=(\\d{3})+(?!\\d))/g,'\\u2009');"
"const gst={},alts={};"
"const ualt=v=>metric?grp(v*0.3048)+' m':grp(v)+' ft';"
"const uspd=v=>metric?Math.round(v*1.852)+' km/h':v+' kt';"
"function showTab(n){['live','hist','set','api'].forEach(k=>{"
"document.getElementById('t_'+k).classList.toggle('on',k===n);"
"document.getElementById('tb_'+k).classList.toggle('on',k===n);});"
"if(n==='live'&&map)setTimeout(()=>map.invalidateSize(),50);"
"if(n==='hist'){loadHist();loadAlerts();}}"
"async function loadAlerts(){try{const r=await fetch('/api/alerts');const t=await r.text();"
"document.getElementById('arows').innerHTML=t.trim().split('\\n').reverse().filter(l=>l)"
".slice(0,200).map(l=>{const p=l.split('\t');"
"return `<tr><td class=dim>${new Date(+p[0]*1000).toLocaleString()}</td><td><b>${p[1]||''}</b></td><td>${p[2]||''}</td></tr>`;})"
".join('')||'<tr><td class=dim>no alerts yet</td></tr>';}catch(e){}}"
"function ensureMap(lat,lon,rkm){if(map||typeof L==='undefined')return;"
"map=L.map('map').setView([lat,lon],8);"
"map.fitBounds(L.latLng(lat,lon).toBounds(rkm*2000),{padding:[10,10]});"
"L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',"
"{attribution:'\\u00A9 OpenStreetMap contributors',maxZoom:12,className:'darktiles'}).addTo(map);"
"L.circle([lat,lon],{radius:rkm*1000,color:'#5dd39e',weight:1,fill:false}).addTo(map);"
"L.circleMarker([lat,lon],{radius:5,color:'#5dd39e',fillOpacity:1}).addTo(map);"
"map.on('click',()=>{if(routeLine){map.removeLayer(routeLine);routeLine=null;}selHex=null;hiRow(null);});"
"layer=L.layerGroup().addTo(map);"
"const inv=()=>map.invalidateSize();"
"addEventListener('resize',inv);addEventListener('orientationchange',()=>setTimeout(inv,300));"
"if(window.ResizeObserver)new ResizeObserver(inv).observe(document.getElementById('map'));}"
"function showRoute(f){if(routeLine)map.removeLayer(routeLine);"
"routeLine=L.polyline([[f.route.from_lat,f.route.from_lon],[f.lat,f.lon],[f.route.to_lat,f.route.to_lon]],"
"{color:'#5dd39e',weight:2,dashArray:'4'}).addTo(map);selHex=f.hex;}"
"function clearRoute(){if(routeLine){map.removeLayer(routeLine);routeLine=null;}selHex=null;}"
"let redraw=false,mks={},lastd=null;"
"function hiRow(h){document.querySelectorAll('#rows tr').forEach(tr=>tr.classList.toggle('sel',tr.dataset.hex===h));}"
"function rowScroll(h){const tr=document.querySelector(`#rows tr[data-hex='${h}']`);if(tr)tr.scrollIntoView({block:'nearest'});}"
"function selRow(h){const f=((lastd&&lastd.flights)||[]).find(x=>x.hex===h);"
"if(routeLine){map.removeLayer(routeLine);routeLine=null;}"
"if(f&&f.route)showRoute(f);"
"selHex=h;hiRow(h);const m=mks[h];if(m){map.panTo(m.getLatLng());m.openPopup();}}"
"const SPRC=['#e7f2ea','#9be8ff','#ffd166','#ff6b6b','#b5e8c8','#ff9ff3','#c3a6ff'];"
"const PLANEP='M12 2 L13.4 9 L22 13.2 L22 15 L13.4 12.6 L12.9 17.6 L15.8 19.8 L15.8 21.2 L12 20.2 L8.2 21.2 L8.2 19.8 L11.1 17.6 L10.6 12.6 L2 15 L2 13.2 L10.6 9 Z';"
"const GLIDP='M12 3 L12.7 9.4 L22.5 11.6 L22.5 13 L12.7 12.4 L12.4 17.4 L14.4 19.2 L14.4 20.2 L12 19.6 L9.6 20.2 L9.6 19.2 L11.6 17.4 L11.3 12.4 L1.5 13 L1.5 11.6 L11.3 9.4 Z';"
"function icoHtml(f){const c=SPRC[f.spr||0]||SPRC[0];"
"const s=f.spr===1?17:f.spr===4?19:f.spr===5||f.spr===6?18:22;"
"const rot=(f.spr===5||f.spr===6)?0:(f.track||0);let b;"
"if(f.spr===2)b=`<rect x='3' y='2' width='18' height='1.6' rx='.8' fill='${c}'/><rect x='11.4' y='3' width='1.2' height='6' fill='${c}'/><ellipse cx='12' cy='13' rx='3.6' ry='5' fill='${c}'/><rect x='11.5' y='17.5' width='1' height='4.5' fill='${c}'/><rect x='8.5' y='21.4' width='7' height='1.2' rx='.6' fill='${c}'/>`;"
"else if(f.spr===5)b=`<circle cx='12' cy='9.5' r='7' fill='${c}'/><path d='M8.5 15.5 L10.5 19 M15.5 15.5 L13.5 19' stroke='${c}' stroke-width='1.2'/><rect x='10' y='19' width='4' height='3.4' rx='1' fill='${c}'/>`;"
"else if(f.spr===6)b=`<circle cx='5.5' cy='5.5' r='3.4' fill='${c}'/><circle cx='18.5' cy='5.5' r='3.4' fill='${c}'/><circle cx='5.5' cy='18.5' r='3.4' fill='${c}'/><circle cx='18.5' cy='18.5' r='3.4' fill='${c}'/><rect x='9' y='9' width='6' height='6' rx='1.5' fill='${c}'/>`;"
"else b=`<path d='${f.spr===4?GLIDP:PLANEP}' fill='${c}'/>`;"
"return[`<svg width='${s}' height='${s}' viewBox='0 0 24 24' style='transform:rotate(${rot}deg);filter:drop-shadow(0 0 2px #000)'>${b}</svg>`,s];}"
"function drawMap(d){if(!d.lat)return;ensureMap(d.lat,d.lon,d.radius_km||100);"
"if(!layer)return;redraw=true;layer.clearLayers();mks={};"
"(d.flights||[]).forEach(f=>{if(f.lat===undefined)return;"
"const[ih,s]=icoHtml(f);"
"const ic=L.divIcon({className:'plane',html:ih,iconSize:[s,s],iconAnchor:[s/2,s/2]});"
"if(f.trail&&f.trail.length>1)L.polyline(f.trail,{color:'#ffd166',weight:1,opacity:.5}).addTo(layer);"
"const m=L.marker([f.lat,f.lon],{icon:ic}).addTo(layer);mks[f.hex]=m;"
"let t=`<b>${f.callsign}</b><br>${f.airline||''} ${f.type||''}<br>${ualt(f.alt_ft)} \\u00B7 ${uspd(f.gs_kt)}`;"
"if(f.route){t+=`<br>${f.route.from} \\u2192 ${f.route.to} (${f.route.progress}%)`;"
"m.on('click',()=>{showRoute(f);hiRow(f.hex);rowScroll(f.hex);});"
"if(f.hex===selHex)showRoute(f);}"
"else m.on('click',()=>{clearRoute();selHex=f.hex;hiRow(f.hex);rowScroll(f.hex);});"
"m.on('popupclose',()=>{if(!redraw&&selHex===f.hex){clearRoute();hiRow(null);}});"
"m.bindTooltip(f.callsign,{permanent:false}).bindPopup(t);});"
"redraw=false;"
"if(d.iss)L.circleMarker([d.iss.lat,d.iss.lon],{radius:8,color:'#9be8ff',weight:2,fillOpacity:.5})"
".addTo(layer).bindPopup(`<b>ISS</b><br>${d.iss.alt_km} km \u00B7 elev ${d.iss.elev_deg}\u00B0`);"
"(d.sondes||[]).forEach(sn=>L.circleMarker([sn.lat,sn.lon],{radius:6,color:'#ffb347',weight:2,fillOpacity:.5})"
".addTo(layer).bindPopup(`<b>${sn.type||'sonde'} ${sn.serial}</b><br>${(sn.alt_m/1000).toFixed(1)} km ${sn.vel_v>=0?'\u2191':'\u2193'}`));"
"(d.ships||[]).forEach(sh=>L.circleMarker([sh.lat,sh.lon],{radius:5,color:'#4fd1c5',weight:2,fillOpacity:.5})"
".addTo(layer).bindPopup(`<b>${sh.name||'ship'}</b><br>${sh.sog_kt.toFixed(1)} kt \u00B7 ${sh.cog}\u00B0`));}"
"let tempF=false;"
"async function load(){try{"
"const r=await fetch('/api/state');const d=await r.json();metric=!!d.metric;lastd=d;"
"let w=d.weather?` &nbsp;|&nbsp; ${tempF?Math.round(d.weather.temp_c*9/5+32)+'\\u00B0F':d.weather.temp_c+'\\u00B0C'} ${d.weather.desc}, wind ${d.weather.wind_kmh} km/h`:'';"
"const n=d.net||{};"
"document.getElementById('hdr').innerHTML=`${d.city||''} (${(+d.lat).toFixed(3)}, ${(+d.lon).toFixed(3)}), radius ${d.radius_km} km${w}`+((d.stats&&d.stats.metar)?`<br><span style='font-family:ui-monospace,monospace;font-size:12px'>${d.stats.metar}${(d.stats.taf?`<br>${d.stats.taf}`:'')}</span>`:'');"
"const s=d.stats||{};document.getElementById('cards').innerHTML="
"`<div class='card'>Network<b>${n.ssid?`${n.ssid} ${n.rssi||''} dBm`:location.host}</b>${[n.ip,n.mdns,n.heap_int?`RAM ${Math.round(n.heap_int/1024)} KB`:'',s.version?`v${s.version}`:''].filter(Boolean).join(' \\u00B7 ')}</div>`+"
"`<div class='card'>Unique aircraft<b>${s.unique_aircraft||0}</b></div>`+"
"`<div class='card'>Max altitude<b>${grp(s.max_alt_ft||0)} ft</b></div>`+"
"`<div class='card'>Fastest<b>${s.max_gs_kt||0} kt</b></div>`+"
"`<div class='card'>Farthest<b>${s.max_dist_km||0} km (${s.max_dist_callsign||'-'})</b></div>`+"
"`<div class='card'>Uptime<b>${s.uptime_min||0} min</b></div>`+"
"((s.days&&s.days.length)?`<div class='card'>Last days<b style='display:flex;align-items:flex-end;gap:2px;height:28px'>${s.days.slice(-14).map(dd=>`<i title='${dd.d}: ${dd.u}' style='display:block;width:8px;background:#5dd39e;height:${Math.max(2,Math.round(28*dd.u/Math.max(...s.days.map(x=>x.u),1)))}px'></i>`).join('')}</b></div>`:'');"
"const ob=document.getElementById('otabtn');ob.disabled=!d.ota_enabled;"
"const bn=(d.stats&&d.stats.board)||'';const vv=(d.stats&&d.stats.version)||'X.Y.Z';"
"const ota7b=/7B/i.test(bn),otaP4=/Tab5/i.test(bn);"
"document.getElementById('otahelp').innerHTML="
"`This device reports itself as <b>${bn||'unknown board'}</b>.<br>`+"
"`Update here with <code>esp32flight-v${vv}-ota.bin</code>`+"
"(otaP4?` <b>(Tab5 build)</b>`:ota7b?` <b>(7B build)</b>`:``)+"
"`, taken from the <a href='https://github.com/theqkash/esp32flight/releases/latest'>latest release</a>. `+"
"`The <code>-full.bin</code> files are only for the very first install over USB, they are not for this page.`;"
"document.getElementById('otastat').textContent=d.ota_enabled?'':'locked - enable OTA in device settings';"
"drawMap(d);"
"document.getElementById('rows').innerHTML=(d.flights||[]).map(f=>{"
"const prev=alts[f.hex]||0;let altc;"
"if(f.alt_ft){delete gst[f.hex];altc=ualt(f.alt_ft);}"
"else{if(prev>0)gst[f.hex]='landed \\u2193';"
"else if(!gst[f.hex]&&f.gs_kt>=40)gst[f.hex]='departing \\u2191';"
"altc=`<span class=dim>${gst[f.hex]||'gnd'}</span>`;}"
"alts[f.hex]=f.alt_ft||0;"
"const em=['7700','7600','7500'].includes(f.squawk);"
"const rt=f.route?`${f.route.from}${f.route.from_time?' '+f.route.from_time:''} \\u2192 ${f.route.to}${f.route.to_time?' '+f.route.to_time:''}<div class='dim'>${f.route.from_city||''}${f.route.from_cc?' ('+f.route.from_cc+')':''} \\u2192 ${f.route.to_city||''}${f.route.to_cc?' ('+f.route.to_cc+')':''}</div>`:'<span class=dim>?</span>';"
"const pr=f.route?`<div class='bar'><i style='width:${f.route.progress}%'></i></div>`:'';"
"const flag=f.cc?String.fromCodePoint(...[...f.cc].map(ch=>127397+ch.charCodeAt(0)))+' ':'';"
"return `<tr data-hex='${f.hex}'${em?' class=em':(f.interesting?' class=gold':'')}><td>${flag}<b>${f.callsign}</b>${f.flight_iata?` <span class=dim>${f.flight_iata}</span>`:''}${em?' \\u26A0 '+f.squawk:''}</td>"
"<td>${f.airline||'<span class=dim>-</span>'}</td><td>${f.type||''}</td><td>${rt}</td><td>${pr}</td>"
"<td class=r>${altc}</td><td class=r>${uspd(f.gs_kt)}</td><td class=r>${f.dist_km} km</td></tr>`;"
"}).join('');"
"document.querySelectorAll('#rows tr').forEach(tr=>tr.onclick=()=>selRow(tr.dataset.hex));"
"hiRow(selHex);"
"}catch(e){}}"
"document.addEventListener('keydown',async e=>{"
"if(e.target.id!=='c_city'||e.key!=='Enter')return;e.preventDefault();"
"const q=e.target.value.trim();if(!q)return;"
"const box=document.getElementById('c_cityres');box.textContent='searching...';"
"if(/^[a-z]{3,4}$/i.test(q)){try{const ar=await fetch('/api/airport?code='+q);"
"if(ar.ok){const ap=await ar.json();"
"document.getElementById('c_lat').value=ap.lat.toFixed(4);"
"document.getElementById('c_lon').value=ap.lon.toFixed(4);"
"document.getElementById('c_fixed').value='1';"
"box.textContent=`airport ${ap.icao}${ap.iata?' / '+ap.iata:''}: ${ap.city}`;return;}}catch(err){}}"
"try{const r=await fetch('https://geocoding-api.open-meteo.com/v1/search?name='+encodeURIComponent(q)+'&count=5&language='+((navigator.language||'en').slice(0,2)));"
"const d=await r.json();const res=d.results||[];"
"if(!res.length){box.textContent='no matches';return}"
"box.innerHTML=res.map((c,i)=>`<a href='#' data-i='${i}'>${c.name}, ${c.country_code} (${c.admin1||''})</a>`).join(' \u00B7 ');"
"box.querySelectorAll('a').forEach(a=>a.onclick=ev=>{ev.preventDefault();const c=res[+a.dataset.i];"
"document.getElementById('c_lat').value=c.latitude.toFixed(4);"
"document.getElementById('c_lon').value=c.longitude.toFixed(4);"
"document.getElementById('c_fixed').value='1';box.textContent=`set to ${c.name}`;});"
"}catch(err){box.textContent='search failed'}});"
"let favs=[[],[],[]];"
"function favRender(){const b=document.getElementById('favbox');"
"b.innerHTML=favs.map((f,i)=>f&&f.name?`<div>${f.name} (${(+f.lat).toFixed(2)}, ${(+f.lon).toFixed(2)}) `+"
"`<a href='#' data-a='use' data-i='${i}'>use</a> \u00B7 <a href='#' data-a='del' data-i='${i}'>clear</a></div>`"
":`<div>slot ${i+1}: <a href='#' data-a='save' data-i='${i}'>save current</a></div>`).join('');"
"b.querySelectorAll('a').forEach(a=>a.onclick=ev=>{ev.preventDefault();const i=+a.dataset.i;"
"if(a.dataset.a==='use'){document.getElementById('c_lat').value=(+favs[i].lat).toFixed(4);"
"document.getElementById('c_lon').value=(+favs[i].lon).toFixed(4);document.getElementById('c_fixed').value='1';}"
"else if(a.dataset.a==='del'){favs[i]={};favRender();}"
"else{const la=parseFloat(String(document.getElementById('c_lat').value).replace(',','.'));"
"const lo=parseFloat(String(document.getElementById('c_lon').value).replace(',','.'));"
"if(!isFinite(la)||!isFinite(lo)){alert('Set coordinates first');return;}"
"const nm=prompt('Name this location:','')||'';if(!nm)return;"
"favs[i]={name:nm,lat:la,lon:lo};favRender();}});}"
"async function restoreBk(){const f=document.getElementById('bkfile').files[0];"
"if(!f){document.getElementById('bkstat').textContent='pick a file first';return}"
"const t=await f.text();try{JSON.parse(t)}catch(e){document.getElementById('bkstat').textContent='not valid JSON';return}"
"document.getElementById('bkstat').textContent='restoring...';"
"try{const r=await fetch('/api/config',{method:'POST',body:t});"
"document.getElementById('bkstat').textContent=r.ok?'restored - device restarting':'restore failed';}"
"catch(e){document.getElementById('bkstat').textContent='restore failed'}}"
"const IN_ACTS=['next_view','prev_view','next_ac','prev_ac','zoom_in','zoom_out','wake','list_planes','list_ships','list_all','toggle_rain','follow_toggle','alt_min_up','alt_min_down','alt_max_up','alt_max_down','brightness_up','brightness_down','screensaver','screen_off','screen_on','screen_toggle','track_toggle'];"
"let inRows=[];"
"function inSel(v){return '<select class=\"in_a\">'+IN_ACTS.map(a=>`<option ${a===v?'selected':''}>${a}</option>`).join('')+'</select>';}"
"function inRender(){const el=document.getElementById('inrows');if(!el)return;el.innerHTML=inRows.map((r,i)=>"
"`<div class='inrow' data-i='${i}' style='margin:4px 0'>&#9881; <b>${r.ev}</b> ${(r.ev[0]==='e'||r.ev[0]==='q')?('CW '+inSel(r.a)+' CCW '+inSel(r.b||IN_ACTS[0])):inSel(r.a)}"
"${(r.ev[0]==='b'||r.ev[0]==='g')?` <select class='in_k'><option value='s' ${r.ev.endsWith('s')?'selected':''}>short</option><option value='l' ${r.ev.endsWith('l')?'selected':''}>long</option></select>`:''}"
" <button type='button' onclick='inRows.splice(${i},1);inRender()'>x</button></div>`).join('');}"
"function inCollect(){document.querySelectorAll('#inrows .inrow').forEach(d=>{const i=+d.dataset.i;const sels=d.querySelectorAll('.in_a');inRows[i].a=sels[0].value;if(sels[1])inRows[i].b=sels[1].value;const k=d.querySelector('.in_k');if(k)inRows[i].ev=inRows[i].ev.slice(0,-1)+k.value;});}"
"async function inLearn(){const st=document.getElementById('instat');const chip=document.getElementById('in_chip').value;"
"if(chip==='none'){st.textContent='pick a chip and save first';return;}"
"st.textContent='listening...';const pins=new Set();let lastEv='';"
"for(let i=0;i<15;i++){await new Promise(r=>setTimeout(r,400));"
"try{const r=await fetch('/api/input/last');const j=await r.json();"
"if(j.event&&j.age_ms<1200&&j.event!==lastEv){lastEv=j.event;if(j.event[0]==='b')pins.add(j.event.slice(1,-1));if(j.event[0]==='e')pins.add('E'+j.event.slice(1,-1));}}catch(e){}}"
"const arr=[...pins];"
"if(arr.length===0){st.textContent='nothing detected - is the current map saved and the expander wired?';return;}"
"if(arr.length===1&&arr[0][0]==='E'){inRows.push({ev:'e'+arr[0].slice(1)+',?',a:IN_ACTS[0],b:IN_ACTS[1]});st.textContent='encoder detected (fill pin B)';}"
"else if(arr.length===2){inRows.push({ev:'e'+arr[0].replace('E','')+','+arr[1].replace('E',''),a:IN_ACTS[0],b:IN_ACTS[1]});st.textContent='encoder pair detected';}"
"else{inRows.push({ev:'b'+arr[0].replace('E','')+'s',a:IN_ACTS[0]});st.textContent='button detected';}"
"inRender();}"
"function inManual(){const ev=prompt('Event code: b<pin>s / b<pin>l (expander button), e<pinA>,<pinB> (expander encoder), g<gpio>s / g<gpio>l (GPIO button), q<gpioA>,<gpioB> (GPIO encoder)','g17s');if(!ev)return;"
"if(/^[bg]\\d+[sl]$/.test(ev))inRows.push({ev:ev,a:IN_ACTS[0]});"
"else if(/^[eq]\\d+,\\d+$/.test(ev))inRows.push({ev:ev,a:IN_ACTS[0],b:IN_ACTS[1]});"
"else{document.getElementById('instat').textContent='bad event code';return;}inRender();}"
"function inParse(m){inRows=[];const chipEl=document.getElementById('in_chip');const addrEl=document.getElementById('in_addr');"
"if(!chipEl)return;chipEl.value='none';addrEl.value='20';if(!m)return void inRender();"
"const t=m.split(';');const h=t[0].match(/^(pcf|mcp)@([0-9a-fA-F]+)$/);"
"if(h){t.shift();chipEl.value=h[1];addrEl.value=h[2];}"
"t.forEach(e=>{let mm=e.match(/^([bg]\\d+[sl])=(.+)$/);if(mm){inRows.push({ev:mm[1],a:mm[2]});return;}"
"mm=e.match(/^([eq]\\d+,\\d+)=([^,]+),(.+)$/);if(mm)inRows.push({ev:mm[1],a:mm[2],b:mm[3]});});inRender();}"
"function inSerialize(){inCollect();const chip=document.getElementById('in_chip').value;"
"if(inRows.length===0)return '';"
"const ent=inRows.filter(r=>chip!=='none'||r.ev[0]==='g'||r.ev[0]==='q').map(r=>(r.ev[0]==='e'||r.ev[0]==='q')?`${r.ev}=${r.a},${r.b}`:`${r.ev}=${r.a}`);"
"if(ent.length===0)return '';"
"if(chip==='none')return ent.join(';');"
"const addr=(document.getElementById('in_addr').value||'20').toLowerCase();"
"return chip+'@'+addr+';'+ent.join(';');}"
"async function loadCfg(){try{const r=await fetch('/api/config');const c=await r.json();"
"tempF=c.temp_f===true;"
"for(const k in c){const el=document.getElementById('c_'+k);if(!el)continue;"
"if(el.tagName==='SELECT')el.value=(c[k]===true||c[k]===1||c[k]==='1')?1:( +c[k]||0);else el.value=c[k];}"
"document.getElementById('c_theme').value=c.theme;document.getElementById('c_lang').value=c.lang;"
"const cm=c.show_classes==null?31:c.show_classes;for(let i=0;i<5;i++)document.getElementById('c_cls'+i).checked=!!(cm>>i&1);"
"const mm=v=>`${String(Math.floor(v/60)).padStart(2,'0')}:${String(v%60).padStart(2,'0')}`;"
"document.getElementById('c_night_start').value=mm(c.night_start_min||1380);"
"document.getElementById('c_night_end').value=mm(c.night_end_min||390);"
"document.getElementById('c_amb_from').value=mm(c.amb_from_min==null?1320:c.amb_from_min);"
"document.getElementById('c_amb_to').value=mm(c.amb_to_min==null?420:c.amb_to_min);"
"if(c.fa_used!=null)document.getElementById('fa_usage').textContent=`Used this month: ${c.fa_used} of ${c.fa_limit} calls, about $${(c.fa_used*0.005).toFixed(2)} at list price. Lookups stop at the budget and are spread evenly over the month; 900 stays inside the free Personal credit.`;"
"favs=(c.favs||[]).map(f=>f&&f.name?f:{});favRender();"
"inParse(c.input_map||'');"
"if(c.local_adsb_use&&c.local_merge)document.getElementById('c_local_adsb_use').value='2';"
"document.getElementById('cfgsave').disabled=false;}catch(e){}}"
"async function saveCfg(){const c={};"
"['ssid','pass','web_pass','ntfy_topic','mqtt_uri','fa_key','watch_regs','webhook_url','local_adsb','filter_airport','openaip_key','ais_key','carto_key','tile_url'].forEach(k=>{const v=document.getElementById('c_'+k).value;if((k!=='pass'&&k!=='web_pass')||v)c[k]=v;});"
"c.cpa_alerts=document.getElementById('c_cpa_alerts').value==='1';"
"c.cpa_all=document.getElementById('c_cpa_all').value==='1';"
"c.filter_apt_exclude=document.getElementById('c_filter_apt_exclude').value==='1';"
"c.night_enabled=document.getElementById('c_night_enabled').value==='1';"
"c.night_auto=document.getElementById('c_night_auto').value==='1';"
"c.ambient_idle_min=+document.getElementById('c_ambient_idle_min').value;"
"c.alt_min_ft=+document.getElementById('c_alt_min_ft').value;c.alt_max_ft=+document.getElementById('c_alt_max_ft').value;"
"const pm=id=>{const v=document.getElementById(id).value.split(':');return (+v[0])*60+(+v[1]||0);};"
"c.night_start_min=pm('c_night_start');c.night_end_min=pm('c_night_end');"
"c.amb_from_min=pm('c_amb_from');c.amb_to_min=pm('c_amb_to');"
"c.amb_sched=document.getElementById('c_amb_sched').value==='1';"
"c.pin_nearest=document.getElementById('c_pin_nearest').value==='1';"
"c.fa_limit=Math.min(20000,Math.max(10,+document.getElementById('c_fa_limit').value||900));"
"c.fixed=document.getElementById('c_fixed').value==='1';"
"c.hide_ground=document.getElementById('c_hide_ground').value==='1';"
"c.show_classes=0;for(let i=0;i<5;i++)if(document.getElementById('c_cls'+i).checked)c.show_classes|=1<<i;"
"c.rain_overlay=document.getElementById('c_rain_overlay').value==='1';"
"c.show_route=document.getElementById('c_show_route').value==='1';"
"c.map_light=document.getElementById('c_map_light').value==='1';"
"c.retro_map=document.getElementById('c_retro_map').value==='1';"
"c.clock_12h=document.getElementById('c_clock_12h').value==='1';"
"c.brightness_ctl=document.getElementById('c_brightness_ctl').value==='1';"
"c.brightness=Math.min(100,Math.max(5,+document.getElementById('c_brightness').value||100));"
"c.input_map=inSerialize();"
"{const lv=document.getElementById('c_local_adsb_use').value;c.local_adsb_use=lv!=='0';c.local_merge=lv==='2';}"
"['taf','iss','sonde','ships','airspace'].forEach(k=>c[k+'_enabled']=document.getElementById('c_'+k+'_enabled').value==='1');"
"['metric_units','metar_decoded','follow_mode','temp_f'].forEach(k=>c[k]=document.getElementById('c_'+k).value==='1');"
"c.favs=favs.map(f=>f&&f.name?f:{name:'',lat:0,lon:0});"
"c.amb_style=+document.getElementById('c_amb_style').value;"
"const num=v=>parseFloat(String(v).replace(',','.'));"
"const la=num(document.getElementById('c_lat').value),lo=num(document.getElementById('c_lon').value);"
"if(isFinite(la)&&Math.abs(la)<=90)c.lat=la;if(isFinite(lo)&&Math.abs(lo)<=180)c.lon=lo;"
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
"async function trkLoad(){try{const r=await fetch('/api/track');const t=await r.json();"
"const st=document.getElementById('trk_st');st.textContent=t.on?'tracking '+t.id:'off';st.classList.toggle('on',t.on);"
"const i=document.getElementById('trk_id');if(document.activeElement!==i&&t.id)i.value=t.id;}catch(e){}}"
"async function trkSet(on){const id=document.getElementById('trk_id').value.trim().toUpperCase();"
"await fetch('/api/track',{method:'POST',body:JSON.stringify(on?{id:id,on:true}:{on:false})});trkLoad();}"
"function trkSel(){const f=((lastd&&lastd.flights)||[]).find(x=>x.hex===selHex);"
"if(f)document.getElementById('trk_id').value=(f.callsign||f.reg||f.hex).trim();}"
"(async()=>{await load();await loadCfg();await trkLoad();})();setInterval(load,5000);setInterval(trkLoad,15000);"
"async function ota(){const f=document.getElementById('fw').files[0];if(!f)return;"
"const st=document.getElementById('otastat');st.textContent='uploading...';"
"try{const r=await fetch('/ota',{method:'POST',body:f});"
"st.textContent=await r.text();}catch(e){st.textContent='device rebooting...'}}"
"</script></body></html>";

/* Screenshot: RGB565 framebuffer as a 16bpp BI_BITFIELDS BMP (top-down) */
static esp_err_t screen_get(httpd_req_t *req)
{
    AUTH_GUARD(req);
    int W, H;
    waveshare_lcd_get_res(&W, &H);
    const uint32_t data_size = W * H * 2;
    uint8_t *fb = waveshare_lcd_get_fb();
    if (fb == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no framebuffer");
    }

    /* streamed in bands: a full-frame snapshot needs 768 KB contiguous,
       which is exactly what a busy PSRAM cannot promise */
    enum { SNAP_BAND_ROWS = 24 };
    uint8_t *snap = heap_caps_malloc((size_t)W * SNAP_BAND_ROWS * 2,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (snap == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
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
    for (int y = 0; y < H; y += SNAP_BAND_ROWS) {
        int rows = H - y < SNAP_BAND_ROWS ? H - y : SNAP_BAND_ROWS;
        size_t n = (size_t)W * rows * 2;
        if (lvgl_port_lock(1000)) {
            memcpy(snap, fb + (size_t)y * W * 2, n);
            lvgl_port_unlock();
        }
        if (httpd_resp_send_chunk(req, (char *)snap, n) != ESP_OK) {
            break;
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);
    free(snap);
    return ESP_OK;
}

static esp_err_t root_get(httpd_req_t *req)
{
    AUTH_GUARD(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static int64_t s_state_access_us;

bool web_state_wanted(void)
{
    return esp_timer_get_time() - s_state_access_us < 300LL * 1000 * 1000;
}

static esp_err_t api_get(httpd_req_t *req)
{
    AUTH_GUARD(req);
    /* only authenticated pollers keep the JSON build alive */
    s_state_access_us = esp_timer_get_time();
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
    AUTH_GUARD(req);
    const settings_t *c = settings_get();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", c->wifi_ssid);
    cJSON_AddBoolToObject(root, "fixed", c->use_fixed_loc);
    cJSON_AddNumberToObject(root, "lat", c->lat);
    cJSON_AddNumberToObject(root, "lon", c->lon);
    cJSON_AddNumberToObject(root, "radius_nm", c->radius_nm);
    cJSON_AddBoolToObject(root, "hide_ground", c->hide_ground);
    cJSON_AddNumberToObject(root, "show_classes", c->show_classes);
    cJSON_AddBoolToObject(root, "rain_overlay", c->rain_overlay);
    cJSON_AddBoolToObject(root, "show_route", c->show_route);
    cJSON_AddBoolToObject(root, "clock_12h", c->clock_12h);
    cJSON_AddNumberToObject(root, "brightness", c->brightness);
    cJSON_AddBoolToObject(root, "brightness_ctl", c->brightness_ctl);
    cJSON_AddBoolToObject(root, "map_light", c->map_light);
    cJSON_AddBoolToObject(root, "retro_map", c->retro_map);
    cJSON_AddNumberToObject(root, "amb_style", c->amb_style);
    {
        int used, limit;
        faflight_usage(&used, &limit);
        cJSON_AddNumberToObject(root, "fa_limit", limit);
        cJSON_AddNumberToObject(root, "fa_used", used);
    }
    cJSON_AddBoolToObject(root, "pin_nearest", c->pin_nearest);
    cJSON_AddBoolToObject(root, "amb_sched", c->amb_sched);
    cJSON_AddNumberToObject(root, "amb_from_min", c->amb_from_min);
    cJSON_AddNumberToObject(root, "amb_to_min", c->amb_to_min);
    cJSON_AddNumberToObject(root, "theme", c->theme);
    cJSON_AddNumberToObject(root, "lang", c->lang);
    cJSON_AddStringToObject(root, "ntfy_topic", c->ntfy_topic);
    cJSON_AddStringToObject(root, "mqtt_uri", c->mqtt_uri);
    cJSON_AddStringToObject(root, "fa_key", c->fa_key);
    cJSON_AddStringToObject(root, "watch_regs", c->watch_regs);
    cJSON_AddStringToObject(root, "webhook_url", c->webhook_url);
    cJSON_AddStringToObject(root, "local_adsb", c->local_adsb);
    cJSON_AddBoolToObject(root, "local_adsb_use", c->local_adsb_use);
    cJSON_AddBoolToObject(root, "local_merge", c->local_merge);
    cJSON_AddBoolToObject(root, "web_pass_set", c->web_pass[0] != '\0');
    cJSON_AddBoolToObject(root, "cpa_alerts", c->cpa_alerts);
    cJSON_AddBoolToObject(root, "cpa_all", c->cpa_all);
    cJSON_AddBoolToObject(root, "night_enabled", c->night_enabled);
    cJSON_AddBoolToObject(root, "night_auto", c->night_auto);
    cJSON_AddNumberToObject(root, "night_start_min", c->night_start_min);
    cJSON_AddNumberToObject(root, "night_end_min", c->night_end_min);
    cJSON_AddNumberToObject(root, "ambient_idle_min", c->ambient_idle_min);
    cJSON_AddStringToObject(root, "filter_airport", c->filter_airport);
    cJSON_AddBoolToObject(root, "filter_apt_exclude", c->filter_apt_exclude);
    cJSON_AddNumberToObject(root, "alt_min_ft", c->alt_min_ft);
    cJSON_AddNumberToObject(root, "alt_max_ft", c->alt_max_ft);
    cJSON_AddBoolToObject(root, "taf_enabled", c->taf_enabled);
    cJSON_AddBoolToObject(root, "iss_enabled", c->iss_enabled);
    cJSON_AddBoolToObject(root, "sonde_enabled", c->sonde_enabled);
    cJSON_AddBoolToObject(root, "ships_enabled", c->ships_enabled);
    cJSON_AddBoolToObject(root, "airspace_enabled", c->airspace_enabled);
    cJSON_AddStringToObject(root, "openaip_key", c->openaip_key);
    cJSON_AddStringToObject(root, "ais_key", c->ais_key);
    cJSON_AddStringToObject(root, "carto_key", c->carto_key);
    cJSON_AddStringToObject(root, "tile_url", c->tile_url);
    cJSON_AddStringToObject(root, "input_map", c->input_map);
    cJSON_AddBoolToObject(root, "metric_units", c->metric_units);
    cJSON_AddBoolToObject(root, "temp_f", c->temp_f);
    cJSON_AddBoolToObject(root, "metar_decoded", c->metar_decoded);
    cJSON_AddBoolToObject(root, "follow_mode", c->follow_mode);
    cJSON *jf = cJSON_AddArrayToObject(root, "favs");
    for (int f = 0; f < 3; f++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", c->fav_name[f]);
        cJSON_AddNumberToObject(e, "lat", c->fav_lat[f]);
        cJSON_AddNumberToObject(e, "lon", c->fav_lon[f]);
        cJSON_AddItemToArray(jf, e);
    }
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


/* Full configuration snapshot, secrets included: the restore path is
 * simply POSTing this JSON back to /api/config. Keep keys in sync with
 * config_post. */
static esp_err_t backup_get(httpd_req_t *req)
{
    AUTH_GUARD(req);
    const settings_t *c = settings_get();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", c->wifi_ssid);
    cJSON_AddStringToObject(root, "pass", c->wifi_pass);
    cJSON_AddStringToObject(root, "web_pass", c->web_pass);
    cJSON_AddStringToObject(root, "ntfy_topic", c->ntfy_topic);
    cJSON_AddStringToObject(root, "mqtt_uri", c->mqtt_uri);
    cJSON_AddStringToObject(root, "fa_key", c->fa_key);
    cJSON_AddStringToObject(root, "watch_regs", c->watch_regs);
    cJSON_AddStringToObject(root, "webhook_url", c->webhook_url);
    cJSON_AddStringToObject(root, "local_adsb", c->local_adsb);
    cJSON_AddBoolToObject(root, "local_adsb_use", c->local_adsb_use);
    cJSON_AddBoolToObject(root, "local_merge", c->local_merge);
    cJSON_AddStringToObject(root, "filter_airport", c->filter_airport);
    cJSON_AddStringToObject(root, "openaip_key", c->openaip_key);
    cJSON_AddStringToObject(root, "ais_key", c->ais_key);
    cJSON_AddStringToObject(root, "carto_key", c->carto_key);
    cJSON_AddStringToObject(root, "tile_url", c->tile_url);
    cJSON_AddStringToObject(root, "input_map", c->input_map);
    cJSON_AddBoolToObject(root, "fixed", c->use_fixed_loc);
    cJSON_AddNumberToObject(root, "lat", c->lat);
    cJSON_AddNumberToObject(root, "lon", c->lon);
    cJSON_AddNumberToObject(root, "radius_nm", c->radius_nm);
    cJSON_AddNumberToObject(root, "theme", c->theme);
    cJSON_AddNumberToObject(root, "lang", c->lang);
    cJSON_AddBoolToObject(root, "rain_overlay", c->rain_overlay);
    cJSON_AddBoolToObject(root, "show_route", c->show_route);
    cJSON_AddBoolToObject(root, "clock_12h", c->clock_12h);
    cJSON_AddNumberToObject(root, "brightness", c->brightness);
    cJSON_AddBoolToObject(root, "brightness_ctl", c->brightness_ctl);
    cJSON_AddBoolToObject(root, "map_light", c->map_light);
    cJSON_AddBoolToObject(root, "retro_map", c->retro_map);
    cJSON_AddNumberToObject(root, "amb_style", c->amb_style);
    cJSON_AddNumberToObject(root, "fa_limit", c->fa_limit);
    cJSON_AddBoolToObject(root, "pin_nearest", c->pin_nearest);
    cJSON_AddBoolToObject(root, "amb_sched", c->amb_sched);
    cJSON_AddNumberToObject(root, "amb_from_min", c->amb_from_min);
    cJSON_AddNumberToObject(root, "amb_to_min", c->amb_to_min);
    cJSON_AddBoolToObject(root, "hide_ground", c->hide_ground);
    cJSON_AddNumberToObject(root, "show_classes", c->show_classes);
    cJSON_AddBoolToObject(root, "cpa_alerts", c->cpa_alerts);
    cJSON_AddBoolToObject(root, "cpa_all", c->cpa_all);
    cJSON_AddBoolToObject(root, "filter_apt_exclude", c->filter_apt_exclude);
    cJSON_AddBoolToObject(root, "night_enabled", c->night_enabled);
    cJSON_AddBoolToObject(root, "night_auto", c->night_auto);
    cJSON_AddNumberToObject(root, "night_start_min", c->night_start_min);
    cJSON_AddNumberToObject(root, "night_end_min", c->night_end_min);
    cJSON_AddNumberToObject(root, "ambient_idle_min", c->ambient_idle_min);
    cJSON_AddNumberToObject(root, "alt_min_ft", c->alt_min_ft);
    cJSON_AddNumberToObject(root, "alt_max_ft", c->alt_max_ft);
    cJSON_AddBoolToObject(root, "taf_enabled", c->taf_enabled);
    cJSON_AddBoolToObject(root, "iss_enabled", c->iss_enabled);
    cJSON_AddBoolToObject(root, "sonde_enabled", c->sonde_enabled);
    cJSON_AddBoolToObject(root, "ships_enabled", c->ships_enabled);
    cJSON_AddBoolToObject(root, "airspace_enabled", c->airspace_enabled);
    cJSON_AddBoolToObject(root, "metric_units", c->metric_units);
    cJSON_AddBoolToObject(root, "temp_f", c->temp_f);
    cJSON_AddBoolToObject(root, "metar_decoded", c->metar_decoded);
    cJSON_AddBoolToObject(root, "follow_mode", c->follow_mode);
    cJSON *favs = cJSON_AddArrayToObject(root, "favs");
    for (int f = 0; f < 3; f++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", c->fav_name[f]);
        cJSON_AddNumberToObject(e, "lat", c->fav_lat[f]);
        cJSON_AddNumberToObject(e, "lon", c->fav_lon[f]);
        cJSON_AddItemToArray(favs, e);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"esp32flight-backup.json\"");
    esp_err_t err = httpd_resp_send(req, json ? json : "{}", HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

static esp_err_t config_post(httpd_req_t *req)
{
    AUTH_GUARD(req);
    static char body[4096];   /* config posts and full backup restores */
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
    set_str_field(root, "web_pass", c->web_pass, sizeof(c->web_pass));
    set_str_field(root, "filter_airport", c->filter_airport, sizeof(c->filter_airport));
    const cJSON *j;
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "fixed")))) {
        c->use_fixed_loc = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "rain_overlay")))) {
        c->rain_overlay = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "show_route")))) {
        c->show_route = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "clock_12h")))) {
        c->clock_12h = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "brightness_ctl")))) {
        c->brightness_ctl = cJSON_IsTrue(j);
    }
    if (cJSON_IsNumber((j = cJSON_GetObjectItem(root, "brightness")))) {
        int b = j->valueint;
        c->brightness = (uint8_t)(b < 5 ? 5 : b > 100 ? 100 : b);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "map_light")))) {
        c->map_light = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "local_adsb_use")))) {
        c->local_adsb_use = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "local_merge")))) {
        c->local_merge = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "retro_map")))) {
        c->retro_map = cJSON_IsTrue(j);
    }
    if (cJSON_IsNumber((j = cJSON_GetObjectItem(root, "amb_style")))) {
        c->amb_style = j->valueint == 1 ? 1 : 0;
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "hide_ground")))) {
        c->hide_ground = cJSON_IsTrue(j);
    }
    if (cJSON_IsNumber((j = cJSON_GetObjectItem(root, "show_classes")))) {
        c->show_classes = (uint8_t)j->valueint & FCLS_ALL_MASK;
        if (c->show_classes == 0) {
            c->show_classes = FCLS_ALL_MASK;   /* nothing = everything */
        }
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "cpa_alerts")))) {
        c->cpa_alerts = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "cpa_all")))) {
        c->cpa_all = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "filter_apt_exclude")))) {
        c->filter_apt_exclude = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "night_enabled")))) {
        c->night_enabled = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "night_auto")))) {
        c->night_auto = cJSON_IsTrue(j);
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
    if (cJSON_IsNumber((j = cJSON_GetObjectItem(root, "alt_min_ft")))) {
        c->alt_min_ft = (int)j->valuedouble;
    }
    if (cJSON_IsNumber((j = cJSON_GetObjectItem(root, "alt_max_ft")))) {
        c->alt_max_ft = (int)j->valuedouble;
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "taf_enabled")))) {
        c->taf_enabled = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "iss_enabled")))) {
        c->iss_enabled = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "sonde_enabled")))) {
        c->sonde_enabled = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "ships_enabled")))) {
        c->ships_enabled = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "airspace_enabled")))) {
        c->airspace_enabled = cJSON_IsTrue(j);
    }
    set_str_field(root, "openaip_key", c->openaip_key, sizeof(c->openaip_key));
    set_str_field(root, "ais_key", c->ais_key, sizeof(c->ais_key));
    set_str_field(root, "carto_key", c->carto_key, sizeof(c->carto_key));
    set_str_field(root, "tile_url", c->tile_url, sizeof(c->tile_url));
    set_str_field(root, "input_map", c->input_map, sizeof(c->input_map));
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "metric_units")))) {
        c->metric_units = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "temp_f")))) {
        c->temp_f = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "metar_decoded")))) {
        c->metar_decoded = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "follow_mode")))) {
        c->follow_mode = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "pin_nearest")))) {
        c->pin_nearest = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool((j = cJSON_GetObjectItem(root, "amb_sched")))) {
        c->amb_sched = cJSON_IsTrue(j);
    }
    if (cJSON_IsNumber((j = cJSON_GetObjectItem(root, "amb_from_min"))) &&
        j->valuedouble >= 0 && j->valuedouble < 1440) {
        c->amb_from_min = (int)j->valuedouble;
    }
    if (cJSON_IsNumber((j = cJSON_GetObjectItem(root, "amb_to_min"))) &&
        j->valuedouble >= 0 && j->valuedouble < 1440) {
        c->amb_to_min = (int)j->valuedouble;
    }
    if (cJSON_IsNumber((j = cJSON_GetObjectItem(root, "fa_limit"))) &&
        j->valuedouble >= 10 && j->valuedouble <= 20000) {
        c->fa_limit = (uint16_t)j->valuedouble;
    }
    const cJSON *jfav = cJSON_GetObjectItem(root, "favs");
    if (cJSON_IsArray(jfav)) {
        for (int f = 0; f < 3; f++) {
            const cJSON *e = cJSON_GetArrayItem((cJSON *)jfav, f);
            const cJSON *nm = e ? cJSON_GetObjectItem(e, "name") : NULL;
            const cJSON *la = e ? cJSON_GetObjectItem(e, "lat") : NULL;
            const cJSON *lo = e ? cJSON_GetObjectItem(e, "lon") : NULL;
            if (cJSON_IsString(nm) && nm->valuestring[0] &&
                cJSON_IsNumber(la) && cJSON_IsNumber(lo) &&
                la->valuedouble >= -90 && la->valuedouble <= 90 &&
                lo->valuedouble >= -180 && lo->valuedouble <= 180) {
                strlcpy(c->fav_name[f], nm->valuestring, sizeof(c->fav_name[f]));
                c->fav_lat[f] = la->valuedouble;
                c->fav_lon[f] = lo->valuedouble;
            } else {
                c->fav_name[f][0] = '\0';
            }
        }
    }
    if (cJSON_IsNumber((j = cJSON_GetObjectItem(root, "lat"))) &&
        j->valuedouble >= -90.0 && j->valuedouble <= 90.0) {
        c->lat = j->valuedouble;
    }
    if (cJSON_IsNumber((j = cJSON_GetObjectItem(root, "lon"))) &&
        j->valuedouble >= -180.0 && j->valuedouble <= 180.0) {
        c->lon = j->valuedouble;
    }
    if (cJSON_IsNumber((j = cJSON_GetObjectItem(root, "radius_nm")))) {
        int r = (int)j->valuedouble;
        if (r >= 1 && r <= 250) {
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

    ESP_LOGI(TAG, "config POST: lang=%d theme=%d fixed=%d radius=%d",
             c->lang, c->theme, c->use_fixed_loc, c->radius_nm);
    settings_save();
    httpd_resp_sendstr(req, "saved - restarting");
    ESP_LOGI(TAG, "config updated from web, restarting");
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return ESP_OK;
}

static esp_err_t log_get(httpd_req_t *req)
{
    AUTH_GUARD(req);
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

static esp_err_t alerts_get(httpd_req_t *req)
{
    AUTH_GUARD(req);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    const char *paths[2] = { ALERTLOG_OLD_PATH, ALERTLOG_PATH };
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

#ifndef APKFLIGHT
/* airport-code location lookup for the panel (ICAO or IATA) */
static esp_err_t airport_get(httpd_req_t *req)
{
    AUTH_GUARD(req);
    char q[32] = "", code[8] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "code", code, sizeof(code)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "code=EPGD or GDN");
    }
    airport_t ap;
    if (!airports_lookup_any(code, &ap)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown airport");
    }
    char json[256];
    snprintf(json, sizeof(json),
             "{\"icao\":\"%s\",\"iata\":\"%s\",\"city\":\"%s\","
             "\"lat\":%.6f,\"lon\":%.6f}",
             ap.icao, ap.iata, ap.city, ap.lat, ap.lon);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

/* undocumented helper for remote screenshots: /view?m=N switches the view */
/* #13: named input actions for custom decks and the expander mapper.
 * GET /api/input?action=zoom_in or POST {"action":"zoom_in"}. */
static esp_err_t input_req(httpd_req_t *req)
{
    AUTH_GUARD(req);
    char act[32] = "";
    if (req->method == HTTP_POST) {
        char body[96];
        int n = httpd_req_recv(req, body, sizeof(body) - 1);
        if (n > 0) {
            body[n] = '\0';
            cJSON *root = cJSON_Parse(body);
            if (root != NULL) {
                const cJSON *j = cJSON_GetObjectItem(root, "action");
                if (cJSON_IsString(j)) {
                    strlcpy(act, j->valuestring, sizeof(act));
                }
                cJSON_Delete(root);
            }
        }
    }
    if (act[0] == '\0') {
        char q[64] = "";
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
            httpd_query_key_value(q, "action", act, sizeof(act));
        }
    }
    if (act[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "action required");
    }
    if (!input_ctl_dispatch(act)) {
        httpd_resp_set_status(req, "422 Unprocessable Entity");
        httpd_resp_set_type(req, "application/json");
        char out[300];
        snprintf(out, sizeof(out), "{\"error\":\"unknown action\",\"actions\":\"%s\"}",
                 input_ctl_actions());
        return httpd_resp_sendstr(req, out);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* Copy keeping only characters that need no JSON escaping. */
static void json_escape_min(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (; *in && o + 1 < n; in++) {
        if (*in != '"' && *in != '\\' && (unsigned char)*in >= 0x20) {
            out[o++] = *in;
        }
    }
    out[o] = '\0';
}

/* Track mode, applied live (no restart): GET -> {"id","on"},
 * POST {"id":"LOT3AB","on":true}; an empty id switches tracking off. */
static esp_err_t track_req(httpd_req_t *req)
{
    AUTH_GUARD(req);
    settings_t *c = settings_get();
    if (req->method == HTTP_POST) {
        char body[128];
        int n = httpd_req_recv(req, body, sizeof(body) - 1);
        if (n <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body required");
        }
        body[n] = '\0';
        cJSON *root = cJSON_Parse(body);
        if (root == NULL) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad JSON");
        }
        const cJSON *j = cJSON_GetObjectItem(root, "id");
        if (cJSON_IsString(j)) {
            strlcpy(c->track_id, j->valuestring, sizeof(c->track_id));
            c->track_on = c->track_id[0] != '\0';
        }
        j = cJSON_GetObjectItem(root, "on");
        if (cJSON_IsBool(j)) {
            c->track_on = cJSON_IsTrue(j) && c->track_id[0] != '\0';
        }
        cJSON_Delete(root);
        settings_save();
    }
    char out[96];
    char id[24];
    json_escape_min(c->track_id, id, sizeof(id));
    snprintf(out, sizeof(out), "{\"id\":\"%s\",\"on\":%s}", id, c->track_on ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

/* learn mode: last physical event seen by the expander poller */
static esp_err_t input_last_get(httpd_req_t *req)
{
    AUTH_GUARD(req);
    char ev[8];
    unsigned age = 0;
    httpd_resp_set_type(req, "application/json");
    if (!input_ctl_last_event(ev, sizeof(ev), &age)) {
        return httpd_resp_sendstr(req, "{}");
    }
    char out[64];
    snprintf(out, sizeof(out), "{\"event\":\"%s\",\"age_ms\":%u}", ev, age);
    return httpd_resp_sendstr(req, out);
}

static esp_err_t view_get(httpd_req_t *req)
{
    AUTH_GUARD(req);
    char q[32] = "", val[8] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "m", val, sizeof(val)) == ESP_OK) {
        int m = atoi(val);
        if (lvgl_port_lock(1000)) {
            if (m == 30) {
                ui_set_update_available(true, "v9.9.9");   /* test helper */
            } else if (m >= 20 && m <= 24) {
                ui_settings_show_tab(m - 20);   /* screenshot helper */
            } else if (m >= 10 && m <= 12) {
                ui_set_list_mode(m - 10);   /* 10 planes, 11 ships, 12 all */
            } else {
                ui_set_view(m);
            }
            lvgl_port_unlock();
        }
        return httpd_resp_sendstr(req, "ok");
    }
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "m=0..6");
}
#endif

static esp_err_t ota_post(httpd_req_t *req)
{
    AUTH_GUARD(req);
    if (esp_ota_get_next_update_partition(NULL) == NULL) {
        /* 4 MB flash class: one factory slot, nowhere to stage an update */
        ESP_LOGW(TAG, "OTA rejected: no spare app slot on this flash layout");
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "this board has no OTA slots (4 MB flash) - "
                                   "update from the web flasher, settings survive");
    }
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
    bool checked = false;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < 4096 ? remaining : 4096);
        if (n <= 0) {
            free(buf);
            esp_ota_abort(ota);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        }
        if (!checked && req->content_len - remaining + n > 512) {
            /* the app descriptor sits early in the image; refuse a binary
               built for a different board variant before writing further.
               Variant = the part of the version after the first dash. */
            checked = true;
            const esp_app_desc_t *inc =
                (const esp_app_desc_t *)(buf + sizeof(esp_image_header_t)
                                             + sizeof(esp_image_segment_header_t));
            if (inc->magic_word == ESP_APP_DESC_MAGIC_WORD) {
                const char *run_v = esp_app_get_description()->version;
                const char *inc_v = inc->version;
                const char *rd = strchr(run_v, '-');
                const char *id = strchr(inc_v, '-');
                bool same = (rd == NULL && id == NULL) ||
                            (rd != NULL && id != NULL && strcmp(rd, id) == 0);
                if (!same) {
                    char msg[160];
                    snprintf(msg, sizeof(msg),
                             "wrong build for this board: file is %s, device runs %s",
                             inc_v, run_v);
                    ESP_LOGW(TAG, "OTA rejected: %s", msg);
                    free(buf);
                    esp_ota_abort(ota);
                    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
                }
            }
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

static struct {
    int count, unique, alt;
    double nearest_km;
} s_metric;

void web_metrics_publish(int count, int unique, int max_alt_ft, double nearest_km)
{
    s_metric.count = count;
    s_metric.unique = unique;
    s_metric.alt = max_alt_ft;
    s_metric.nearest_km = nearest_km;
}

static esp_err_t metrics_get(httpd_req_t *req)
{
    AUTH_GUARD(req);
    /* scalars pushed by flight_task: no JSON round-trip per scrape */
    char out[512];
    int count = s_metric.count, unique = s_metric.unique, alt = s_metric.alt;
    double nearest = s_metric.nearest_km;
    snprintf(out, sizeof(out),
             "esp32flight_aircraft_count %d\n"
             "esp32flight_unique_aircraft %d\n"
             "esp32flight_max_alt_ft %d\n"
             "esp32flight_nearest_km %.1f\n"
             "esp32flight_heap_free_bytes %u\n"
             "esp32flight_internal_heap_free_bytes %u\n"
             "esp32flight_internal_heap_total_bytes %u\n"
             "esp32flight_internal_heap_largest_block %u\n"
             "esp32flight_psram_free_bytes %u\n"
             "esp32flight_psram_total_bytes %u\n"
             "esp32flight_internal_heap_min_free_bytes %u\n"
             "esp32flight_psram_min_free_bytes %u\n"
             "esp32flight_reset_reason %d\n"
             "esp32flight_uptime_seconds %lld\n",
             count, unique, alt, nearest,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
             (int)esp_reset_reason(),
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
#ifndef APKFLIGHT
    config.core_id = 0;   /* keep TLS/JSON chatter off the LVGL core */
#endif
    config.max_uri_handlers = 24;
    /* httpd took 7 of only 10 lwIP sockets: one open panel tab
     * (keep-alive + 5 s polling) left nothing for the device's own TLS
     * fetches, and tiles, routes and weather failed together (#35). lwIP
     * now has 16 (sdkconfig): 6 for the panel + 3 httpd internals leaves
     * 7 for outbound. Capping the panel at 4 instead dropped a browser's
     * parallel requests. */
#ifndef APKFLIGHT
    config.max_open_sockets = 6;
    config.lru_purge_enable = true;
#endif
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
        { .uri = "/api/backup", .method = HTTP_GET, .handler = backup_get },
        { .uri = "/api/log", .method = HTTP_GET, .handler = log_get },
        { .uri = "/api/alerts", .method = HTTP_GET, .handler = alerts_get },
        { .uri = "/screen.bmp", .method = HTTP_GET, .handler = screen_get },
#ifndef APKFLIGHT
        { .uri = "/view", .method = HTTP_GET, .handler = view_get },
        { .uri = "/api/airport", .method = HTTP_GET, .handler = airport_get },
        { .uri = "/api/input", .method = HTTP_GET, .handler = input_req },
        { .uri = "/api/input", .method = HTTP_POST, .handler = input_req },
        { .uri = "/api/input/last", .method = HTTP_GET, .handler = input_last_get },
        { .uri = "/api/track", .method = HTTP_GET, .handler = track_req },
        { .uri = "/api/track", .method = HTTP_POST, .handler = track_req },
#endif
        { .uri = "/metrics", .method = HTTP_GET, .handler = metrics_get },
        { .uri = "/ota", .method = HTTP_POST, .handler = ota_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    ESP_LOGI(TAG, "web panel at http://esp32flight.local/");
}
