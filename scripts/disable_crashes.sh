#!/usr/bin/env bash
# Turn off aircraft crashes in a savegame, so the fixture measures only the code
# under test.
#
# Crashes are the remaining noise source in the regression fixtures: any change
# that shifts timing consumes the synced Random() differently, so a different set
# of aircraft dies, and an airport served by three aircraft loses ~22% of its
# movements when one of them goes. See "Comparing two runs" in CLAUDE.md.
#
# 'vehicle.plane_crashes' is a game setting (saved with the savegame), 0 = none.
# It gates the general per-brake-tick roll in RollAirplaneCrashCheck(). It does
# NOT gate the elevated short-strip overrun for fast jets (fixed prob 3276),
# which is gated by the no_jetcrash *cheat* instead -- that one is unreachable
# from the console, and turning it on would also change which airports jets may
# be sent to, so this script leaves it alone. A fixture that lands fast jets on
# airports without large-runway safety can therefore still crash; check
# '[AircraftLost]' in the run log rather than assuming.
#
# Uses the same scripts/game_start.scr hook as resave.sh, so the state written is
# the loaded state with the setting changed and zero ticks simulated.
#
# Usage: scripts/disable_crashes.sh <savegame> [<savegame> ...]
# Files are rewritten in place.
set -euo pipefail

OPENTTD="${OPENTTD_BIN:-/Users/tor/ttd/OpenTTD/build/openttd}"
[[ -x "${OPENTTD}" ]] || { echo "error: no openttd binary at ${OPENTTD}" >&2; exit 1; }

SAVE_DIR="${OPENTTD_PERSONAL_DIR:-${HOME}/Documents/OpenTTD}/save"
STAMP="nocrash_tmp_$$"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}" "${SAVE_DIR}/${STAMP}.sav"' EXIT
mkdir -p "${WORK_DIR}/scripts"
printf 'setting vehicle.plane_crashes 0\nsave %s\nexit\n' "${STAMP}" > "${WORK_DIR}/scripts/game_start.scr"

# Reading a setting back needs console output on stdout, which only the dedicated
# server driver gives. Read-only: never the run that writes the file.
read_setting() {
	local dir
	dir="$(mktemp -d)"
	mkdir -p "${dir}/scripts"
	printf 'setting vehicle.plane_crashes\nexit\n' > "${dir}/scripts/game_start.scr"
	(cd "${dir}" && "${OPENTTD}" -D -x -g "$1" -s null -m null) 2>&1 |
		sed -n "s/.*value for 'vehicle.plane_crashes' is '\([0-9]*\)'.*/\1/p" | tail -1
	rm -rf "${dir}"
}

for save in "$@"; do
	[[ -f "${save}" ]] || { echo "error: no such savegame: ${save}" >&2; exit 1; }
	abs="$(cd "$(dirname "${save}")" && pwd)/$(basename "${save}")"

	before="$(read_setting "${abs}")"

	rm -f "${SAVE_DIR}/${STAMP}.sav"
	(cd "${WORK_DIR}" && "${OPENTTD}" -x -g "${abs}" -s null -m null -v null:ticks=5) >"${WORK_DIR}/openttd.log" 2>&1

	if [[ ! -f "${SAVE_DIR}/${STAMP}.sav" ]]; then
		echo "error: no file written for ${save}; see ${WORK_DIR}/openttd.log" >&2
		cat "${WORK_DIR}/openttd.log" >&2
		exit 1
	fi

	mv "${SAVE_DIR}/${STAMP}.sav" "${abs}"
	after="$(read_setting "${abs}")"
	echo "${save}: vehicle.plane_crashes ${before:-?} -> ${after:-?}"
done
