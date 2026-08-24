#!/usr/bin/env bash
# Run a local dedicated server plus two headless spectator clients and fail if
# either client desynchronises. The second client joins after the server has
# simulated for a while, so the test covers both ordinary deterministic play
# and the save/load boundary used to transfer a running game to a new client.
#
# The default T5j2 fixture keeps many aircraft moving through contended modular
# airports. OpenTTD's normal RNG sync check is requested every frame for this
# test instead of every 100 frames, which makes failures surface promptly.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT_DIR}"

OPENTTD="${OPENTTD_BIN:-${ROOT_DIR}/build/openttd}"
SAVE_FILE="${ROOT_DIR}/scripts/testdata/T5j2.sav"
WARMUP_DAYS=60
SOAK_DAYS=30
PORT=$((20000 + $$ % 30000))
RUN_BUILD=1
KEEP_ARTIFACTS=0
ARTIFACT_DIR=""
ACCELERATE=0

usage() {
	cat <<'USAGE'
Usage: scripts/multiplayer_desync_test.sh [options]

  --save FILE       Savegame to exercise (default: scripts/testdata/T5j2.sav)
  --warmup-days N   Days between the early and late joins (default: 60)
  --soak-days N     Days to run after the late join (default: 30)
  --port N          Loopback TCP port (default: derived from this process ID)
  --no-build        Reuse the existing ./build/openttd binary
  --accelerate      Fast-forward the warmup via LLDB (macOS/developer use)
  --artifact-dir D  Put configs and logs in D instead of a temporary directory
  --keep-artifacts  Keep a temporary artifact directory even when the test passes
  -h, --help        Show this help

The test starts one dedicated server and two null-video spectator clients. It
fails unless both clients actually join and remain connected, or if any client
records sync_err / the server reports a desync. Artifacts are always retained
on failure and removed after a pass unless --keep-artifacts was supplied.

At normal multiplayer speed, one game day takes about two seconds. The default
run therefore takes roughly three minutes plus startup/build time. On macOS,
--accelerate temporarily changes only the dedicated server's runtime game speed
through LLDB, then restores normal speed before the late client joins.
USAGE
}

