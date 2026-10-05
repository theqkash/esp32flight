<h1 align="center">esp32flight</h1>

<p align="center">
  A standalone desk flight radar on a single ESP32 board with a touchscreen,<br>
  also available as an Android app built from the same code.<br>
  Live aircraft around you: airline logos, routes, maps, alerts and a built-in web panel.<br>
  No Raspberry Pi, no server, no subscriptions, no API keys.
</p>

<p align="center">
  <a href="https://discord.gg/CwH8XKRnpz"><img src="https://img.shields.io/badge/Discord-join%20the%20esp32flight%20community-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Join the esp32flight Discord"></a>
  <a href="https://ko-fi.com/theqkash"><img src="https://img.shields.io/badge/Ko--fi-support%20the%20project-ff5e5b?style=for-the-badge&logo=kofi&logoColor=white" alt="Support esp32flight on Ko-fi"></a>
</p>

<p align="center">
  <a href="https://theqkash.github.io/esp32flight/"><img src="https://img.shields.io/badge/flash-from%20your%20browser-4da3ff?style=flat-square" alt="browser flasher"></a>
  <a href="https://github.com/theqkash/esp32flight/releases"><img src="https://badgen.net/github/release/theqkash/esp32flight?color=2ea44f" alt="latest release"></a>
  <a href="https://github.com/theqkash/esp32flight/releases/latest"><img src="https://img.shields.io/badge/android-apk%20download-3ddc84?style=flat-square&logo=android&logoColor=white" alt="Android APK"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue?style=flat-square" alt="MIT"></a>
  <a href="https://ko-fi.com/theqkash"><img src="https://img.shields.io/badge/ko--fi-support%20the%20project-ff5e5b?style=flat-square&logo=kofi&logoColor=white" alt="Ko-fi"></a>
</p>

---

> [!WARNING]
> **FlightAware integration: known issue.** A bug in the current firmware makes the optional FlightAware AeroAPI integration burn through API credits far faster than intended. **Leave the FlightAware key empty for now** - the fix ships with the next esp32flight update in the second half of October 2026. Everything else works without it; the integration is optional.


| | |
|---|---|
| ![flight details](docs/detail.png) | ![forest theme](docs/detail-forest.png) |
| ![radar view](docs/radar.png) | ![retro radar with rain](docs/retro.png) |
| ![ambient mode with rain radar](docs/ambient-mode.png) | ![route map](docs/route-map.png) |
| ![aircraft photo](docs/photo.png) | ![web panel](docs/web-panel.png) |

## Quick start

