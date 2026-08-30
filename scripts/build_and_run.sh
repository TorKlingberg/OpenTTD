#!/usr/bin/env bash

set -euo pipefail

SAVE_FILE="${1:-}"

scripts/build_and_sign.sh

# Self-contained symbols, so this game stays inspectable under `lldb -p` even
# after the build directory has moved on. See scripts/make_dsym.sh.
scripts/make_dsym.sh

CMD=(./build/openttd -d misc=3)
if [[ -n "$SAVE_FILE" ]]; then
  CMD+=(-g "$SAVE_FILE")
fi

"${CMD[@]}" > /tmp/openttd.log 2>&1