require_uint() {
	local name="$1"
	local value="$2"
	if ! [[ "${value}" =~ ^[0-9]+$ ]]; then
		echo "error: ${name} must be a non-negative integer" >&2
		exit 2
	fi
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--save)
			shift
			[[ $# -gt 0 ]] || { echo "error: --save needs a file" >&2; exit 2; }
			SAVE_FILE="$1"
			;;
		--warmup-days)
			shift
			[[ $# -gt 0 ]] || { echo "error: --warmup-days needs a value" >&2; exit 2; }
			WARMUP_DAYS="$1"
			;;
		--soak-days)
			shift
			[[ $# -gt 0 ]] || { echo "error: --soak-days needs a value" >&2; exit 2; }
			SOAK_DAYS="$1"
			;;
		--port)
			shift
			[[ $# -gt 0 ]] || { echo "error: --port needs a value" >&2; exit 2; }
			PORT="$1"
			;;
		--artifact-dir)
			shift
			[[ $# -gt 0 ]] || { echo "error: --artifact-dir needs a directory" >&2; exit 2; }
			ARTIFACT_DIR="$1"
			KEEP_ARTIFACTS=1
			;;
		--no-build) RUN_BUILD=0 ;;
		--accelerate) ACCELERATE=1 ;;
		--keep-artifacts) KEEP_ARTIFACTS=1 ;;
		-h|--help) usage; exit 0 ;;
		*) echo "error: unknown argument '$1'" >&2; usage >&2; exit 2 ;;
	esac
	shift
done

require_uint "warmup days" "${WARMUP_DAYS}"
require_uint "soak days" "${SOAK_DAYS}"
require_uint "port" "${PORT}"
if [[ "${PORT}" -lt 1 || "${PORT}" -gt 65535 ]]; then
	echo "error: port must be between 1 and 65535" >&2
	exit 2
fi

if [[ ! -f "${SAVE_FILE}" ]]; then
	echo "error: savegame not found: ${SAVE_FILE}" >&2
	exit 2
fi
SAVE_FILE="$(cd "$(dirname "${SAVE_FILE}")" && pwd)/$(basename "${SAVE_FILE}")"

if [[ "${RUN_BUILD}" -eq 1 ]]; then
	"${ROOT_DIR}/scripts/build_and_sign.sh"
fi
if [[ ! -x "${OPENTTD}" ]]; then
	echo "error: OpenTTD binary not found or not executable: ${OPENTTD}" >&2
	exit 2
fi
if [[ "${ACCELERATE}" -eq 1 ]] && ! command -v lldb >/dev/null 2>&1; then
	echo "error: --accelerate requires lldb" >&2
	exit 2
fi

if [[ -n "${ARTIFACT_DIR}" ]]; then
	mkdir -p "${ARTIFACT_DIR}"
	RUN_DIR="$(cd "${ARTIFACT_DIR}" && pwd)"
	if [[ -n "$(find "${RUN_DIR}" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
		echo "error: artifact directory must be empty: ${RUN_DIR}" >&2
		exit 2
	fi
else
	RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/openttd_multiplayer_desync.XXXXXX")"
fi

SERVER_DIR="${RUN_DIR}/server"
EARLY_DIR="${RUN_DIR}/early-client"
LATE_DIR="${RUN_DIR}/late-client"
SERVER_LOG="${RUN_DIR}/server.log"
EARLY_LOG="${RUN_DIR}/early-client.log"
LATE_LOG="${RUN_DIR}/late-client.log"
SERVER_DESYNC_LOG="${SERVER_DIR}/save/autosave/commands-out.log"
EARLY_DESYNC_LOG="${EARLY_DIR}/save/autosave/commands-out.log"
LATE_DESYNC_LOG="${LATE_DIR}/save/autosave/commands-out.log"

mkdir -p "${SERVER_DIR}/scripts" "${EARLY_DIR}" "${LATE_DIR}"

# A -c path makes its containing directory the instance's personal directory.
# This keeps commands-out.log, emergency saves, secrets, and configuration from
# the three processes separate. A non-empty client_name is mandatory: without
# it NetworkClientConnectGame rejects the join and a null-video process merely
# simulates the title game forever, which otherwise looks like a passing soak.
printf '%s\n' \
	'[network]' \
	'client_name = MP Desync Server' \
	'server_name = Modular Airport Desync Test' \
	'server_game_type = local' \
	> "${SERVER_DIR}/openttd.cfg"

printf '%s\n' \
	'[network]' \
	'client_name = MP Desync Early' \
	> "${EARLY_DIR}/openttd.cfg"

printf '%s\n' \
	'[network]' \
	'client_name = MP Desync Late' \
	> "${LATE_DIR}/openttd.cfg"

# game_start.scr executes immediately after the fixture is loaded. sync_freq=1
# asks the server to send an RNG sync check every frame without requiring a
# special RANDOM_DEBUG build. Keeping the game running while a client downloads
# is intentional: it exercises the normal catch-up path as well as save/load.
printf '%s\n' \
	'setting network.sync_freq 1' \
	'setting network.pause_on_join false' \
	'echo MP_DESYNC_SERVER_READY' \
	> "${SERVER_DIR}/scripts/game_start.scr"

server_pid=""
early_pid=""
late_pid=""
test_passed=0

stop_process() {
	local pid="$1"
	[[ -n "${pid}" ]] || return 0
	if kill -0 "${pid}" 2>/dev/null; then
		kill -TERM "${pid}" 2>/dev/null || true
	fi
}

cleanup() {
	local status=$?
	trap - EXIT INT TERM

	# Stop clients first. The verdict is computed before cleanup, so their normal
	# disconnect messages cannot be mistaken for test failures.
	stop_process "${early_pid}"
	stop_process "${late_pid}"
	stop_process "${server_pid}"
	wait "${early_pid}" 2>/dev/null || true
	wait "${late_pid}" 2>/dev/null || true
	wait "${server_pid}" 2>/dev/null || true

	if [[ "${test_passed}" -eq 1 && "${KEEP_ARTIFACTS}" -eq 0 ]]; then
		rm -rf "${RUN_DIR}"
	else
		echo "Artifacts: ${RUN_DIR}"
	fi

	exit "${status}"
}
trap cleanup EXIT INT TERM

process_alive() {
	local pid="$1"
	[[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null
}

desync_detected() {
	if [[ -f "${EARLY_DESYNC_LOG}" ]] && rg -q 'sync_err:' "${EARLY_DESYNC_LOG}"; then
		echo "FAIL: early client recorded sync_err" >&2
		return 0
	fi
	if [[ -f "${LATE_DESYNC_LOG}" ]] && rg -q 'sync_err:' "${LATE_DESYNC_LOG}"; then
		echo "FAIL: late client recorded sync_err" >&2
		return 0
	fi
	if [[ -f "${SERVER_DESYNC_LOG}" ]] && rg -q 'desync error' "${SERVER_DESYNC_LOG}"; then
		echo "FAIL: server recorded a client leaving with a desync error" >&2
		return 0
	fi
	return 1
}

wait_for_pattern() {
	local file="$1"
	local pattern="$2"
	local description="$3"
	local watched_pid="$4"
	local timeout_seconds="$5"
	local deadline=$((SECONDS + timeout_seconds))

	while [[ "${SECONDS}" -lt "${deadline}" ]]; do
		if [[ -f "${file}" ]] && rg -q "${pattern}" "${file}"; then
			return 0
		fi
		if [[ -n "${watched_pid}" ]] && ! process_alive "${watched_pid}"; then
			echo "FAIL: process exited while waiting for ${description}" >&2
			return 1
		fi
		if desync_detected; then
			return 1
		fi
		sleep 0.1
	done

	echo "FAIL: timed out waiting for ${description}" >&2
	return 1
}

sync_day_count() {
	local count=""
	if [[ -f "${SERVER_DESYNC_LOG}" ]]; then
		count="$(rg -c ' sync: ' "${SERVER_DESYNC_LOG}" 2>/dev/null || true)"
	fi
	echo "${count:-0}"
}

wait_for_sync_days() {
	local start_count="$1"
	local days="$2"
	local description="$3"
	local target=$((start_count + days))
	local timeout_seconds=$((days * 4 + 30))
	local deadline=$((SECONDS + timeout_seconds))
	local current

	while [[ "${SECONDS}" -lt "${deadline}" ]]; do
		if desync_detected; then
			return 1
		fi
		if ! process_alive "${server_pid}" || ! process_alive "${early_pid}"; then
			echo "FAIL: server or early client exited during ${description}" >&2
			return 1
		fi
		if [[ -n "${late_pid}" ]] && ! process_alive "${late_pid}"; then
			echo "FAIL: late client exited during ${description}" >&2
			return 1
		fi

		current="$(sync_day_count)"
		if [[ "${current}" -ge "${target}" ]]; then
			return 0
		fi
		sleep 0.2
	done

	echo "FAIL: timed out during ${description} (${days} game days requested)" >&2
	return 1
}

print_failure_context() {
	echo >&2
	echo "Failure context:" >&2
	for file in "${EARLY_DESYNC_LOG}" "${LATE_DESYNC_LOG}" "${SERVER_DESYNC_LOG}"; do
		[[ -f "${file}" ]] || continue
		echo "--- ${file}" >&2
		tail -n 12 "${file}" >&2
	done
	echo "Full process logs:" >&2
	echo "  ${SERVER_LOG}" >&2
	echo "  ${EARLY_LOG}" >&2
	echo "  ${LATE_LOG}" >&2
}

fail_test() {
	echo "FAIL: modular-airport multiplayer desync test" >&2
	print_failure_context
	exit 1
}

set_server_speed() {
	local speed="$1"
	local description="$2"
	local lldb_log="${RUN_DIR}/lldb.log"

	if ! lldb -p "${server_pid}" --batch \
		-o "expr -- _game_speed = ${speed}" \
		-o 'detach' >> "${lldb_log}" 2>&1; then
		echo "FAIL: could not ${description}; see ${lldb_log}" >&2
		return 1
	fi
}

start_client() {
	local client_dir="$1"
	local client_log="$2"
	"${OPENTTD}" \
		-X -x \
		-c "${client_dir}/openttd.cfg" \
		-n "127.0.0.1:${PORT}#255" \
		-v null:ticks=2000000000 -s null -m null \
		-d net=3,desync=2,misc=2 \
		> "${client_log}" 2>&1 &
	started_client_pid=$!
}

echo "Fixture:   ${SAVE_FILE}"
echo "Binary:    ${OPENTTD}"
echo "Port:      ${PORT}"
echo "Artifacts: ${RUN_DIR}"
echo "Starting dedicated server..."

"${OPENTTD}" \
	-X -x \
	-c "${SERVER_DIR}/openttd.cfg" \
	-D "127.0.0.1:${PORT}" \
	-g "${SAVE_FILE}" \
	-d net=3,desync=2,misc=2 \
	< /dev/null > "${SERVER_LOG}" 2>&1 &
server_pid=$!

wait_for_pattern "${SERVER_LOG}" 'MP_DESYNC_SERVER_READY' \
	"the server fixture to load" "${server_pid}" 60 || fail_test
wait_for_pattern "${SERVER_LOG}" "Listening on 127\\.0\\.0\\.1:${PORT}" \
	"the loopback listener" "${server_pid}" 15 || fail_test

echo "Starting early spectator client..."
started_client_pid=""
start_client "${EARLY_DIR}" "${EARLY_LOG}"
early_pid="${started_client_pid}"
wait_for_pattern "${EARLY_DESYNC_LOG}" 'MP Desync Early has joined the game' \
	"the early client to join" "${early_pid}" 60 || fail_test
early_join_day="$(sync_day_count)"
echo "Early client joined; warming up for ${WARMUP_DAYS} game days..."
if [[ "${ACCELERATE}" -eq 1 ]]; then
	echo "Accelerating the dedicated server for the warmup..."
	set_server_speed 0 "accelerate the warmup" || fail_test
fi
wait_for_sync_days "${early_join_day}" "${WARMUP_DAYS}" \
	"the pre-late-join warmup" || fail_test
if [[ "${ACCELERATE}" -eq 1 ]]; then
	set_server_speed 100 "restore normal multiplayer speed" || fail_test
fi

echo "Starting late spectator client..."
start_client "${LATE_DIR}" "${LATE_LOG}"
late_pid="${started_client_pid}"
wait_for_pattern "${LATE_DESYNC_LOG}" 'MP Desync Late has joined the game' \
	"the late client to join" "${late_pid}" 60 || fail_test
late_join_day="$(sync_day_count)"
echo "Both clients joined; soaking for ${SOAK_DAYS} game days..."
wait_for_sync_days "${late_join_day}" "${SOAK_DAYS}" \
	"the two-client soak" || fail_test

if desync_detected; then
	fail_test
fi
if [[ -f "${SERVER_DESYNC_LOG}" ]] && rg -q 'has left the game' "${SERVER_DESYNC_LOG}"; then
	echo "FAIL: a client disconnected before the soak completed" >&2
	fail_test
fi

test_passed=1
echo "PASS: both spectator clients stayed synchronised (${WARMUP_DAYS} warmup + ${SOAK_DAYS} soak days)"
