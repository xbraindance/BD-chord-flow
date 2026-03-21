#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

"$ROOT_DIR/tests/test_chordflow_pad_switch.sh"
"$ROOT_DIR/tests/test_chordflow_save_behavior.sh"
"$ROOT_DIR/tests/test_chordflow_preset_paths.sh"
"$ROOT_DIR/tests/test_chordflow_multi_json_load.sh"
"$ROOT_DIR/tests/test_chordflow_state_restore.sh"
"$ROOT_DIR/tests/test_chordflow_octave_transpose.sh"
"$ROOT_DIR/tests/test_chordflow_note_order_boundary.sh"
"$ROOT_DIR/tests/test_chordflow_overlap_release.sh"
"$ROOT_DIR/tests/test_factory_preset_quality.sh"
"$ROOT_DIR/tests/chordflow_ui_behavior_test.sh"
"$ROOT_DIR/tests/test_release_metadata.sh"

echo "All Chord Flow tests passed."
