#!/usr/bin/env bash
# Airport throughput regression suite.
#
# Usage: scripts/regression_test.sh [--full] [--no-build] [--sequential]
#
# By default this runs the single T5j2 fixture: a real player layout under
# sustained contention, and at ~2 minutes the cheapest of the four by a wide
# margin. That is the normal check, before a commit included.
#
# --full adds the other three. It is not a per-commit gate: reach for it when a
# change has a real chance of breaking ground/taxi pathfinding. In particular
# T7d is the only fixture with genuine route diversity and the only probe of the
# wait-don't-downgrade tier, so alternate-routing work is invisible to the
# default run — and T7d alone costs ~13 minutes, which is why it is not it.
#
# Fixtures run concurrently: the simulation is single-threaded, so four of them
# cost roughly one wall-clock fixture on any multi-core machine. Each writes its
# own log (see the "log:" path each case prints) and none of them writes shared
# state, so the fixtures within a run do not interfere.
#
# Two *separate* suite runs still do: the log path is derived from the fixture
# name, so a second run truncates the first one's log out from under it and both
# sets of [AirportStats] lines are lost, which surfaces as "no countable years"
# and reads exactly like a broken fixture. Give a concurrent run its own
# --log-dir.

# Each fixture is checked two ways: total movements against the min_movements=
# floor in its .expected file, and the run's log against FAILURE_PATTERNS below.
# The log check exists because throughput alone hides small correctness faults --
# a few aircraft on a broken path cost a handful of movements out of thousands,
# comfortably inside the floor's headroom.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "${SCRIPT_DIR}/.."

# The default fixture. See the block comment above for why it is this one.
DEFAULT_CASES=(
	"scripts/testdata/T5j2.sav:scripts/testdata/T5j2.expected"
)

# --full. T7d stays first so the longest fixture starts first.
FULL_CASES=(
	"scripts/testdata/T7d.sav:scripts/testdata/T7d.expected"
	"scripts/testdata/T5j2.sav:scripts/testdata/T5j2.expected"
	"scripts/testdata/mass7-inair.sav:scripts/testdata/mass7-inair.expected"
	"scripts/testdata/helis2.sav:scripts/testdata/helis2.expected"
)

YEARS=5
run_build=1
run_parallel=1
# Matches n_years_plus2.sh's default, so the log paths stay where the docs and
# the interactive habits expect them.
log_dir="/tmp"
TEST_CASES=("${DEFAULT_CASES[@]}")

usage() {
	cat <<'USAGE'
Usage: scripts/regression_test.sh [--full] [--no-build] [--sequential]
                                 [--log-dir DIR]

  --full         Run every fixture (T5j2, T7d, mass7-inair, helis2) instead of
                 just T5j2. For changes that could break taxi pathfinding; the
                 bare run is the normal check. Costs ~13 minutes against the
                 default run's ~2, all of it T7d.
  --no-build     Skip the build; run whatever ./build/openttd already is.
  --sequential   Run the fixtures one at a time instead of concurrently.
  --log-dir DIR  Write the per-fixture openttd logs to DIR (default: /tmp).
                 Use this when another regression run may be in flight; the
                 default paths are shared and the two would clobber each other.

A fixture passes only if its movement total clears the floor in its .expected
file AND its log is free of every FAILURE_PATTERNS entry in this script.
USAGE
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--full) TEST_CASES=("${FULL_CASES[@]}") ;;
		--no-build) run_build=0 ;;
		--sequential) run_parallel=0 ;;
		--log-dir)
			shift
			[[ $# -gt 0 ]] || { echo "error: --log-dir needs a directory" >&2; exit 1; }
			log_dir="$1"
			;;
		-h|--help) usage; exit 0 ;;
		*) echo "error: unknown argument '$1'" >&2; usage >&2; exit 1 ;;
	esac
	shift
done

