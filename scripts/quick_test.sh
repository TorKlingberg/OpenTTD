#!/usr/bin/env bash
# Quick build + per-save timing/movement check.
# Usage: scripts/quick_test.sh [YEARS] [SAVE_FILE...]
#   Default YEARS=1, default saves: mass7-inair.sav, helis2.sav
set -euo pipefail

cd "$(dirname "$0")/.."

YEARS="${1:-1}"
shift || true

if [ "$#" -gt 0 ]; then
	SAVES=("$@")
else
	SAVES=(scripts/testdata/mass7-inair.sav scripts/testdata/helis2.sav)
fi

scripts/build_and_sign.sh

DAY_TICKS=74
EXTRA_MONTH_DAYS=62
SAFETY_DAYS=7
TOTAL_DAYS=$((YEARS * 365 + EXTRA_MONTH_DAYS + SAFETY_DAYS))
TOTAL_TICKS=$((TOTAL_DAYS * DAY_TICKS))

for save in "${SAVES[@]}"; do
	echo
	echo "=== $save (${YEARS}y) ==="
	START=$(date +%s)
	./build/openttd -d misc=1 -x -g "$save" -s null -m null -v null:ticks="$TOTAL_TICKS" > /tmp/openttd_quick.log 2>&1
	END=$(date +%s)
	ELAPSED=$((END - START))
	echo "elapsed: ${ELAPSED}s"
	rg "\[AirportStats\] Year [0-9]+ totals" /tmp/openttd_quick.log || echo "(no AirportStats lines)"
done