1. Get one of the [ten supported boards](#hardware) - from the $15 Guition JC8048W550 to the original **Waveshare ESP32-S3-Touch-LCD-7** (about $35) - and a USB cable.
2. Open **[the browser installer](https://theqkash.github.io/esp32flight/)** in Chrome or Edge, click the button for your board, pick the serial port. The Waveshare 800x480 family and the Guition even share one auto-detecting binary.
3. Tap the gear icon, pick your 2.4 GHz Wi-Fi from the scan list, save. The device locates itself by IP (or type any city) and starts tracking.

Later updates install over the air from the web panel. The cable is only ever needed once.

No board at hand? The same radar also ships as an [Android app](#android-app).

## What it does

**On the screen**

- Live flight list within a configurable radius (1 to 250 NM): airline logo, type, altitude with trend arrow, speed, distance; the 40 nearest of up to 80 tracked
- Flight details: airline, route with cities, country flags and local airport times, progress bar, ETA and local arrival time, squawk, ADS-B category, aircraft photo (planespotters.net)
- Spotter line: which way to look (compass + elevation) and a flyover prediction ("passes you in ~3 min at 1.2 km")
- Five views: list + details, ambient auto-cycling mode, radar on a real map of your area with runway strips of nearby airports, session stats (hourly chart, top airlines, daily records, METAR raw or decoded), retro CRT radar
- Optional extra objects on the radar: the ISS when it crosses your area, weather balloons from the SondeHub network, AIS ships (own free key) with a full detail card (type, destination, speed), and airspace outlines (CTR/TMA/danger, own free openAIP key)
- List content toggle when ships are on: planes only, ships only or everything; tap any plane or ship on the map for details
- Aviation or metric units (ft/kt or m and km/h), one switch
- Full-screen route map with the great-circle track, swipe pan and zoom, flight trails
- Map screensaver after idle: your observation circle, every aircraft in range, clock and weather; tap a plane for its route
- Aircraft class filter: pick any mix of airliners, light aircraft, helicopters, military and others; the class colors the type code in the list
- Night mode, 7 color themes, English and Polish UI, 24 h or 12 h AM/PM clock, all settings on the touchscreen; auto-cycling can be turned off to follow one flight
- Optional route on the list and radar: origin/destination codes on list rows, cities in the tapped-aircraft bubble
- Optional backlight brightness control on boards that can dim (slider, off by default), and a choice of map tile providers: OpenStreetMap with a built-in dark style, CARTO with your own free key, or any custom {z}/{x}/{y} tile server

**Data**

- Free community ADS-B sources (airplanes.live, adsb.lol, adsb.fi) with automatic failover, refresh every 8 s
- Or your own **dump1090/readsb receiver** on the LAN, with internet fallback
- Routes cross-checked against the aircraft's real position across three databases, so stale entries are rejected, not displayed
- Optional free FlightAware key adds ticket-style flight numbers (FR4238) and live routes - **currently not recommended, see the warning at the top** (credit-burn bug, fix due second half of October 2026)

**Alerts and integrations**

- Push to your phone via [ntfy.sh](https://ntfy.sh): emergency squawks, watchlist aircraft, incoming flyovers
- Watchlist with gold highlighting; military and notable heavies (A380, AN-124, C-17...) always stand out
- MQTT with Home Assistant auto-discovery (aircraft sensors, plus the screen backlight as a dimmable HA light on boards with brightness control enabled), generic JSON webhooks, alert history on flash
- **Physical controls**: buttons and rotary encoders on a cheap PCF8574/MCP23017 I2C expander (or free GPIOs where a board has them) mapped to actions like view switching, radar zoom, altitude limits or brightness - configured in the web panel with a learn mode; custom decks can drive the same actions over HTTP (`/api/input`)

**Web panel** at `http://esp32flight.local`

- Tabs: Live (Leaflet map with trails and flags, flight table), History (spotting log with CSV export, alert history), Settings (full config with per-field help), API (built-in reference)
- OTA firmware updates from the browser, locked by default and armed from the device
- Prometheus `/metrics`, live `/screen.bmp` screenshots, optional password (HTTP Basic Auth) covering the panel and the whole API

## Android app

The same radar, compiled for Android from the same core sources, so every feature above ships on both at the same version. Download `esp32flight-vX.Y.Z.apk` from the [latest release](https://github.com/theqkash/esp32flight/releases/latest) and install it (sideload; Android 5.0 or newer, phones and tablets, arm64 + arm32).

- Always landscape, always fullscreen, notch and cutout areas included
- The canvas adapts to the screen: tablets render more list rows, a bigger radar and larger type instead of a stretched frame
- The web panel runs inside the app at `http://<phone-ip>:8080`
- Wi-Fi setup is not needed (the system connection is used) and settings apply live, no restart
- Source lives in [`apkflight/`](apkflight/), a thin SDL2 + shim layer over the shared code

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

> [!WARNING]
> **Hold off on this one until the second half of October 2026.** The current firmware has a bug that makes the FlightAware integration consume API credits very quickly - a free Personal key can be exhausted in a short time. The fix is coming in the next update; until then keep the key field empty.

By default flights show radio callsigns (`RYR638T`). A free FlightAware AeroAPI key adds the commercial flight number (`FR4238`) next to it and uses the live origin/destination as an extra route source. FlightAware serves actual flight plans, so with a key set the routes become authoritative - the stale shuttle-route entries that community databases sometimes serve stop mattering. Create a **Personal** key at [flightaware.com/aeroapi](https://www.flightaware.com/commercial/aeroapi/) and paste it into **FlightAware API key**. Results are cached, so the free monthly credit is more than enough.
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
| `GET/POST /api/input` | run a named action (`?action=zoom_in`, `next_view`, `wake`...) - for buttons, decks, automations |
| `GET /api/input/last` | last physical input event, used by the panel's learn mode |
| `POST /ota` | firmware update (403 unless unlocked on the device) |

With a panel password set, every endpoint requires Basic Auth: `curl -u admin:PASSWORD ...`

## Data sources (free; the two marked layers take your own free key)

| What | Source |
|---|---|
| Aircraft positions (ADS-B) | [airplanes.live](https://airplanes.live), fallbacks [adsb.lol](https://adsb.lol) and [adsb.fi](https://adsb.fi) |
| Routes + airlines | [adsbdb.com](https://www.adsbdb.com), [adsb.lol routeset](https://api.adsb.lol/docs), [hexdb.io](https://hexdb.io) |
| Map tiles | [OpenStreetMap](https://www.openstreetmap.org/copyright) standard tiles ((c) OpenStreetMap contributors); the dark style is applied on-device |
| Precipitation radar (optional overlay) | [RainViewer](https://www.rainviewer.com/api.html) |
| Geocoding, weather, timezones | [Open-Meteo](https://open-meteo.com) |
| METAR + TAF | [aviationweather.gov](https://aviationweather.gov) (NOAA) |
| ISS position | [wheretheiss.at](https://wheretheiss.at) |
| Radiosondes | [SondeHub](https://sondehub.org) community network |
| Ships (AIS, optional, own free key) | [aisstream.io](https://aisstream.io) |
| Airspace outlines (optional, own free key) | [openAIP](https://www.openaip.net) |
| IP geolocation | [ip-api.com](https://ip-api.com) |
| Aircraft photos | [planespotters.net](https://www.planespotters.net) via adsbdb |
| Airline logos | [sexym0nk3y/airline-logos](https://github.com/sexym0nk3y/airline-logos), [Jxck-S/airline-logos](https://github.com/Jxck-S/airline-logos), served on demand from [esp32flight-logos](https://github.com/theqkash/esp32flight-logos) |
| Country flags | [flagcdn.com](https://flagpedia.net) (bundled) |
| Airports + runways | [OurAirports](https://ourairports.com) (bundled, public domain) |
| Offline world map | NASA Blue Marble |

Optional, with a user-provided free key: [FlightAware AeroAPI](https://www.flightaware.com/commercial/aeroapi/) for commercial flight numbers and live routes (currently not recommended - credit-burn bug, fix due second half of October 2026).

## Hardware

Thirteen supported configurations, all running the same code. The Waveshare 800x480 family and the Guition share one auto-detecting binary; the rest are dedicated builds, every one of them a button on [the flasher](https://theqkash.github.io/esp32flight/) (the newest ports start on [the test-builds page](https://theqkash.github.io/esp32flight/beta.html) until the next release):

- **Waveshare ESP32-S3-Touch-LCD-7** (7", the original target, ~$35): ESP32-S3, 16 MB flash, 8 MB PSRAM, 800x480 RGB (ST7262), GT911 touch. Printable case: [on Printables](https://www.printables.com/model/1425850-waveshare-esp32-s3-7inch-capacitive-touch-display).
- **Waveshare ESP32-S3-Touch-LCD-4.3 / 4.3B / 5**: same pinout as the 7. Note: 4.3 units ship an 8 MB module (N8R8) - use the `-8mb` build (curated logo set, the rest fetched on demand from [esp32flight-logos](https://github.com/theqkash/esp32flight-logos)).
- **Waveshare ESP32-S3-Touch-LCD-7B** (7", 1024x600, ~$40): the sharpest panel, real brightness PWM via its helper MCU. Runs the memory buses at 120 MHz - the panel demands it.
- **Waveshare ESP32-S3-Touch-LCD-7C / 7C BOX** (7", 800x480, ~$70): the boxed one - enclosure, battery, speakers, isolated IO. Audio hardware not used by the radar yet.
- **Guition JC8048W550** (5", ~$15): the budget pick, hardware-proven since 0.3.x.
- **Sunton ESP32-4827S043** (4.3", 480x272, ~$15): capacitive "C" and resistive "R" variants, each with its own build; the UI renders proportionally downscaled. Community-contributed.
- **Sunton ESP32-8048S070** (7", 800x480, ~$22) and its unbranded clones: colors and display window bench-tuned by the community.
- **Elecrow CrowPanel 5.0 / 7.0** (800x480, 4 MB flash class): single app slot, updates via the flasher (no OTA), logos on demand. Most unbranded 7" Amazon HMI boards are CrowPanel 7.0 rebrands - check the module marking (N4R8) if unsure. Hardware-confirmed by testers.
- **Elecrow CrowPanel Advance 7.0** (DIS02170A V1.2+, 7", 800x480, 16 MB): the newer Advance series with real backlight dimming via its helper MCU. V1.0/V1.1 hardware revisions are not covered.
- **BigTreeTech Panda Touch** (5", 800x480, ~$75): BTT's Bambu pendant is a regular ESP32-S3 display underneath. Flashing replaces the stock Bambu-remote firmware (restorable from a backup), and the battery makes it a portable radar.
- **M5Stack Tab5** (5", 1280x720, ~$60): ESP32-P4 with Wi-Fi over its ESP32-C6, battery, the crispest screen of the fleet. All three display generations (ILI9881C, ST7123, ST7121) are auto-detected.
- **Guition JC8012P4A1** (10.1", 800x1280, ESP32-P4, ~$40): the biggest screen of the fleet, Wi-Fi over its ESP32-C6, clean-room GSL3680 touch driver. Panel revision V1 hardware-confirmed; V2 (rear-case batch 2628+) has its own build awaiting a tester.

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

The Android app builds from [`apkflight/`](apkflight/): `make` gives a desktop test build (SDL2 via Homebrew), `gradle assembleRelease` in `apkflight/android/` produces the APK. Native curl and mbedTLS are cross-compiled once with `apkflight/scripts/build_android_deps.sh`.
</details>

## Support the project

esp32flight is free, open source and has no subscriptions - but every supported board
in the table above had to land on my desk before its port could ship, and the next
ones are waiting. If this thing earned a spot on your shelf, a coffee genuinely
keeps new hardware coming:

<a href="https://ko-fi.com/theqkash"><img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="Support on Ko-fi" height="36"></a>

Bug reports and feature ideas are just as welcome: [open an issue](https://github.com/theqkash/esp32flight/issues)
or drop by the [Discord server](https://discord.gg/CwH8XKRnpz) to chat, share spots and get help.

## License

MIT (c) [Łukasz Nowak (@theqkash)](https://github.com/theqkash)

Display bring-up adapted from Waveshare's demo code (CC0). Built with [LVGL](https://lvgl.io) 8 and ESP-IDF.
