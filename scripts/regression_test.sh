#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "${SCRIPT_DIR}/.."

TEST_CASES=(
	"scripts/testdata/mass7-inair.sav:scripts/testdata/mass7-inair.expected"
	"scripts/testdata/helis2.sav:scripts/testdata/helis2.expected"
)

read_min_movements() {
	local expected_file="$1"
	local min_movements=""

	while IFS= read -r line; do
		[[ "${line}" =~ ^#.*$ || -z "${line}" ]] && continue
		if [[ "${line}" =~ ^min_movements=([0-9]+)$ ]]; then
			min_movements="${BASH_REMATCH[1]}"
		fi
	done < "${expected_file}"

	if [[ -z "${min_movements}" ]]; then
		echo "FAIL: could not read min_movements from ${expected_file}" >&2
		exit 1
	fi

	echo "${min_movements}"
}

run_case() {
	local save_file="$1"
	local expected_file="$2"
	local min_movements
	local run_output
	local actual

	min_movements="$(read_min_movements "${expected_file}")"

	echo "Reference: ${save_file} min_movements=${min_movements}"
	echo "Running 5-year simulation..."

	run_output="$(bash scripts/airport_stats_history.sh --current 5 "${save_file}" 2>&1)" || {
		echo "FAIL: ${save_file}: run failed"
		echo "${run_output}"
		exit 1
	}

	if [[ "${run_output}" =~ movements=([0-9]+) ]]; then
		actual="${BASH_REMATCH[1]}"
	else
		echo "FAIL: ${save_file}: could not parse movements from output"
		echo "${run_output}"
		exit 1
	fi

	echo "Actual:    ${save_file} movements=${actual}"

	if [[ "${actual}" -lt "${min_movements}" ]]; then
		echo "FAIL: ${save_file}: movements ${actual} < minimum ${min_movements}"
		exit 1
	fi

	echo "PASS: ${save_file} (${actual} >= ${min_movements})"
}

for test_case in "${TEST_CASES[@]}"; do
	IFS=: read -r save_file expected_file <<< "${test_case}"
	run_case "${save_file}" "${expected_file}"
done
