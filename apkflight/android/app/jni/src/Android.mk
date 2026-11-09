LOCAL_PATH := $(call my-dir)

APK_ROOT := $(LOCAL_PATH)/../../../..
ESPF     := $(APK_ROOT)/..
LVGL     := $(ESPF)/managed_components/lvgl__lvgl
PREBUILT := $(APK_ROOT)/third_party/prebuilt/$(TARGET_ARCH_ABI)

# One version for firmware and app: taken from the firmware's PROJECT_VER.
# Emitted as a generated header, not a -D flag: make tracks header changes,
# so bumping PROJECT_VER rebuilds the shim (a -D change would be ignored by
# incremental builds and the app would keep reporting the old version).
APKVER := $(shell sed -n 's/.*set(PROJECT_VER "\([0-9.]*\)").*/\1/p' $(ESPF)/CMakeLists.txt | head -1)
APKVER_H := $(APK_ROOT)/shim/apk_version.h
$(shell new='#define APK_VERSION "$(APKVER)"'; \
        [ -f $(APKVER_H) ] && [ "$$(cat $(APKVER_H))" = "$$new" ] || echo "$$new" > $(APKVER_H))

# --- curl + mbedTLS built by scripts/build_android_deps.sh ---

include $(CLEAR_VARS)
LOCAL_MODULE := curl
LOCAL_SRC_FILES := $(PREBUILT)/lib/libcurl.a
LOCAL_EXPORT_C_INCLUDES := $(PREBUILT)/include
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := mbedtls
LOCAL_SRC_FILES := $(PREBUILT)/lib/libmbedtls.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := mbedx509
LOCAL_SRC_FILES := $(PREBUILT)/lib/libmbedx509.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := mbedcrypto
LOCAL_SRC_FILES := $(PREBUILT)/lib/libmbedcrypto.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := everest
LOCAL_SRC_FILES := $(PREBUILT)/lib/libeverest.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := p256m
LOCAL_SRC_FILES := $(PREBUILT)/lib/libp256m.a
include $(PREBUILT_STATIC_LIBRARY)

# --- the app: LVGL + esp32flight core + platform layer ---

include $(CLEAR_VARS)
LOCAL_MODULE := main

CORE_EXCLUDE := main.c lvgl_port.c waveshare_rgb_lcd_port.c tab5_lcd_port.c jc10_lcd_port.c elecrow10_lcd_port.c wifi_mgr.c \
                mqtt_pub.c settings.c http_util.c

LVGL_SRC := $(shell find $(LVGL)/src -name '*.c')
CORE_SRC := $(filter-out $(addprefix $(ESPF)/main/, $(CORE_EXCLUDE)), \
                         $(wildcard $(ESPF)/main/*.c))
PLAT_SRC := $(wildcard $(APK_ROOT)/platform/*.c) $(APK_ROOT)/vendor/cJSON.c

LOCAL_SRC_FILES := $(LVGL_SRC) $(CORE_SRC) $(PLAT_SRC)

LOCAL_C_INCLUDES := $(APK_ROOT) $(APK_ROOT)/shim $(APK_ROOT)/vendor \
                    $(ESPF)/main $(LVGL) $(LVGL)/src

LOCAL_CFLAGS := -O2 -Wno-format \
                -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DAPKFLIGHT_NO_WIFI -DAPKFLIGHT \
                -include $(APK_ROOT)/shim/path_remap.h

LOCAL_SHARED_LIBRARIES := SDL2
LOCAL_STATIC_LIBRARIES := curl mbedtls mbedx509 mbedcrypto everest p256m
LOCAL_LDLIBS := -llog -landroid -lz

include $(BUILD_SHARED_LIBRARY)
