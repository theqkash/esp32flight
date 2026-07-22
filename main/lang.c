#include "lang.h"
#include "lvgl.h"
#include "settings.h"

static const lang_t k_en = {
    .connecting = "connecting to Wi-Fi...",
    .setup_wifi = "tap %s to set up Wi-Fi",
    .locating = "locating...",
    .geo_retry = "geolocation failed, retrying...",
    .no_data = "no data - check connection",
    .status_fmt = "%.12s | %d ac | %d km",
    .waiting_aircraft = "Waiting for aircraft...",
    .route_unknown = "Route unknown",
    .km_to_go_fmt = "%d%%  -  %.0f km to go%s%s",
    .ground = "ground",
    .map_btn = "Map",
    .ring_fmt = "N up  -  ring %d km",
    .st_alt = "ALTITUDE", .st_speed = "SPEED", .st_vrate = "V/RATE",
    .st_dist = "DISTANCE", .st_track = "TRACK", .st_reg = "REG",
    .settings_title = "Settings",
    .wifi_ssid = "Wi-Fi SSID", .password = "Password",
    .auto_location = "Auto location (IP)", .hide_ground = "Hide ground",
    .airline_only = "Airline only",
    .city_search = "City search", .latitude = "Latitude", .longitude = "Longitude",
    .search_radius = "Search radius", .theme_lbl = "Theme",
    .language_lbl = "Language", .save = "Save",
    .ota_unlock = "Allow OTA updates (until restart)",
    .ota_hint = "Applies immediately, the web panel unlocks within seconds.\n"
                "Do not tap Save: a restart locks OTA again. Just close this screen.",
    .dd_networks = "networks", .dd_results = "results",
    .scanning = "(scanning...)", .no_networks = "(no networks)",
    .not_found = "(not found)", .searching = "(searching...)",
    .saved_restarting = "Saved - restarting...",
    .loading_photo = "Loading photo...", .no_photo = "No photo available",
    .look_fmt = LV_SYMBOL_EYE_OPEN " Look %s, %d\xC2\xB0 above horizon",
    .look_short_fmt = LV_SYMBOL_EYE_OPEN " %s %d\xC2\xB0",
    .cpa_fmt = "flyover in ~%d min (%.1f km)",
    .tab_net = "Network", .tab_place = "Location", .tab_filters = "Filters",
    .tab_integr = "Integrations", .tab_system = "System",
    .cpa_lbl = "Flyover alerts (CPA)",
    .amb_idle_lbl = "Map screensaver after (min, 0=off)",
    .night_lbl = "Night mode", .night_from = "From (HH:MM)", .night_to = "To (HH:MM)",
    .lbl_ntfy = "ntfy.sh topic", .lbl_mqtt = "MQTT broker (Home Assistant)",
    .lbl_fa = "FlightAware API key", .lbl_watch = "Watchlist (e.g. SP-LR,RCH,A388)",
    .lbl_webhook = "Webhook URL", .lbl_ladsb = "Local receiver aircraft.json URL",
    .hint_ntfy = "Push to your phone: install the ntfy app and subscribe to this topic",
    .hint_mqtt = "mqtt://user:pass@host:1883, sensors auto-appear in Home Assistant",
    .hint_fa = "Adds flight numbers (FR4238); free key: flightaware.com/aeroapi",
    .hint_webhook = "POSTs alert JSON to this URL (Node-RED, n8n, Discord bridge)",
    .hint_ladsb = "Own dump1090/readsb receiver, e.g. http://ip:8080/data/aircraft.json; replaces internet APIs when reachable",
    .avg_word = "avg", .best_word = "best",
    .stats_title = "Session statistics",
    .st_hourly = "NEW AIRCRAFT PER HOUR",
    .st_top_airlines = "TOP AIRLINES",
    .st_unique = "UNIQUE AIRCRAFT",
    .st_fastest = "FASTEST", .st_farthest = "FARTHEST", .st_highest = "HIGHEST",
    .wx_clear = "Clear", .wx_partly = "Partly cloudy", .wx_overcast = "Overcast",
    .wx_fog = "Fog", .wx_drizzle = "Drizzle", .wx_rain = "Rain",
    .wx_snow = "Snow", .wx_showers = "Showers", .wx_thunder = "Thunderstorm",
};

