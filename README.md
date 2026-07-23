<h1 align="center">esp32flight</h1>

<p align="center">
  A standalone desk flight radar on a single ESP32 board with a 7" touchscreen.<br>
  Live aircraft around you: airline logos, routes, maps, alerts and a built-in web panel.<br>
  No Raspberry Pi, no server, no subscriptions, no API keys.
</p>

<p align="center">
  <a href="https://theqkash.github.io/esp32flight/"><img src="https://img.shields.io/badge/flash-from%20your%20browser-4da3ff?style=flat-square" alt="browser flasher"></a>
  <a href="https://github.com/theqkash/esp32flight/releases"><img src="https://img.shields.io/github/v/release/theqkash/esp32flight?style=flat-square&color=2ea44f" alt="latest release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue?style=flat-square" alt="MIT"></a>
  <a href="https://ko-fi.com/theqkash"><img src="https://img.shields.io/badge/ko--fi-support%20the%20project-ff5e5b?style=flat-square&logo=kofi&logoColor=white" alt="Ko-fi"></a>
</p>

---

| | |
|---|---|
| ![flight details](docs/detail-light.png) | ![forest theme](docs/detail-forest.png) |
| ![ambient mode with map](docs/ambient-mode.png) | ![radar view](docs/radar.png) |
| ![route map](docs/route-map.png) | ![aircraft photo](docs/photo.png) |

## Quick start

