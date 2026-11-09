# apkflight desktop build (macOS/SDL2). The core is compiled straight out
# of ../esp32flight/main - one source of truth, two targets.

ESPF := ..
LVGL := $(ESPF)/managed_components/lvgl__lvgl

# One version for firmware and app: taken from the firmware's PROJECT_VER.
# Written to a header (not -D) so incremental builds notice version bumps.
APKVER := $(shell sed -n 's/.*set(PROJECT_VER "\([0-9.]*\)").*/\1/p' $(ESPF)/CMakeLists.txt | head -1)
$(shell new='#define APK_VERSION "$(APKVER)"'; \
        [ -f shim/apk_version.h ] && [ "$$(cat shim/apk_version.h)" = "$$new" ] || echo "$$new" > shim/apk_version.h)

CC      := clang
CFLAGS  := -O2 -std=gnu11 -Wno-format \
           -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DAPKFLIGHT_NO_WIFI -DAPKFLIGHT \
           -include shim/path_remap.h \
           -I. -Ishim -Ivendor -I$(ESPF)/main -I$(LVGL) -I$(LVGL)/src \
           $(shell sdl2-config --cflags) -I$(shell sdl2-config --prefix)/include
CURL_PREFIX := $(shell brew --prefix curl 2>/dev/null)
ifneq ($(CURL_PREFIX),)
CFLAGS  += -I$(CURL_PREFIX)/include
LDLIBS  := $(shell sdl2-config --libs) -L$(CURL_PREFIX)/lib -lcurl -lpthread -lm
else
LDLIBS  := $(shell sdl2-config --libs) -lcurl -lpthread -lm
endif

LVGL_SRC := $(shell find $(LVGL)/src -name '*.c')

# Device-only modules replaced by platform/ or shim/ equivalents
CORE_EXCLUDE := main.c lvgl_port.c waveshare_rgb_lcd_port.c tab5_lcd_port.c jc10_lcd_port.c elecrow10_lcd_port.c wifi_mgr.c \
                mqtt_pub.c settings.c http_util.c
CORE_SRC := $(filter-out $(addprefix $(ESPF)/main/, $(CORE_EXCLUDE)), \
                         $(wildcard $(ESPF)/main/*.c))
PLAT_SRC := $(wildcard platform/*.c) vendor/cJSON.c
# apk_settings.c + others picked up by the wildcard above

apkflight: $(LVGL_SRC) $(CORE_SRC) $(PLAT_SRC) lv_conf.h
	$(CC) $(CFLAGS) $(LVGL_SRC) $(CORE_SRC) $(PLAT_SRC) -o $@ $(LDLIBS)

clean:
	rm -f apkflight apkflight-shot.bmp

.PHONY: clean

# Stage the device /assets content + CA bundle into the APK assets dir
android-assets:
	rm -rf android/app/src/main/assets/appdata
	mkdir -p android/app/src/main/assets/appdata
	rsync -aL assets/ android/app/src/main/assets/appdata/ \
	    --exclude '*.tsv.old' --exclude 'obslog*' --exclude 'alerts*'
	cp third_party/cacert.pem android/app/src/main/assets/appdata/cacert.pem
	cd android/app/src/main/assets/appdata && find . -type f | sed 's|^\./||' > manifest.txt

.PHONY: android-assets
