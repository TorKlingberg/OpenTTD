#!/usr/bin/env bash
# Sample-profile a helis.sav run.
# Usage: scripts/profile_helis.sh [DAYS]
set -euo pipefail

cd "$(dirname "$0")/.."

DAYS="${1:-365}"
DAY_TICKS=74
TICKS=$((DAYS * DAY_TICKS))

scripts/build_and_sign.sh > /dev/null

./build/openttd -d misc=0 -x -g scripts/testdata/helis.sav -s null -m null -v null:ticks="$TICKS" > /tmp/openttd_prof.log 2>&1 &
PID=$!
echo "openttd PID=$PID, sampling for 30s..."
sleep 5
sample "$PID" 25 -file /tmp/openttd_helis_sample.txt > /dev/null 2>&1
wait "$PID" || true

echo
echo "=== Top hotspots ==="
rg -A 0 "^\s+[0-9]+ +[A-Za-z_]" /tmp/openttd_helis_sample.txt | head -40 || true
echo
echo "Full sample written to /tmp/openttd_helis_sample.txt"
