#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$repo_root/build"

if [[ ! -d "$build_dir" ]]; then
  echo "Build directory not found: $build_dir" >&2
  exit 1
fi

cores="$(sysctl -n hw.ncpu 2>/dev/null || echo 8)"

cd "$build_dir"
make -j"$cores"

# Sign with get-task-allow so `lldb -p <pid>` can pause a running game. An ad-hoc
# signature without it lets LLDB attach but not stop the process, and that failed
# attach kills the game.
entitlements="$repo_root/scripts/debug.entitlements"

codesign -s - --deep --force --entitlements "$entitlements" openttd
codesign -s - --deep --force --entitlements "$entitlements" openttd_test
