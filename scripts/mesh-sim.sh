#!/bin/sh
# Boot an N-radio mesh of desktop sims for the app's mesh-scale tests
# (09-testing §2). Each sim is one dumb radio on its own TCP port; the app's
# MeshSimHarness bridges them with the RF loss model.
#
#   scripts/mesh-sim.sh [N] [base-port]    # default 3 radios on 8801..8803
#
# Build first:  pio run -e sim
# Detached use: setsid nohup scripts/mesh-sim.sh 3 8801 >/dev/null 2>&1 &
# (a sim started bare from a shell dies when that shell exits)
set -e
N="${1:-3}"
BASE="${2:-8801}"
SIM="$(dirname "$0")/../.pio/build/sim/program"
[ -x "$SIM" ] || { echo "sim not built — run: pio run -e sim" >&2; exit 1; }
i=0
while [ "$i" -lt "$N" ]; do
  setsid nohup "$SIM" --port $((BASE + i)) >/dev/null 2>&1 &
  i=$((i + 1))
done
echo "mesh sims: ports $BASE..$((BASE + N - 1))"
