#!/usr/bin/env bash
# MeshHop hardware bench (09-testing §3).
# Rig: two XIAO WIO + one nRF52 board on a powered USB hub.
# Runs: flash all boards → smoke self-test → app-driven scenario via the
# desktop simulator bridge; writes a JSON report to reports/.
set -euo pipefail

cd "$(dirname "$0")/.."
REPORT_DIR=reports
mkdir -p "$REPORT_DIR"
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
REPORT="$REPORT_DIR/bench-$STAMP.json"

echo "== MeshHop bench $STAMP =="

# 1. Flash each connected board (device paths resolved per rig; see rig.cfg)
#    pio run -e xiao_wio -t upload --upload-port /dev/ttyACM0
# 2. Smoke self-test per board (PASS/FAIL over USB)
# 3. App-driven scenario through the simulator bridge:
#    .pio/build/sim/program --port 8765 &
#    scripts/scenario_ping.py tcp:127.0.0.1:8765   # PING round-trip
#    scripts/scenario_history.py                   # FETCH_PACKETS replay equality
# 4. Soak: 72h listening + store replay equality check (nightly job, not PR)

cat > "$REPORT" <<JSON
{ "run": "$STAMP", "status": "skeleton", "note": "wire up rig.cfg device paths; see docs/plans/09-testing.md" }
JSON
echo "wrote $REPORT"
