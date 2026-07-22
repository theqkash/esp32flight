#!/usr/bin/env bash
# Download 20 px tall country flag PNGs from flagcdn.com into assets/flags/.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p assets/flags

codes=$(curl -fsSL https://flagcdn.com/en/codes.json |
    python3 -c "import json,sys; print(' '.join(k for k in json.load(sys.stdin) if '-' not in k))")

n=0
for cc in $codes; do
    curl -fsSL "https://flagcdn.com/h20/${cc}.png" -o "assets/flags/${cc}.png" || {
        echo "skip ${cc}"; continue; }
    n=$((n + 1))
done
echo "fetched ${n} flags -> assets/flags/ ($(du -sh assets/flags | cut -f1))"
