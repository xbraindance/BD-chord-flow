# Chord Flow Preset Format

Chord Flow reads presets from JSON arrays of preset objects.

Default load order:

1. `presets/default.json` -> `Factory` bank
2. Any additional `presets/*.json` (except `default.json` and `user.json`) -> one bank per file
3. `presets/user.json` -> `User` bank

Save behavior:

- Saving from UI always writes to the `User` bank.
- Saved data is written to `presets/user.json`.

## Adding Custom Banks

Custom chord bank `.json` files can be added either with Move Everything
Installer (`Manage Presets`) or by copying files via SFTP into the module
`presets/` folder.

## Top-level schema

Each file is a JSON array:

```json
[
  {
    "name": "Preset Name",
    "bank": "Factory",
    "global_octave": 2,
    "global_transpose": 0,
    "pads": [ ... ]
  }
]
```

## Preset object fields

- `name` (string): Display name in browser.
- `bank` (string, optional in file):
  - If present, uses that bank name.
  - If omitted, bank is inferred by source file (`Factory`/`User`).
- `global_octave` (int): `-6..6`, default baseline is `2`.
- `global_transpose` (int): `-12..12`.
- `pads` (array): Up to 32 pad entries.
  - If fewer than 32 are provided, remaining pads are auto-filled with defaults.

## Pad object fields

```json
{
  "octave": 0,
  "root": "c",
  "bass": "none",
  "chord_type": "maj7",
  "inversion": 0,
  "strum": 0,
  "strum_dir": 0,
  "articulation": 0,
  "reverse_art": 0
}
```

- `octave` (int): `-6..6`
- `root` (enum string): `c c# d d# e f f# g g# a a# b`
- `bass` (enum string): `none c c# d d# e f f# g g# a a# b`
- `chord_type` (enum string): any supported type from DSP/UI chord list
- `inversion` (int): `0..3` (`root`, `1st`, `2nd`, `3rd`)
- `strum` (int): `0..100`
- `strum_dir` (int): `0=up`, `1=down`
- `articulation` (int): `0=off`, `1=on`
- `reverse_art` (int): `0=off`, `1=on`

## Supported `chord_type` values

```text
maj, min, 5th, sus2, sus4, add2, add9, add11,
6th, min6, maj7, min7, dom7, maj9, min9, dom9,
dim, dim7, m7b5, aug, aug7, sus7, 7sus2, 7sus4,
9sus, sus9, 11th, m11, sus11, 13th, maj13, min13,
dom7add9, dom7b9, dom7#9, dom7#5, dom7alt, maj7#5, maj7b5, maj7#11, maj9#11,
madd9, madd11, mb5, mb6, m7b13, m7add13, m11b5, mM13, sus13, add13,
11sus, 11sus2, 13b9, 5add9, 5b9, 6sus2, 6sus2b5, 6sus4, 7#9#5, 9#11,
dimM7, dim#5, dim11, addb9, aug#9, mb7, mb9, mb13, m6addb13,
no3, no5, maj7add6, maj7sus2, maj9no3, maj6
```

## Notes

- Roots/bass are stored as lowercase sharp-note tokens.
- Flats should be normalized before writing (for example `Bb -> a#`, `Db -> c#`).
- Chord Flow preserves pad data for all 32 pads internally.
- Additional bank files are discovered automatically from `presets/*.json`.
- Filename (without extension) is used as inferred bank name when `bank` is omitted.
- `presets/private/` is ignored by git and can be used for local-only banks.
