#!/usr/bin/env bash

set -euo pipefail

SAVE_FILE="${1:-}"

scripts/build_and_sign.sh

# Modular airport diagnostics use the misc channel; level 3 enables detailed [ModAp] traces.
CMD=(./build/openttd -d misc=3)
if [[ -n "$SAVE_FILE" ]]; then
  CMD+=(-g "$SAVE_FILE")
fi

echo "Running: ${CMD[*]}"
echo "Log: /tmp/openttd.log"
"${CMD[@]}" > /tmp/openttd.log 2>&1
