#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "${SCRIPT_DIR}/.."

EXPECTED_FILE="scripts/testdata/mass6-inair.expected"

# Read minimum threshold from expected file.
min_movements=""
while IFS= read -r line; do
	[[ "${line}" =~ ^#.*$ || -z "${line}" ]] && continue
	if [[ "${line}" =~ ^min_movements=([0-9]+)$ ]]; then
		min_movements="${BASH_REMATCH[1]}"
	fi
done < "${EXPECTED_FILE}"

if [[ -z "${min_movements}" ]]; then
	echo "FAIL: could not read min_movements from ${EXPECTED_FILE}" >&2
	exit 1
fi

echo "Reference: min_movements=${min_movements}"
echo "Running 1-year simulation..."

run_output="$(bash scripts/airport_stats_history.sh --current 1 2>&1)" || {
	echo "FAIL: run failed"
	echo "${run_output}"
	exit 1
}

# Extract the totals line.
if [[ "${run_output}" =~ movements=([0-9]+) ]]; then
	actual="${BASH_REMATCH[1]}"
else
	echo "FAIL: could not parse movements from output"
	echo "${run_output}"
	exit 1
fi

echo "Actual:    movements=${actual}"

if [[ "${actual}" -lt "${min_movements}" ]]; then
	echo "FAIL: movements ${actual} < minimum ${min_movements}"
	exit 1
fi

echo "PASS (${actual} >= ${min_movements})"
