#!/bin/sh
# Build assets/runways.tsv from the OurAirports public-domain dataset:
# open runways of large/medium airports with scheduled service, one line per
# runway: ICAO<TAB>LE_LAT<TAB>LE_LON<TAB>HE_LAT<TAB>HE_LON
set -e
cd "$(dirname "$0")/.."
tmpa=$(mktemp)
tmpr=$(mktemp)
curl -sL -o "$tmpa" "https://davidmegginson.github.io/ourairports-data/airports.csv"
curl -sL -o "$tmpr" "https://davidmegginson.github.io/ourairports-data/runways.csv"
python3 - "$tmpa" "$tmpr" assets/runways.tsv <<'EOF'
import csv, sys

keep = {}
with open(sys.argv[1], encoding="utf-8") as f:
    for r in csv.DictReader(f):
        if r["type"] not in ("large_airport", "medium_airport"):
            continue
        if r["scheduled_service"] != "yes":
            continue
        icao = r["icao_code"] or r["ident"]
        if len(icao) == 4:
            keep[r["ident"]] = icao

rows = 0
with open(sys.argv[2], encoding="utf-8") as f, \
     open(sys.argv[3], "w", encoding="utf-8") as out:
    for r in csv.DictReader(f):
        icao = keep.get(r["airport_ident"])
        if icao is None or r["closed"] == "1":
            continue
        c = [r["le_latitude_deg"], r["le_longitude_deg"],
             r["he_latitude_deg"], r["he_longitude_deg"]]
        if not all(c):
            continue
        out.write("\t".join([icao] + c) + "\n")
        rows += 1
print(f"runways: {rows}")
EOF
rm -f "$tmpa" "$tmpr"
ls -lh assets/runways.tsv
