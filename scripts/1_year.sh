#!/usr/bin/env bash
set -euo pipefail

# Own log path, not /tmp/openttd.log — see the comment in n_years_plus2.sh.
LOG_FILE="${OPENTTD_REGRESSION_LOG:-/tmp/openttd_regression_mass7-inair.log}"

scripts/build_and_sign.sh && ./build/openttd -d misc=1 -x -g scripts/testdata/mass7-inair.sav -s null -m null -v null:ticks=32000 > "${LOG_FILE}" 2>&1 && rg "\[AirportStats\] Year [0-9]+ totals" "${LOG_FILE}"
