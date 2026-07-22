#!/bin/sh
# Regenerate main/font_pl_*.c: Montserrat Medium with Latin Extended-A
# (Polish diacritics) + the LVGL built-in FontAwesome symbol set.
set -e
cd "$(dirname "$0")/../main"
TTF=../managed_components/lvgl__lvgl/scripts/built_in_font/Montserrat-Medium.ttf
SYM=../managed_components/lvgl__lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff
# LVGL symbol set + FontAwesome weather glyphs (sun, cloud-sun, cloud, smog,
# cloud-rain, cloud-showers-heavy, snowflake, bolt, wind)
SYMS=61441,61448,61451,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650,0xf185,0xf6c4,0xf0c2,0xf75f,0xf73d,0xf740,0xf2dc,0xf72e
for sz in 14 16 20; do
    npx --yes lv_font_conv@1.5.3 --bpp 4 --size "$sz" \
        --font "$TTF" -r 0x20-0x7F,0xA0-0x24F \
        --font "$SYM" -r "$SYMS" \
        --format lvgl --no-compress -o "font_pl_$sz.c"
done