# run_case sets OPENTTD_REGRESSION_LOG per fixture, so honouring an inherited
# one would either be ignored or, worse, point every fixture at the same file.
if [[ -n "${OPENTTD_REGRESSION_LOG:-}" ]]; then
	echo "error: OPENTTD_REGRESSION_LOG is set, but this suite picks a log path per" >&2
	echo "       fixture. Use --log-dir DIR instead." >&2
	exit 1
fi

mkdir -p "${log_dir}"

# Log lines that mean the modular airport code went wrong, as opposed to the
# ordinary contention a busy fixture is *supposed* to produce. Each of these
# must be absent from a run; any occurrence fails the fixture.
#
# Deliberately NOT here, because a healthy busy airport emits them constantly:
# stuck(reserve) / stuck(occupied) / stuck(no-path), runway-rest-invariant (a
# rollout end is physically on the runway, so some waiting there is unavoidable
# and it self-clears), clamp pre-ground-move, retarget failed, landing-chain
# fail/reject, takeoff-skip, takeoff-path not enterable, and diverted.
#
# Format: <extended regex>|<what it means>. See skills/stuck_plane_debugging.md.
FAILURE_PATTERNS=(
	'landing-chain-invariant|aircraft off a safe stop with no reserved route to one'
	'\[FALLBACK\] stale-clear|reservation left behind by a vehicle that moved on'
	'\[FALLBACK\] force-clear-all|reservations force-dropped from under a stuck aircraft'
	'landing-chain restore failed|post-load landing chain could not be rebuilt'
	'rollout fallback failed|aircraft could not vacate the runway after landing'
	'invalid ground state|aircraft in a ground state at flight speed'
	'invalid HANGAR state|aircraft in HANGAR state off a hangar tile'
	'in unexpected state|aircraft in a state the modular handler does not know'
	'reached non-hangar tile|hangar target resolved to something that is not a hangar'
	'goal_data null|ground goal tile has no modular tile data'
	'helicopter touchdown without ground goal|helicopter landed with nowhere to go'
	'takeoff recovery failed|no runway available for a takeoff already committed'
	'takeoff recovery: invalid takeoff tile|takeoff target is not a runway piece'
	'cannot move further on Airport|classic FTA state machine dead end'
	'is not valid for current airport|classic FTA position out of range'
	'[Ee]xception in Aircraft::Tick|C++ exception escaped the tick handler'
	'\[AircraftLost\]|an aircraft crashed; every fixture has crashes off'
)

# Fails the fixture if the run's log contains any FAILURE_PATTERNS match.
# Prints one summary line per matching pattern plus the first example, so the
# report says which bug fired without replaying thousands of lines.
scan_log_for_failures() {
	local log_path="$1"
	local save_file="$2"
	local entry pattern description count example
	local found=0

	if [[ ! -f "${log_path}" ]]; then
		echo "FAIL: ${save_file}: log ${log_path} is missing; cannot check for failure patterns"
		return 1
	fi

	for entry in "${FAILURE_PATTERNS[@]}"; do
		pattern="${entry%%|*}"
		description="${entry#*|}"
		count="$(grep -cE "${pattern}" "${log_path}" || true)"
		[[ "${count}" -eq 0 ]] && continue
		found=1
		echo "FAIL: ${save_file}: ${count}x ${description}"
		example="$(grep -m1 -E "${pattern}" "${log_path}" || true)"
		echo "      ${example}"
	done

	if [[ "${found}" -eq 1 ]]; then
		echo "      Full log: ${log_path}"
		return 1
	fi
	return 0
}

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
		return 1
	fi

	echo "${min_movements}"
}

