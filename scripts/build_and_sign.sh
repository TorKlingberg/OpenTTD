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

codesign -s - --deep --force openttd
codesign -s - --deep --force openttd_test
