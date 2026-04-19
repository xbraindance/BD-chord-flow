# BD Chord Flow (Schwung FKA Move Everything)

Chord Flow is a chainable MIDI FX module for Schwung that lets you trigger chords with pads for better chord progressions. Browse banks of chord presets, play pads to trigger chords, and edit/save full pad sets. 

! This module is used with an empty Drum kit template on Ableton Move. Otherwise the chords won't trigger.

## New features
- Add custom preset chords to midi/ folder by uploading a midi file with a chord which will appear at the end of the listed chord types as C01, C02, etc.
- Now with 256 presets
- Scrolls long preset names in preset browser

## Quick start

1. Make sure Native Move sends midi to Schwung by pressing Shift + track button for the track and set midi out channel. e.g. Track button 4 > Midi channel 4.

2. Change that track layout to an empty/template drum track in Move native mode. It needs to be in drum pad mode to trigger the 16 midi chords in Chord Flow.

3. Then shift + Vol + the track button that you just set the midi channel for in step 1 to enter Schwung signal chain view.

4. Press the second slot aka “Synth” and select a synth you want to chord trigger.

5. In Schwung signal chain view, press the first slot aka Midi FX and select Chord Flow.

6. Choose a genre preset and tap one of 16 “drum” pads to trigger a custom chord. All pads can be customised and saved as a new preset.
   

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

## Release

Releases are tag-driven via GitHub Actions (`.github/workflows/release.yml`).

```bash
git tag -a v0.6.1 -m "Chord Flow v0.6.1"
git push origin v0.6.1
```

Before tagging, keep these in sync:

- `src/module.json` `version`
- `release.json` `version`
- `release.json` `download_url`

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