# Runs one fixture and reports PASS/FAIL. Writes only to stdout/stderr, so
# concurrent invocations can each be redirected to their own buffer.
run_case() {
	local save_file="$1"
	local expected_file="$2"
	local min_movements
	local run_output
	local actual
	local started
	local elapsed

	min_movements="$(read_min_movements "${expected_file}")" || return 1

	# Each fixture gets its own log. Concurrent fixtures must never share one:
	# the loser's [AirportStats] lines vanish and the run reports no countable
	# years, which looks like a paused fixture rather than a log collision.
	export OPENTTD_REGRESSION_LOG="${log_dir}/openttd_regression_$(basename "${save_file}" .sav).log"

	echo "Reference: ${save_file} min_movements=${min_movements}"
	echo "Running ${YEARS}-year simulation..."

	started="$(date +%s)"
	run_output="$(bash scripts/airport_stats_history.sh --current "${YEARS}" "${save_file}" 2>&1)" || {
		echo "FAIL: ${save_file}: run failed"
		echo "${run_output}"
		return 1
	}
	elapsed=$(($(date +%s) - started))

	if [[ "${run_output}" =~ movements=([0-9]+) ]]; then
		actual="${BASH_REMATCH[1]}"
	else
		echo "FAIL: ${save_file}: could not parse movements from output"
		echo "${run_output}"
		return 1
	fi

	echo "${run_output}" | grep '^log: ' || true
	echo "Actual:    ${save_file} movements=${actual} (${elapsed}s)"

	if [[ "${actual}" -lt "${min_movements}" ]]; then
		echo "FAIL: ${save_file}: movements ${actual} < minimum ${min_movements}"
		return 1
	fi

	# Throughput can hold up while the code is quietly misbehaving -- a handful of
	# aircraft taking a broken path costs a few movements out of thousands, well
	# inside the floor's headroom. So check the log as well as the total.
	scan_log_for_failures "${OPENTTD_REGRESSION_LOG}" "${save_file}" || return 1

	echo "PASS: ${save_file} (${actual} >= ${min_movements}, log clean)"
}

# Build once, here, rather than letting each fixture's n_years_plus2.sh do it.
# Concurrent fixtures would otherwise run concurrent `make` invocations against
# the same build directory and race over the same object files; and even
# sequentially, rebuilding once per fixture means a source edit mid-run splits
# the suite across two builds.
if [[ "${run_build}" -eq 1 ]]; then
	scripts/build_and_sign.sh
fi
export OPENTTD_SKIP_BUILD=1

if [[ ! -x ./build/openttd ]]; then
	echo "error: ./build/openttd is missing or not executable" >&2
	exit 1
fi

if [[ "${run_parallel}" -eq 0 ]]; then
	status=0
	for test_case in "${TEST_CASES[@]}"; do
		IFS=: read -r save_file expected_file <<< "${test_case}"
		run_case "${save_file}" "${expected_file}" || status=1
		echo
	done
	exit "${status}"
fi

# Parallel path. Each case's output is buffered to its own file and replayed in
# fixture order once everything finishes, so interleaved writes cannot scramble
# the report. `wait -n` needs bash 4.3 and macOS ships 3.2, so wait for all.
out_dir="$(mktemp -d "${TMPDIR:-/tmp}/openttd_regression.XXXXXX")"
trap 'rm -rf "${out_dir}"' EXIT

pids=()
for i in "${!TEST_CASES[@]}"; do
	IFS=: read -r save_file expected_file <<< "${TEST_CASES[$i]}"
	echo "Starting ${save_file} (${YEARS}y)"
	# `if` rather than `cmd; echo $?`: set -e would abort the subshell on a
	# failing run_case before the status file was ever written.
	(
		if run_case "${save_file}" "${expected_file}" > "${out_dir}/${i}.out" 2>&1; then
			echo 0 > "${out_dir}/${i}.status"
		else
			echo 1 > "${out_dir}/${i}.status"
		fi
	) &
	pids+=("$!")
done

echo "Waiting for ${#TEST_CASES[@]} fixture(s)..."
for pid in "${pids[@]}"; do
	wait "${pid}" || true
done

status=0
for i in "${!TEST_CASES[@]}"; do
	echo
	cat "${out_dir}/${i}.out"
	if [[ "$(cat "${out_dir}/${i}.status" 2>/dev/null || echo 1)" != "0" ]]; then
		status=1
	fi
done

echo
if [[ "${status}" -eq 0 ]]; then
	echo "All ${#TEST_CASES[@]} fixture(s) passed."
else
	echo "Regression suite FAILED."
fi
exit "${status}"
