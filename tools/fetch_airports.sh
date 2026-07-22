#!/bin/sh
# Build assets/airports.tsv from the OurAirports public-domain dataset:
# one line per airport with an IATA code, tab-separated:
# ICAO<TAB>IATA<TAB>CITY<TAB>COUNTRY<TAB>LAT<TAB>LON<TAB>NAME
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp)
curl -sL -o "$tmp" "https://davidmegginson.github.io/ourairports-data/airports.csv"
python3 - "$tmp" assets/airports.tsv <<'EOF'
import csv, sys

rows = 0
with open(sys.argv[1], encoding="utf-8") as f, \
     open(sys.argv[2], "w", encoding="utf-8") as out:
    for r in csv.DictReader(f):
        if not r["iata_code"] or r["type"] not in (
                "large_airport", "medium_airport", "small_airport"):
            continue
        icao = r["icao_code"] or r["ident"]
        if len(icao) != 4:
            continue
        fields = [icao, r["iata_code"], r["municipality"], r["iso_country"],
                  r["latitude_deg"], r["longitude_deg"], r["name"]]
        out.write("\t".join(x.replace("\t", " ") for x in fields) + "\n")
        rows += 1
print(f"airports: {rows}")
EOF
rm -f "$tmp"
ls -lh assets/airports.tsv
