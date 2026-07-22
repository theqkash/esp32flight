# esp32flight

A standalone desktop flight radar for the **Waveshare ESP32-S3-Touch-LCD-7**
(800×480). It shows live aircraft around your location with airline logos,
routes, flight progress, a route map, radar view, weather, and a built-in web
panel with OTA updates. No Raspberry Pi, no server - one ESP32 board.

**[Flash it from your browser](https://theqkash.github.io/esp32flight/)** (Chrome/Edge,
no tools needed) or grab binaries from
[Releases](https://github.com/theqkash/esp32flight/releases).

## Screenshots

| | |
|---|---|
| ![flight details](docs/detail-light.png) | ![forest theme](docs/detail-forest.png) |
| ![ambient mode with map](docs/ambient-mode.png) | ![radar view](docs/radar.png) |
| ![route map](docs/route-map.png) | ![aircraft photo](docs/photo.png) |

Web panel with live map, stats and OTA:

![web panel](docs/web-panel.png)

## Features

- **Live flight list** within a configurable radius (10-250 nm) - callsign,
  airline logo, aircraft type, altitude, speed, distance
- **Flight details**: airline name, route with city + country, progress bar,
  ETA with arrival time, aircraft photo (planespotters.net)
- **Route map**: full-screen world map with the great-circle track and live
  aircraft position
- **Auto (ambient) mode**: aircraft cycle automatically with a persistent
  map + info bubble - flight-tracker-on-the-shelf style
- **Radar view**: home-centered rings with aircraft plotted by bearing/distance
- **Weather** in the header (Open-Meteo): icon, temperature, wind
- **Emergency alert** for squawk 7700/7500/7600
- **On-device settings** (touch): Wi-Fi scan + join, city search geocoding,
  fixed/auto (IP) location, radius, filters (ground traffic, airline-only),
  7 color themes, English/Polish UI
- **Spotter tools**: where to look (compass + elevation angle), flyover
  prediction (CPA) with push alerts, METAR from the nearest station
- **Local ADS-B receiver support**: point it at a dump1090/readsb
  `aircraft.json` on your LAN and go fully independent of internet APIs
- **Full-screen map screensaver** after idle, night mode with backlight-off
  quiet hours, first-boot setup screen
- **Integrations**: ntfy.sh push, MQTT with Home Assistant discovery,
  generic webhooks, optional FlightAware AeroAPI for IATA flight numbers
- **Web panel** at `http://esp32flight.local` - live flight table with trails
  and country flags, Leaflet map, daily/session stats, spotting history with
  CSV export, full device settings, Prometheus `/metrics`, `/screen.bmp`
  screenshots and **OTA firmware updates** from the browser

## Setting up the integrations

All of these are optional and configured in the web panel
(`http://esp32flight.local`, Device settings) or on the device
(gear icon, Integrations tab). Empty field = feature off.

### Push notifications to your phone (ntfy.sh)

Free, no account needed.

1. Install the [ntfy](https://ntfy.sh) app (Android/iOS).
2. In the app, subscribe to a topic with a unique name you invent,
   e.g. `jans-esp32flight-8341` (anyone who knows the name can read it,
   so make it non-obvious).
3. Enter the same topic name in **ntfy.sh topic** and save.

You will get a push for emergency squawks (7500/7600/7700), watchlist
aircraft entering your radius and, with **Flyover alerts** enabled,
a heads-up a few minutes before an interesting aircraft passes nearly
overhead - enough time to step outside.

### Home Assistant / MQTT

Enter your broker URI as **MQTT broker**, e.g.
`mqtt://user:password@192.168.1.10:1883`. The device announces itself
via MQTT discovery, so an "esp32flight" device appears in Home Assistant
automatically with sensors: nearest aircraft (callsign, route, distance),
aircraft count and session unique count. No YAML needed.

### FlightAware flight numbers

By default flights show radio callsigns (`RYR638T`). A free FlightAware
AeroAPI key adds the commercial flight number (`FR4238`) next to it.
Create a **Personal** key at
[flightaware.com/aeroapi](https://www.flightaware.com/commercial/aeroapi/)
and paste it into **FlightAware API key**. Results are cached, so the free
monthly credit is more than enough.

### Webhook

On every alert (emergency, watchlist, flyover) the device POSTs
`{"source": "esp32flight", "title": "...", "message": "..."}` to the URL
in **Webhook URL**. Point it at Node-RED, n8n, a Discord/Slack bridge or
your own endpoint.

### Watchlist

Comma-separated registration or callsign prefixes in **Watchlist**,
e.g. `SP-LR,RCH,A388`. Matching aircraft are highlighted in gold and
push-notified. Military aircraft and notable heavies (A380, AN-124,
C-17, B-52...) are always highlighted, no entry needed.

### Local ADS-B receiver (dump1090 / readsb)

If you run your own receiver (RTL-SDR dongle on a Raspberry Pi with
dump1090, readsb, or a full FlightRadar24/ADSBx feeder image), point
**Local receiver URL** at its JSON output, e.g.
`http://192.168.1.50:8080/data/aircraft.json`. The device then reads
aircraft straight from your antenna instead of internet APIs: faster
updates, no rate limits, works even without internet. When the receiver
is unreachable it falls back to the internet sources automatically.

## HTTP API

Everything the web panel shows is available as plain HTTP on port 80
(`http://esp32flight.local` or the device IP). The full reference with
examples lives in the panel itself, under the **API** tab. Summary:

| Endpoint | What it returns |
|---|---|
| `GET /api/state` | live JSON: flights with routes and trails, weather, network, stats |
| `GET /api/config` | current settings (passwords never included) |
| `POST /api/config` | update any subset of settings, saves and restarts |
| `GET /api/log` | spotting history TSV (epoch, hex, callsign, type, airline) |
| `GET /screen.bmp` | live 800x480 screenshot of the display |
| `GET /metrics` | Prometheus metrics |
| `POST /ota` | firmware update (403 unless unlocked on the device) |

The panel and the whole API can be protected with a password (HTTP Basic
Auth, user `admin`), set in the web settings or on the device (System tab).
An empty password, the default, leaves the panel open - fine for a home
network. `curl -u admin:PASSWORD ...` once it is set.

## Data sources (all free, no API keys)

| What | Source |
|---|---|
| Aircraft positions (ADS-B) | [airplanes.live](https://airplanes.live), fallback [adsb.lol](https://adsb.lol) |
| Routes + airlines | [adsbdb.com](https://www.adsbdb.com), fallback [hexdb.io](https://hexdb.io) |
| Geocoding + weather | [Open-Meteo](https://open-meteo.com) |
| IP geolocation | [ip-api.com](https://ip-api.com) |
| Aircraft photos | [planespotters.net](https://www.planespotters.net) via adsbdb |
| Airline logos | [sexym0nk3y/airline-logos](https://github.com/sexym0nk3y/airline-logos), [Jxck-S/airline-logos](https://github.com/Jxck-S/airline-logos) |
| Airport database | [OurAirports](https://ourairports.com) (bundled, public domain) |
| World map | NASA Blue Marble |

Routes resolved from callsigns are **position-validated** (great-circle
plausibility check) and cross-checked against the second source, because
callsign→route databases are sometimes stale.

## Hardware

Waveshare ESP32-S3-Touch-LCD-7: ESP32-S3 (16 MB flash, 8 MB PSRAM), 800×480
RGB LCD (ST7262), GT911 capacitive touch, CH343 USB-UART.

## Building

Requires ESP-IDF ≥ 5.5, ImageMagick and Node (for asset generation).

```sh
# one-time: fetch airline logos + generate fonts (Latin Ext A/B + icons)
./tools/fetch_logos.sh
./tools/fetch_airports.sh
./tools/gen_fonts.sh

idf.py set-target esp32s3
idf.py -p /dev/cu.usbmodemXXXX -b 230400 flash
```

First boot: tap the gear icon, pick your Wi-Fi from the scan list, type the
password on the on-screen keyboard, save. Subsequent updates can be done
over the air from the web panel (`build/esp32flight.bin`).

## License

MIT © [Łukasz Nowak (@theqkash)](https://github.com/theqkash)

Display bring-up adapted from Waveshare's demo code (CC0). Built with
[LVGL](https://lvgl.io) 8 and ESP-IDF.
