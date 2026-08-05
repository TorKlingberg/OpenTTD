#!/usr/bin/env bash
set -euo pipefail

## --current mode: build + run once on the working tree, no git interaction.
if [[ "${1:-}" == "--current" ]]; then
	YEARS_TO_RUN="${2:-1}"
	SAVE_FILE="${3:-scripts/testdata/mass7-inair.sav}"
	if ! [[ "${YEARS_TO_RUN}" =~ ^[0-9]+$ ]]; then
		echo "error: years_to_run must be a non-negative integer" >&2
		exit 1
	fi
	echo "Running current working tree (years=${YEARS_TO_RUN}, save=${SAVE_FILE})"
	run_output="$(bash scripts/n_years_plus2.sh "${YEARS_TO_RUN}" "${SAVE_FILE}" 2>&1)" || { echo "${run_output}"; exit 1; }
	echo "${run_output}"
	landings_total=0
	takeoffs_total=0
	first_year=""
	years_counted=0
	while IFS= read -r line; do
		if [[ "${line}" =~ Year[[:space:]]([0-9]+)[[:space:]]totals:[[:space:]]landings=([0-9]+)[[:space:]]takeoffs=([0-9]+) ]]; then
			year="${BASH_REMATCH[1]}"
			# Skip the first reported year: it is a partial warmup year whose
			# length depends on the save's start date, so it is not comparable.
			if [[ -z "${first_year}" ]]; then
				first_year="${year}"
				continue
			fi
			landings_total=$((landings_total + BASH_REMATCH[2]))
			takeoffs_total=$((takeoffs_total + BASH_REMATCH[3]))
			years_counted=$((years_counted + 1))
		fi
	done <<< "${run_output}"

	# A run that reported no years at all did not simulate anything, which is a
	# broken fixture rather than a throughput result. Distinguish it explicitly:
	# "movements=0" parses just fine downstream and would otherwise be read as a
	# catastrophic regression, or silently pass against a zero floor.
	if [[ "${years_counted}" -eq 0 ]]; then
		echo "error: ${SAVE_FILE} produced no countable [AirportStats] years." >&2
		echo "       The simulation did not advance. The usual cause is a savegame that was" >&2
		echo "       paused when it was saved: the tick budget is consumed while the game loop" >&2
		echo "       does nothing, so the run finishes in seconds with no output." >&2
		echo "       Load it, unpause, and re-save. Regression fixtures must be unpaused." >&2
		exit 1
	fi

	echo "Total (excl first year): landings=${landings_total} takeoffs=${takeoffs_total} movements=$((landings_total + takeoffs_total)) years=${years_counted}"
	exit 0
fi

START_COMMIT="${1:-42029f9699}"
OUT_DIR="${2:-/tmp/airport_stats_history_$(date +%Y%m%d_%H%M%S)}"
YEARS_TO_RUN="${3:-1}"
CSV_PATH="${OUT_DIR}/airport_stats.csv"
RUN_LOG="${OUT_DIR}/runner.log"

if ! git rev-parse --verify "${START_COMMIT}^{commit}" >/dev/null 2>&1; then
	echo "error: start commit '${START_COMMIT}' was not found" >&2
	exit 1
fi

if ! [[ "${YEARS_TO_RUN}" =~ ^[0-9]+$ ]]; then
	echo "error: years_to_run must be a non-negative integer" >&2
	exit 1
fi

if ! git diff --quiet || ! git diff --cached --quiet; then
	echo "error: working tree is not clean; commit or stash first" >&2
	exit 1
fi

mkdir -p "${OUT_DIR}"

echo "commit,subject,years_requested,landings_excl_first_year,takeoffs_excl_first_year,total_movements_excl_first_year,status" > "${CSV_PATH}"

orig_commit="$(git rev-parse --verify HEAD)"
orig_branch="$(git symbolic-ref -q --short HEAD || true)"
restore_ref="${orig_commit}"
if [[ -n "${orig_branch}" ]]; then
	restore_ref="${orig_branch}"
fi

cleanup() {
	git checkout -q "${restore_ref}" || true
}
trap cleanup EXIT

commits=()
while IFS= read -r commit; do
	commits+=("${commit}")
done < <(git rev-list --reverse "${START_COMMIT}^..HEAD")
total="${#commits[@]}"

if [[ "${total}" -eq 0 ]]; then
	echo "error: no commits found in range ${START_COMMIT}^..HEAD" >&2
	exit 1
fi

echo "Running ${total} commits from ${START_COMMIT} to HEAD (years=${YEARS_TO_RUN}, plus 2 months)"
echo "Output directory: ${OUT_DIR}"
echo "Running ${total} commits from ${START_COMMIT} to HEAD (years=${YEARS_TO_RUN}, plus 2 months)" >> "${RUN_LOG}"

for i in "${!commits[@]}"; do
	commit="${commits[$i]}"
	step="$((i + 1))"
	subject="$(git show -s --format=%s "${commit}")"
	safe_subject="${subject//\"/\"\"}"
	commit_log="${OUT_DIR}/${step}_${commit}.log"
	status="ok"
	landings_total=0
	takeoffs_total=0
	movement_total=0
	first_year=""

	echo "[${step}/${total}] ${commit} ${subject}" | tee -a "${RUN_LOG}"
	git checkout -q "${commit}"

	if run_output="$(bash scripts/n_years_plus2.sh "${YEARS_TO_RUN}" 2>&1)"; then
		printf "%s\n" "${run_output}" > "${commit_log}"
		stats_lines="$(printf "%s\n" "${run_output}" | rg '\[AirportStats\] Year [0-9]+ totals: landings=[0-9]+ takeoffs=[0-9]+' || true)"
		if [[ -z "${stats_lines}" ]]; then
			status="missing_stats"
		else
			while IFS= read -r stats_line; do
				if [[ "${stats_line}" =~ Year[[:space:]]([0-9]+)[[:space:]]totals:[[:space:]]landings=([0-9]+)[[:space:]]takeoffs=([0-9]+) ]]; then
					year="${BASH_REMATCH[1]}"
					landings="${BASH_REMATCH[2]}"
					takeoffs="${BASH_REMATCH[3]}"
					# Skip the first reported year (partial warmup year).
					if [[ -z "${first_year}" ]]; then
						first_year="${year}"
					else
						landings_total=$((landings_total + landings))
						takeoffs_total=$((takeoffs_total + takeoffs))
					fi
				else
					status="parse_error"
				fi
			done <<< "${stats_lines}"

			if [[ "${status}" == "ok" ]]; then
				movement_total=$((landings_total + takeoffs_total))
			fi
		fi
	else
		status="run_failed"
		printf "%s\n" "${run_output}" > "${commit_log}"
	fi

	if [[ "${status}" == "ok" ]]; then
		summary="  -> ok landings(excl_first_year)=${landings_total} takeoffs(excl_first_year)=${takeoffs_total} total=${movement_total}"
	else
		summary="  -> ${status} (see ${commit_log})"
	fi
	echo "${summary}" | tee -a "${RUN_LOG}"

	echo "${commit},\"${safe_subject}\",${YEARS_TO_RUN},${landings_total},${takeoffs_total},${movement_total},${status}" >> "${CSV_PATH}"
done

echo "Done. CSV: ${CSV_PATH}"