static const lang_t k_pl = {
    .connecting = "łączenie z Wi-Fi...",
    .setup_wifi = "dotknij %s aby ustawić Wi-Fi",
    .locating = "ustalanie pozycji...",
    .geo_retry = "geolokalizacja nieudana, ponawiam...",
    .no_data = "brak danych - sprawdź połączenie",
    .status_fmt = "%.12s | %d szt. | %d km",
    .waiting_aircraft = "Czekam na samoloty...",
    .route_unknown = "Trasa nieznana",
    .km_to_go_fmt = "%d%%  -  %.0f km do celu%s%s",
    .ground = "na ziemi",
    .map_btn = "Mapa",
    .ring_fmt = "N u góry  -  pierścień %d km",
    .st_alt = "WYSOKOŚĆ", .st_speed = "PRĘDKOŚĆ", .st_vrate = "WZNOSZENIE",
    .st_dist = "DYSTANS", .st_track = "KURS", .st_reg = "REJESTRACJA",
    .settings_title = "Ustawienia",
    .wifi_ssid = "Sieć Wi-Fi", .password = "Hasło",
    .auto_location = "Lokalizacja auto (IP)", .hide_ground = "Ukryj naziemne",
    .airline_only = "Tylko rejsowe",
    .city_search = "Szukaj miasta", .latitude = "Szerokość", .longitude = "Długość",
    .search_radius = "Promień", .theme_lbl = "Motyw",
    .language_lbl = "Język", .save = "Zapisz",
    .ota_unlock = "Zezwól na aktualizacje OTA (do restartu)",
    .ota_hint = "Działa od razu, panel web odblokuje się w kilka sekund.\n"
                "Nie klikaj Zapisz: restart ponownie zablokuje OTA. Zamknij ten ekran.",
    .dd_networks = "sieci", .dd_results = "wyniki",
    .scanning = "(skanowanie...)", .no_networks = "(brak sieci)",
    .not_found = "(nie znaleziono)", .searching = "(szukam...)",
    .saved_restarting = "Zapisano - restart...",
    .loading_photo = "Wczytywanie zdjęcia...", .no_photo = "Brak zdjęcia",
    .look_fmt = LV_SYMBOL_EYE_OPEN " Patrz %s, %d\xC2\xB0 nad horyzontem",
    .look_short_fmt = LV_SYMBOL_EYE_OPEN " %s %d\xC2\xB0",
    .cpa_fmt = "przelot za ~%d min (%.1f km)",
    .tab_net = "Sieć", .tab_place = "Miejsce", .tab_filters = "Filtry",
    .tab_integr = "Integracje", .tab_system = "System",
    .cpa_lbl = "Alerty przelotu (CPA)",
    .amb_idle_lbl = "Wygaszacz z mapą po (min, 0=wył.)",
    .night_lbl = "Tryb nocny", .night_from = "Od (HH:MM)", .night_to = "Do (HH:MM)",
    .lbl_ntfy = "Temat ntfy.sh", .lbl_mqtt = "Broker MQTT (Home Assistant)",
    .lbl_fa = "Klucz FlightAware API", .lbl_watch = "Watchlista (np. SP-LR,RCH,A388)",
    .lbl_webhook = "Adres webhooka", .lbl_ladsb = "URL aircraft.json lokalnego odbiornika",
    .hint_ntfy = "Push na telefon: zainstaluj apkę ntfy i zasubskrybuj ten temat",
    .hint_mqtt = "mqtt://user:hasło@host:1883, encje same pojawią się w Home Assistant",
    .hint_fa = "Dodaje numery rejsów (FR4238); darmowy klucz: flightaware.com/aeroapi",
    .hint_webhook = "Wysyła POST z JSON-em przy każdym alercie (Node-RED, n8n)",
    .hint_ladsb = "Własny odbiornik dump1090/readsb, np. http://ip:8080/data/aircraft.json; zastępuje API z internetu gdy dostępny",
    .avg_word = "śr.", .best_word = "rekord",
    .stats_title = "Statystyki sesji",
    .st_hourly = "NOWE MASZYNY WG GODZIN",
    .st_top_airlines = "NAJCZĘSTSZE LINIE",
    .st_unique = "UNIKALNE MASZYNY",
    .st_fastest = "NAJSZYBSZY", .st_farthest = "NAJDALSZY", .st_highest = "NAJWYŻEJ",
    .wx_clear = "Bezchmurnie", .wx_partly = "Częściowe zachm.", .wx_overcast = "Pochmurno",
    .wx_fog = "Mgła", .wx_drizzle = "Mżawka", .wx_rain = "Deszcz",
    .wx_snow = "Śnieg", .wx_showers = "Przelotne opady", .wx_thunder = "Burza",
};

const lang_t *L(void)
{
    return settings_get()->lang == 1 ? &k_pl : &k_en;
}

const char *lang_weather_desc(int code)
{
    const lang_t *l = L();
    if (code == 0) return l->wx_clear;
    if (code <= 2) return l->wx_partly;
    if (code == 3) return l->wx_overcast;
    if (code == 45 || code == 48) return l->wx_fog;
    if (code >= 51 && code <= 57) return l->wx_drizzle;
    if (code >= 61 && code <= 67) return l->wx_rain;
    if (code >= 71 && code <= 77) return l->wx_snow;
    if (code >= 80 && code <= 82) return l->wx_showers;
    if (code == 85 || code == 86) return l->wx_snow;
    if (code >= 95) return l->wx_thunder;
    return l->wx_overcast;
}

const char *lang_compass(int deg)
{
    static const char *dirs[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return dirs[((deg % 360) + 382) / 45 % 8];
}
