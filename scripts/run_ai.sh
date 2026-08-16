#!/usr/bin/env bash
#
# Run ModularAirportAI headless on a fresh map and report what it built.
#
# Usage: scripts/run_ai.sh [years] [seed] [extra openttd args...]
#
# A savegame records which AI runs in it, so testing a *new* AI means starting a
# new game rather than loading a fixture. Two settings make that work headless:
# competitors_interval = 0 (otherwise the AI silently never starts) and
# max_no_competitors >= 1.
#
# AILog output is at debug level script=4, not 2. The [AirportStats] lines that
# say whether the airports actually move aircraft are at misc=1.

set -euo pipefail

YEARS="${1:-5}"
SEED="${2:-12345}"
shift 2 2>/dev/null || shift $# 2>/dev/null || true

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AI_SRC="${ROOT}/ai/ModularAirportAI"
AI_DST="${ROOT}/build/ai/ModularAirportAI"
LOG="${MODULAR_AI_LOG:-/tmp/openttd_ai.log}"
CFG="${ROOT}/build/modular_ai.cfg"

DAY_TICKS=74
TOTAL_TICKS=$(( (YEARS * 365 + 62 + 7) * DAY_TICKS ))

START_YEAR="${MODULAR_AI_START_YEAR:-1970}"
SELFTEST="${MODULAR_AI_SELFTEST:-0}"
MAX_AIRPORTS="${MODULAR_AI_MAX_AIRPORTS:-12}"
VARIETY="${MODULAR_AI_VARIETY:-2}"
MAP_X="${MODULAR_AI_MAP_X:-9}"
MAP_Y="${MODULAR_AI_MAP_Y:-9}"

rm -rf "${AI_DST}"
mkdir -p "${AI_DST}"
cp "${AI_SRC}"/*.nut "${AI_DST}/"

cat > "${CFG}" <<EOF
[misc]
language = english.lng

[gui]
autosave = off

[difficulty]
max_no_competitors = 1
competitors_interval = 0

[game_creation]
town_name = english
starting_year = ${START_YEAR}
map_x = ${MAP_X}
map_y = ${MAP_Y}

[vehicle]
plane_speed = 2

[ai_players]
modularairportai = selftest=${SELFTEST},max_airports=${MAX_AIRPORTS},variety=${VARIETY}
EOF

cd "${ROOT}/build"
./openttd -x -c "${CFG}" -G "${SEED}" -snull -mnull -vnull:ticks="${TOTAL_TICKS}" \
	-d script=4,misc=1 "$@" -g > "${LOG}" 2>&1 || true

echo "log: ${LOG}"
grep -oE "\[script:[0-9]\] \[[0-9]\] \[[A-Z]\] .*" "${LOG}" | sed -E 's/^\[script:[0-9]\] \[[0-9]\] \[[A-Z]\] //' || true
echo "--- throughput ---"
grep -oE "\[AirportStats\].*" "${LOG}" || echo "(no airport activity)"
