#!/usr/bin/env bash
set -euo pipefail

scripts/build_and_sign.sh && ./build/openttd -d misc=1 -x -g /Users/tor/Documents/OpenTTD/save/mass6-inair.sav -s null -m null -v null:ticks=32000 > /tmp/openttd.log 2>&1 && rg "\[AirportStats\] Year 2017 totals" /tmp/openttd.log
