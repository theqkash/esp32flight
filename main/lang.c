#include "lang.h"
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
