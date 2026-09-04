#!/usr/bin/env bash
# Remove every AI company from a savegame and stop new ones from being started,
# so the fixture measures only the human company's airports.
#
# AI companies are a noise source in the regression fixtures: which airports an
# AI decides to build shifts with any change that consumes the synced Random()
# differently, which moves fixture totals by a percent or two with no routing
# cause. See "Comparing two runs" in skills/regression_testing.md.
#
# Uses the same scripts/game_start.scr hook as resave.sh, so the state written
# is the loaded state with the AI companies deleted and zero ticks simulated.
# 'stop_ai' deletes the company outright, taking its stations and vehicles with
# it; 'difficulty.max_no_competitors = 0' keeps MaybeStartNewCompany() from
# spawning replacements during the run.
#
# No need to run this on scripts/testdata/helis2.sav or mass7-inair.sav: neither
# ever had AI companies. (A re-save does not cost helis2 its stale-descent-flag
# coverage -- Aircraft::flags round-trips; see skills/regression_testing.md.)
#
# Usage: scripts/strip_ai.sh <savegame> [<savegame> ...]
# Files are rewritten in place.
set -euo pipefail

OPENTTD="${OPENTTD_BIN:-/Users/tor/ttd/OpenTTD/build/openttd}"
[[ -x "${OPENTTD}" ]] || { echo "error: no openttd binary at ${OPENTTD}" >&2; exit 1; }

SAVE_DIR="${OPENTTD_PERSONAL_DIR:-${HOME}/Documents/OpenTTD}/save"
STAMP="strip_ai_tmp_$$"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}" "${SAVE_DIR}/${STAMP}.sav"' EXIT
mkdir -p "${WORK_DIR}/scripts"

# 'stop_ai' refuses human companies and unknown ids with a console error, so
# sweeping the whole company range needs no knowledge of the save.
{
	echo "setting max_no_competitors 0"
	for id in $(seq 1 15); do echo "stop_ai ${id}"; done
	echo "save ${STAMP}"
	echo "exit"
} > "${WORK_DIR}/scripts/game_start.scr"

# Listing companies needs console output on stdout, which only the dedicated
# server driver gives. It is a read-only look at the result, never the run that
# writes the file.
list_companies() {
	local dir
	dir="$(mktemp -d)"
	mkdir -p "${dir}/scripts"
	printf 'companies\nexit\n' > "${dir}/scripts/game_start.scr"
	(cd "${dir}" && "${OPENTTD}" -D -x -g "$1" -s null -m null) 2>&1 | grep '^#:' || true
	rm -rf "${dir}"
}

for save in "$@"; do
	[[ -f "${save}" ]] || { echo "error: no such savegame: ${save}" >&2; exit 1; }
	abs="$(cd "$(dirname "${save}")" && pwd)/$(basename "${save}")"

	before="$(list_companies "${abs}" | grep -c 'AI$' || true)"

	rm -f "${SAVE_DIR}/${STAMP}.sav"
	(cd "${WORK_DIR}" && "${OPENTTD}" -x -g "${abs}" -s null -m null -v null:ticks=5) >"${WORK_DIR}/openttd.log" 2>&1

	if [[ ! -f "${SAVE_DIR}/${STAMP}.sav" ]]; then
		echo "error: strip produced no file for ${save}; see ${WORK_DIR}/openttd.log" >&2
		cat "${WORK_DIR}/openttd.log" >&2
		exit 1
	fi

	mv "${SAVE_DIR}/${STAMP}.sav" "${abs}"
	after="$(list_companies "${abs}" | grep -c 'AI$' || true)"
	echo "${save}: AI companies ${before} -> ${after}"
done
