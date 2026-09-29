#!/usr/bin/env bash
#
# Regenerates the nanopb C sources from protobufs/meshpigeon/*.proto.
#
# The generated output is COMMITTED (lib/meshpigeon-core/src/generated/), so
# PlatformIO builds never need nanopb installed (the runtime itself is vendored
# in lib/meshpigeon-core/src/nanopb/). CI re-runs this script and fails if the
# checkout is not up to date, and `buf lint` / `buf breaking` guard schema
# quality and the additive-only evolution policy.
#
# Usage:
#   NANOPB_DIR=/path/to/nanopb-0.4.9 ./scripts/regen-protos.sh            # regen
#   NANOPB_DIR=/path/to/nanopb-0.4.9 ./scripts/regen-protos.sh --check    # CI gate
#
# NANOPB_DIR must contain the nanopb generator (generator-bin/ or the python
# generator); see https://jpa.kapsi.fi/nanopb/download/. protoc must be on
# PATH (or inside NANOPB_DIR/generator-bin). CI fetches the same tarball to
# $HOME/nanopb and runs --check, so a stale checkout fails the build.

set -euo pipefail

cd "$(dirname "$0")/.."

: "${NANOPB_DIR:?set NANOPB_DIR to your nanopb checkout (e.g. nanopb-0.4.9)}"

OUT="$(pwd)/lib/meshpigeon-core/src/generated"
COMMITTED="$OUT"

CHECK=0
[ "${1:-}" = "--check" ] && CHECK=1

if [ -x "$NANOPB_DIR/generator-bin/protoc" ]; then
  PROTOC="$NANOPB_DIR/generator-bin/protoc"
else
  PROTOC=protoc
fi

scratch=""
# The `return 0` matters: a trap whose last command fails (an empty $scratch
# makes the test false) overwrites the script's exit status, so a successful
# regen would look like a failed one.
cleanup() { [ -n "$scratch" ] && rm -rf "$scratch"; return 0; }
trap cleanup EXIT
if [ "$CHECK" = 1 ]; then
  scratch=$(mktemp -d)
  OUT="$scratch/generated"
fi

mkdir -p "$OUT"

# nanopb needs the .options file in the current directory; -I roots the
# imports at protobufs/ so imports read "meshpigeon/envelope.proto". OUT is
# absolute and pre-created: the generator must be handed a real, existing
# directory.
cd protobufs
"$PROTOC" --nanopb_out="-S.cpp -v:$OUT" -I=. meshpigeon/envelope.proto \
  meshpigeon/device.proto meshpigeon/radio.proto
cd ..

if [ "$CHECK" = 1 ]; then
  if diff -r "$OUT" "$COMMITTED" >/dev/null 2>&1; then
    echo "regen-protos: generated sources are current"
  else
    echo "regen-protos: generated sources are STALE — run scripts/regen-protos.sh and commit" >&2
    diff -rq "$OUT" "$COMMITTED" | head >&2
    exit 1
  fi
else
  echo "regenerated $OUT — commit the result together with any .proto change"
fi
