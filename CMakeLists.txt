cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "0.2.2")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
add_compile_options("-Wno-attributes")
# LVGL object allocations go to PSRAM (internal RAM is reserved for task
# stacks and Wi-Fi buffers); falls back to internal when PSRAM is full.
add_compile_options("-DLV_MEM_CUSTOM_ALLOC(s)=heap_caps_malloc_prefer(s,2,(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT),MALLOC_CAP_8BIT)")
add_compile_options("-DLV_MEM_CUSTOM_FREE(p)=heap_caps_free(p)")
add_compile_options("-DLV_MEM_CUSTOM_REALLOC(p,s)=heap_caps_realloc_prefer(p,s,2,(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT),MALLOC_CAP_8BIT)")
project(esp32flight)
