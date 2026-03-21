#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT_DIR/build/tests/test_chordflow_state_restore"
mkdir -p "$(dirname "$BIN")"

cc -std=c99 -Wall -Wextra -O2 \
  -I"$ROOT_DIR" \
  -I"$ROOT_DIR/../move-anything/src" \
  "$ROOT_DIR/tests/test_chordflow_state_restore.c" \
  "$ROOT_DIR/src/dsp/chord_flow_plugin.c" \
  -o "$BIN"

"$BIN"
