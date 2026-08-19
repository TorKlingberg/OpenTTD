#!/usr/bin/env bash
# Load a savegame headlessly and immediately re-save it in the current savegame
# format, without advancing the simulation.
#
# The hook is scripts/game_start.scr, which OnStartGame() execs right after
# SM_LOAD_GAME finishes loading and *before* the first StateGameLoop() tick, so
# the written state is the loaded state converted to SAVEGAME_VERSION and
# nothing else. The script runs from a private working directory so its
# game_start.scr shadows nothing in the repo or the personal dir.
#
# Usage: scripts/resave.sh <savegame> [<savegame> ...]
# Files are rewritten in place.
set -euo pipefail

OPENTTD="${OPENTTD_BIN:-/Users/tor/ttd/OpenTTD/build/openttd}"
[[ -x "${OPENTTD}" ]] || { echo "error: no openttd binary at ${OPENTTD}" >&2; exit 1; }

# 'save <name>' always writes to <personal dir>/save/<name>.sav; there is no
# console command that takes an arbitrary path, so pick a scratch name there and
# move the result into place afterwards.
SAVE_DIR="${OPENTTD_PERSONAL_DIR:-${HOME}/Documents/OpenTTD}/save"
STAMP="resave_tmp_$$"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}" "${SAVE_DIR}/${STAMP}.sav"' EXIT
mkdir -p "${WORK_DIR}/scripts"
printf 'save %s\nexit\n' "${STAMP}" > "${WORK_DIR}/scripts/game_start.scr"

for save in "$@"; do
	[[ -f "${save}" ]] || { echo "error: no such savegame: ${save}" >&2; exit 1; }
	abs="$(cd "$(dirname "${save}")" && pwd)/$(basename "${save}")"
	before="$("${OPENTTD}" -q "${abs}" | awk '/Savegame ver:/ {print $3}')"

	rm -f "${SAVE_DIR}/${STAMP}.sav"
	# -x: never write the config file. -s/-m/-v null: fully headless.
	# ticks=5 is only a ceiling; game_start.scr exits during the first GameLoop().
	(cd "${WORK_DIR}" && "${OPENTTD}" -x -g "${abs}" -s null -m null -v null:ticks=5) >"${WORK_DIR}/openttd.log" 2>&1

	if [[ ! -f "${SAVE_DIR}/${STAMP}.sav" ]]; then
		echo "error: re-save produced no file for ${save}; see ${WORK_DIR}/openttd.log" >&2
		cat "${WORK_DIR}/openttd.log" >&2
		exit 1
	fi

	mv "${SAVE_DIR}/${STAMP}.sav" "${abs}"
	after="$("${OPENTTD}" -q "${abs}" | awk '/Savegame ver:/ {print $3}')"
	echo "${save}: savegame version ${before} -> ${after}"
done
