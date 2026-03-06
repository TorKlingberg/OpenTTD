#!/usr/bin/env bash
set -euo pipefail

YEARS="${1:-1}"

if ! [[ "${YEARS}" =~ ^[0-9]+$ ]]; then
	echo "error: years must be a non-negative integer" >&2
	exit 1
fi

DAY_TICKS=74
EXTRA_MONTH_DAYS=62
SAFETY_DAYS=7
TOTAL_DAYS=$((YEARS * 365 + EXTRA_MONTH_DAYS + SAFETY_DAYS))
TOTAL_TICKS=$((TOTAL_DAYS * DAY_TICKS))

scripts/build_and_sign.sh \
	&& ./build/openttd -d misc=1 -x -g /Users/tor/Documents/OpenTTD/save/mass6-inair.sav -s null -m null -v null:ticks="${TOTAL_TICKS}" > /tmp/openttd.log 2>&1 \
	&& rg "\[AirportStats\] Year [0-9]+ totals" /tmp/openttd.log
