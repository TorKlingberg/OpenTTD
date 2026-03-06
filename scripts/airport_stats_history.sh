#!/usr/bin/env bash
set -euo pipefail

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

echo "commit,subject,years_requested,landings_excl_2016,takeoffs_excl_2016,total_movements_excl_2016,status" > "${CSV_PATH}"

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
					if [[ "${year}" != "2016" ]]; then
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
		summary="  -> ok landings(excl_2016)=${landings_total} takeoffs(excl_2016)=${takeoffs_total} total=${movement_total}"
	else
		summary="  -> ${status} (see ${commit_log})"
	fi
	echo "${summary}" | tee -a "${RUN_LOG}"

	echo "${commit},\"${safe_subject}\",${YEARS_TO_RUN},${landings_total},${takeoffs_total},${movement_total},${status}" >> "${CSV_PATH}"
done

echo "Done. CSV: ${CSV_PATH}"
