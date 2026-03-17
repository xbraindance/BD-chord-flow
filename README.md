# Chord Flow (Move Everything)

Chord Flow is a chainable MIDI FX module for Move Everything that lets you browse banks of chord presets, play pads, and edit/save full pad sets.

## Current behavior

- Starts in a preset browser with bank shown in the title bar (`CF: <bank>`).
- Click (jog press) loads the selected preset and enters edit mode.
- Jog in browser scrolls continuously across all banks.
- Pad presses select the active pad and refresh the edit values.
- `Global Oct` (default `+2`) transposes all pads in the current preset.
- `Global Trans` adds semitone transpose (`-12..+12`) for the whole preset.
- `Pad Oct` adds per-pad octave transpose on top of `Global Oct`.
- `Bass` supports slash bass (`none` or note).
- `Bank` parameter in edit view jumps directly to the first preset of a bank.
- Save row writes a new preset snapshot to the `User` bank.

## Repository layout

```text
docs/
  PRESETS.md
src/
  module.json
  help.json
  ui.js
  presets/
    default.json
  dsp/
    chord_flow_plugin.c
scripts/
  build.sh
  build-module.sh
  install.sh
  test.sh
tests/
  test_chordflow_pad_switch.c
  test_chordflow_pad_switch.sh
  test_chordflow_save_behavior.c
  test_chordflow_save_behavior.sh
  chordflow_ui_behavior_test.sh
```

This layout matches the same structure used by modules like `superarp` and `eucalypso`.

## Preset banks

At runtime, banks are loaded in this order:

1. `presets/default.json` -> `Factory`
2. Any additional `presets/*.json` (except `default.json` and `user.json`) -> one bank per file
3. `presets/user.json` -> `User`

Save always targets `presets/user.json` (`User` bank).

Local-only/private banks can be kept under `presets/private/` (gitignored) and copied manually as needed.

## Preset format

See full format docs here:

- [docs/PRESETS.md](/Users/dominiklange/Documents/GitHub/move-everything-chordflow/docs/PRESETS.md)

## Build

```bash
./scripts/build.sh
```

Output:

- `dist/chord-flow/`
- `dist/chord-flow-module.tar.gz`

## Test

```bash
./scripts/test.sh
```

## Install to Move

```bash
./scripts/install.sh
```

The module is installed to:

`/data/UserData/move-anything/modules/midi_fx/chord-flow/`

Preset files used at runtime are under:

`/data/UserData/move-anything/modules/midi_fx/chord-flow/presets/`

- `default.json` (shipped defaults)
- `*.json` (additional banks, one bank per file)
- `user.json` (saved user presets)

Custom chord bank `.json` files can be added either with Move Everything
Installer (`Manage Presets`) or by copying files via SFTP into this `presets/`
folder.

## References

- Move Everything module docs: [MODULES.md](https://github.com/handcraftedcc/move-everything/blob/main/docs/MODULES.md)
