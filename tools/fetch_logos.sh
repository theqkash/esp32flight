#!/bin/sh
# Populate assets/logos/ with airline logo PNGs keyed by ICAO code.
# Primary: sexym0nk3y/airline-logos (90x90); gaps filled from
# Jxck-S/airline-logos (radarbox set). Each logo is normalized: white
# backgrounds/borders removed via corner flood-fill, trimmed, centered
# on a transparent 90x90 canvas.
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
git clone -q --depth 1 https://github.com/sexym0nk3y/airline-logos "$tmp/a"
git clone -q --depth 1 https://github.com/Jxck-S/airline-logos "$tmp/b"
mkdir -p assets/logos
rm -f assets/logos/*.png
cp "$tmp/a/logos/"*.png assets/logos/
for f in "$tmp/b/radarbox_logos/"*.png; do
    n=$(basename "$f")
    [ -e "assets/logos/$n" ] || cp "$f" "assets/logos/$n"
done
rm -rf "$tmp"
# Flatten onto a white "chip" with rounded corners baked in: native
# antialiasing preserved, no fringe on any theme (FR24 style).
magick -size 90x90 xc:none -fill white -draw "roundrectangle 0,0,89,89,14,14" /tmp/logo_mask.png
for f in assets/logos/*.png; do
    magick "$f" -fuzz 4% -trim +repage -resize 80x80 \
        -background white -alpha remove \
        -gravity center -extent 90x90 \
        /tmp/logo_mask.png -compose DstIn -composite \
        -compose Over -stroke '#a9b1be' -strokewidth 2 -fill none \
        -draw "roundrectangle 1,1,88,88,14,14" \
        -strip -define png:compression-level=9 PNG32:"$f" 2>/dev/null || true
done
rm -f /tmp/logo_mask.png

# Brand aliases: subsidiaries sharing the parent's livery/logo
alias_logo() { [ -e "assets/logos/$2.png" ] && cp "assets/logos/$2.png" "assets/logos/$1.png"; }
alias_logo WMT WZZ   # Wizz Air Malta
alias_logo WAZ WZZ   # Wizz Air Abu Dhabi
alias_logo WUK WZZ   # Wizz Air UK
alias_logo EJU EZY   # easyJet Europe
alias_logo EZS EZY   # easyJet Switzerland
alias_logo RUK RYR   # Ryanair UK
alias_logo RYS RYR   # Buzz (Ryanair Sun)
alias_logo MAY RYR   # Malta Air
alias_logo NOZ NAX   # Norwegian Air Sweden
alias_logo KLC KLM   # KLM Cityhopper
alias_logo LDA LOT   # LOT - regional ops

echo "Logos: $(ls assets/logos/*.png | wc -l), total $(du -sh assets/logos | cut -f1)"