1. Get a **Waveshare ESP32-S3-Touch-LCD-7** (~$35) and a USB cable.
2. Open **[the browser installer](https://theqkash.github.io/esp32flight/)** in Chrome or Edge, click *Install*, pick the serial port. Done in a few minutes.
3. Tap the gear icon, pick your 2.4 GHz Wi-Fi from the scan list, save. The device locates itself by IP (or type any city) and starts tracking.

Later updates install over the air from the web panel. The cable is only ever needed once.

## What it does

**On the screen**

- Live flight list within a configurable radius (10 to 250 NM): airline logo, type, altitude with trend arrow, speed, distance; the 40 nearest of up to 80 tracked
- Flight details: airline, route with cities, country flags and local airport times, progress bar, ETA and local arrival time, squawk, ADS-B category, aircraft photo (planespotters.net)
- Spotter line: which way to look (compass + elevation) and a flyover prediction ("passes you in ~3 min at 1.2 km")
- Four views: list + details, ambient auto-cycling mode, radar on a real map of your area, session stats (hourly chart, top airlines, daily records, METAR)
- Full-screen route map with the great-circle track, swipe pan and zoom, flight trails
- Map screensaver after idle: your observation circle, every aircraft in range, clock and weather; tap a plane for its route
- Night mode, 7 color themes, English and Polish UI, all settings on the touchscreen

**Data**

- Free community ADS-B sources (airplanes.live, adsb.lol) with automatic failover, refresh every 8 s
- Or your own **dump1090/readsb receiver** on the LAN, with internet fallback
- Routes cross-checked against the aircraft's real position across three databases, so stale entries are rejected, not displayed
- Optional free FlightAware key adds ticket-style flight numbers (FR4238) and live routes

**Alerts and integrations**

- Push to your phone via [ntfy.sh](https://ntfy.sh): emergency squawks, watchlist aircraft, incoming flyovers
- Watchlist with gold highlighting; military and notable heavies (A380, AN-124, C-17...) always stand out
- MQTT with Home Assistant auto-discovery, generic JSON webhooks, alert history on flash

**Web panel** at `http://esp32flight.local`

- Tabs: Live (Leaflet map with trails and flags, flight table), History (spotting log with CSV export, alert history), Settings (full config with per-field help), API (built-in reference)
- OTA firmware updates from the browser, locked by default and armed from the device
- Prometheus `/metrics`, live `/screen.bmp` screenshots, optional password (HTTP Basic Auth) covering the panel and the whole API

## Setting up the integrations

All optional, configured in the web panel (Settings tab) or on the device (gear icon, Integrations tab). Empty field = feature off.

<details>
<summary><b>Push notifications to your phone (ntfy.sh)</b></summary>

Free, no account needed.

1. Install the [ntfy](https://ntfy.sh) app (Android/iOS).
2. In the app, subscribe to a topic with a unique name you invent, e.g. `jans-esp32flight-8341` (anyone who knows the name can read it, so make it non-obvious).
3. Enter the same topic name in **ntfy.sh topic** and save.

You will get a push for emergency squawks (7500/7600/7700), watchlist aircraft entering your radius and, with **Flyover alerts** enabled, a heads-up a few minutes before an interesting aircraft passes nearly overhead.
</details>

<details>
<summary><b>Home Assistant / MQTT</b></summary>

Enter your broker URI as **MQTT broker**, e.g. `mqtt://user:password@192.168.1.10:1883`. The device announces itself via MQTT discovery, so an "esp32flight" device appears in Home Assistant automatically with sensors: nearest aircraft (callsign, route, distance), aircraft count and session unique count. No YAML needed.
</details>

<details>
<summary><b>FlightAware flight numbers and routes</b></summary>

By default flights show radio callsigns (`RYR638T`). A free FlightAware AeroAPI key adds the commercial flight number (`FR4238`) next to it and uses the live origin/destination as an extra route source. Create a **Personal** key at [flightaware.com/aeroapi](https://www.flightaware.com/commercial/aeroapi/) and paste it into **FlightAware API key**. Results are cached, so the free monthly credit is more than enough.
</details>

<details>
<summary><b>Webhook</b></summary>

On every alert (emergency, watchlist, flyover) the device POSTs `{"source": "esp32flight", "title": "...", "message": "..."}` to the URL in **Webhook URL**. Point it at Node-RED, n8n, a Discord/Slack bridge or your own endpoint.
</details>

<details>
<summary><b>Watchlist</b></summary>

Comma-separated registration or callsign prefixes in **Watchlist**, e.g. `SP-LR,RCH,A388`. Matching aircraft are highlighted in gold and push-notified. Military aircraft and notable heavies are always highlighted, no entry needed.
</details>

<details>
<summary><b>Local ADS-B receiver (dump1090 / readsb)</b></summary>

If you run your own receiver (RTL-SDR dongle on a Raspberry Pi with dump1090, readsb, or a feeder image), point **Local receiver URL** at its JSON output, e.g. `http://192.168.1.50:8080/data/aircraft.json`. The device then reads aircraft straight from your antenna instead of internet APIs: faster updates, no rate limits, works even without internet. Falls back to the internet automatically when the receiver is unreachable.
</details>

## HTTP API

Everything the panel shows is plain HTTP on port 80. The full reference with examples lives in the panel itself, under the **API** tab. Summary:

| Endpoint | What it returns |
|---|---|
| `GET /api/state` | live JSON: flights with routes and trails, weather, network, stats |
| `GET /api/config` | current settings (passwords never included) |
| `POST /api/config` | update any subset of settings, saves and restarts |
| `GET /api/log` | spotting history TSV (epoch, hex, callsign, type, airline) |
| `GET /api/alerts` | alert history TSV (epoch, title, message) |
| `GET /screen.bmp` | live 800x480 screenshot of the display |
| `GET /metrics` | Prometheus metrics (aircraft, session records, heap) |
| `POST /ota` | firmware update (403 unless unlocked on the device) |

With a panel password set, every endpoint requires Basic Auth: `curl -u admin:PASSWORD ...`

## Data sources (all free, no API keys)

| What | Source |
|---|---|
| Aircraft positions (ADS-B) | [airplanes.live](https://airplanes.live), fallback [adsb.lol](https://adsb.lol) |
| Routes + airlines | [adsbdb.com](https://www.adsbdb.com), [adsb.lol routeset](https://api.adsb.lol/docs), [hexdb.io](https://hexdb.io) |
| Map tiles | [CARTO basemaps](https://carto.com/basemaps) with data (c) [OpenStreetMap](https://www.openstreetmap.org/copyright) contributors |
| Geocoding, weather, timezones | [Open-Meteo](https://open-meteo.com) |
| METAR | [aviationweather.gov](https://aviationweather.gov) (NOAA) |
| IP geolocation | [ip-api.com](https://ip-api.com) |
| Aircraft photos | [planespotters.net](https://www.planespotters.net) via adsbdb |
| Airline logos | [sexym0nk3y/airline-logos](https://github.com/sexym0nk3y/airline-logos), [Jxck-S/airline-logos](https://github.com/Jxck-S/airline-logos) |
| Country flags | [flagcdn.com](https://flagpedia.net) (bundled) |
| Airport database | [OurAirports](https://ourairports.com) (bundled, public domain) |
| Offline world map | NASA Blue Marble |

Optional, with a user-provided free key: [FlightAware AeroAPI](https://www.flightaware.com/commercial/aeroapi/) for commercial flight numbers and live routes.

## Hardware

Waveshare ESP32-S3-Touch-LCD-7: ESP32-S3 (16 MB flash, 8 MB PSRAM), 800x480 RGB LCD (ST7262), GT911 capacitive touch, CH343 USB-UART. Available from the usual electronics shops; cases are printable or purchasable.

## Building from source

<details>
<summary>ESP-IDF 5.5+, ImageMagick and Node required</summary>

```sh
# one-time: fetch airline logos, airports, flags + generate fonts
./tools/fetch_logos.sh
./tools/fetch_airports.sh
./tools/fetch_flags.sh
./tools/gen_fonts.sh

idf.py set-target esp32s3
idf.py -p /dev/cu.usbmodemXXXX -b 230400 flash
```

Subsequent updates can go over the air from the web panel (`build/esp32flight.bin`).
</details>

## Support the project

If this thing earned a spot on your shelf, you can buy me a coffee:

<a href="https://ko-fi.com/theqkash"><img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="Support on Ko-fi" height="36"></a>

Bug reports and feature ideas are just as welcome: [open an issue](https://github.com/theqkash/esp32flight/issues).

## License

MIT (c) [Łukasz Nowak (@theqkash)](https://github.com/theqkash)

Display bring-up adapted from Waveshare's demo code (CC0). Built with [LVGL](https://lvgl.io) 8 and ESP-IDF.
