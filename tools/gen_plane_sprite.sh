#!/bin/sh
# Generate main/img_plane.c: a white 28x28 plane silhouette (nose up) as an
# LVGL true-color-alpha C array. The UI recolors it per altitude band and
# rotates it with lv_img_set_angle.
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
magick -size 28x28 xc:none -fill white \
    -draw "path 'M 14,1 L 16.2,10 L 26,15.5 L 26,18 L 16.4,15.4 L 15.4,21.5 L 18.8,24.4 L 18.8,26 L 14,24.6 L 9.2,26 L 9.2,24.4 L 12.6,21.5 L 11.6,15.4 L 2,18 L 2,15.5 L 11.8,10 Z'" \
    "$tmp/plane.png"
magick "$tmp/plane.png" -depth 8 "$tmp/plane.rgba"
python3 - "$tmp/plane.rgba" main/img_plane.c <<'EOF'
import sys

W = H = 28
raw = open(sys.argv[1], "rb").read()
assert len(raw) == W * H * 4

out = []
for i in range(W * H):
    r, g, b, a = raw[i*4:i*4+4]
    px = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    out += [px & 0xFF, px >> 8, a]

with open(sys.argv[2], "w") as f:
    f.write('#include "lvgl.h"\n\n')
    f.write("static const uint8_t plane_map[] = {\n")
    for i in range(0, len(out), 24):
        f.write("    " + ",".join(str(b) for b in out[i:i+24]) + ",\n")
    f.write("};\n\n")
    f.write("const lv_img_dsc_t img_plane = {\n")
    f.write("    .header.always_zero = 0,\n")
    f.write("    .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,\n")
    f.write(f"    .header.w = {W},\n")
    f.write(f"    .header.h = {H},\n")
    f.write(f"    .data_size = {len(out)},\n")
    f.write("    .data = plane_map,\n")
    f.write("};\n")
print("img_plane.c written")
EOF
rm -rf "$tmp"
