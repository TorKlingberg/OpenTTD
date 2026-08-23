#!/usr/bin/env bash
set -euo pipefail

YEARS="${1:-1}"
SAVE_FILE="${2:-scripts/testdata/mass7-inair.sav}"

if ! [[ "${YEARS}" =~ ^[0-9]+$ ]]; then
	echo "error: years must be a non-negative integer" >&2
	exit 1
fi

DAY_TICKS=74
EXTRA_MONTH_DAYS=62
SAFETY_DAYS=7
TOTAL_DAYS=$((YEARS * 365 + EXTRA_MONTH_DAYS + SAFETY_DAYS))
TOTAL_TICKS=$((TOTAL_DAYS * DAY_TICKS))

# /tmp/openttd.log belongs to the interactive runners (build_and_run*.sh and the lldb
# workflows), and a game started by one of those holds it open as its stdout for as long as
# it runs. Truncating that path here does not move the live game's file offset, so the two
# processes interleave and the [AirportStats] lines this run depends on are overwritten —
# which surfaces much later as a regression "failure" with no countable years. Keep batch
# runs on their own path. One log per fixture, overwritten each run, so repeated runs do not
# accumulate multi-megabyte files in /tmp.
LOG_FILE="${OPENTTD_REGRESSION_LOG:-/tmp/openttd_regression_$(basename "${SAVE_FILE}" .sav).log}"

# Callers that drive several fixtures at once (scripts/regression_test.sh) build
# once themselves and set this. Concurrent `make` invocations against the same
# build directory would race over the same object files, and rebuilding per
# fixture also lets a mid-run source edit split a suite across two builds.
if [[ "${OPENTTD_SKIP_BUILD:-0}" != "1" ]]; then
	scripts/build_and_sign.sh
fi
./build/openttd -d misc=1 -x -g "${SAVE_FILE}" -s null -m null -v null:ticks="${TOTAL_TICKS}" > "${LOG_FILE}" 2>&1
echo "log: ${LOG_FILE}"
rg "\[AirportStats\] Year [0-9]+ totals" "${LOG_FILE}" || true
