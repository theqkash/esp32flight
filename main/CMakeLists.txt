# Board bring-up is per chip: the S3 boards (Waveshare/Guition) share the RGB
# port with runtime autodetect, the ESP32-P4 (M5Stack Tab5) has a MIPI-DSI port.
if(CONFIG_CANFLIGHT_BOARD_JC8012P4A1)
    set(BOARD_PORT_SRC "jc10_lcd_port.c" "jc10/esp_lcd_touch_gsl3680.c")
elseif(CONFIG_CANFLIGHT_BOARD_ELECROW_P4_10)
    set(BOARD_PORT_SRC "elecrow10_lcd_port.c")
elseif(IDF_TARGET STREQUAL "esp32p4")
    set(BOARD_PORT_SRC "tab5_lcd_port.c" "tab5/esp_lcd_st7123.c" "tab5/esp_lcd_st7121.c")
else()
    set(BOARD_PORT_SRC "waveshare_rgb_lcd_port.c")
endif()

idf_component_register(
    SRCS
        "main.c"
        "${BOARD_PORT_SRC}"
        "lvgl_port.c"
        "geo_math.c"
        "http_util.c"
        "wifi_mgr.c"
        "geolocate.c"
        "flight_data.c"
        "routes.c"
        "logos.c"
        "flags.c"
        "alertlog.c"
        "runways.c"
        "airlines.c"
        "airports.c"
        "dailystats.c"
        "faflight.c"
        "metar.c"
        "units.c"
        "extras.c"
        "ships.c"
        "airspace.c"
        "regcountry.c"
        "flight_task.c"
        "geocode.c"
        "rainviewer.c"
        "img_plane.c"
        "img_heli.c"
        "img_small.c"
        "img_mil.c"
        "img_glider.c"
        "img_balloon.c"
        "img_drone.c"
        "jpeg_dec.c"
        "lang.c"
        "mqtt_pub.c"
        "notify.c"
        "obslog.c"
        "settings.c"
        "input_ctl.c"
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
        "font_pl_8.c"
        "font_pl_10.c"
        "font_pl_12.c"
        "font_pl_14.c"
        "font_pl_16.c"
        "font_pl_20.c"
    INCLUDE_DIRS "." ${BOARD_PORT_INC} "tab5"
    REQUIRES esp_wifi esp_http_client esp_http_server esp-tls nvs_flash json spiffs esp_netif app_update mqtt
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1)

idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -Wno-format)

if(CONFIG_ESPTOOLPY_FLASHSIZE_8MB)
    # curated asset set built by tools/make_assets_8mb.sh
    spiffs_create_partition_image(assets ../assets-8mb FLASH_IN_PROJECT)
elseif(CONFIG_ESPTOOLPY_FLASHSIZE_4MB)
    # logo-less asset set built by tools/make_assets_4mb.sh
    spiffs_create_partition_image(assets ../assets-4mb FLASH_IN_PROJECT)
else()
    spiffs_create_partition_image(assets ../assets FLASH_IN_PROJECT)
endif()
