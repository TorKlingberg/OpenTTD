#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

exec lldb -s scripts/lldb_lockup.lldb -- ./build-debug/openttd "$@"
