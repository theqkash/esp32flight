idf_component_register(
    SRCS
        "main.c"
        "waveshare_rgb_lcd_port.c"
        "lvgl_port.c"
        "geo_math.c"
        "http_util.c"
        "wifi_mgr.c"
        "geolocate.c"
        "flight_data.c"
        "routes.c"
        "logos.c"
        "airlines.c"
        "airports.c"
        "faflight.c"
        "flight_task.c"
        "geocode.c"
        "jpeg_dec.c"
        "lang.c"
        "mqtt_pub.c"
        "notify.c"
        "obslog.c"
        "settings.c"
        "theme.c"
        "tilemap.c"
        "trails.c"
        "tz.c"
        "weather.c"
        "ui.c"
        "ui_settings.c"
        "ui_map.c"
        "ui_photo.c"
        "web_server.c"
        "font_pl_14.c"
        "font_pl_16.c"
        "font_pl_20.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_http_client esp_http_server esp-tls nvs_flash json spiffs esp_netif app_update mqtt
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1)

idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -Wno-format)

spiffs_create_partition_image(assets ../assets FLASH_IN_PROJECT)
