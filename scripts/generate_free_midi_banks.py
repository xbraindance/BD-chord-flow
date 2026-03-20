#!/usr/bin/env python3
"""Generate Chord Flow preset banks from free-midi-chords chords.py.

Outputs one JSON file per bank in Chord Flow preset format.
"""

from __future__ import annotations

import argparse
import ast
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple

ROMAN_TO_SEMITONE = {
    "I": 0,
    "II": 2,
    "III": 4,
    "IV": 5,
    "V": 7,
    "VI": 9,
    "VII": 11,
}

ROMAN_TOKENS = ["vii", "VII", "iii", "III", "ii", "II", "iv", "IV", "vi", "VI", "v", "V", "i", "I"]

NOTE_NAMES = ["c", "c#", "d", "d#", "e", "f", "f#", "g", "g#", "a", "a#", "b"]
NOTE_TO_PC = {n: i for i, n in enumerate(NOTE_NAMES)}

MODE_INFO = {
    "ionian": {"offset": 0, "scale": {0, 2, 4, 5, 7, 9, 11}},
    "dorian": {"offset": 2, "scale": {0, 2, 3, 5, 7, 9, 10}},
    "phrygian": {"offset": 4, "scale": {0, 1, 3, 5, 7, 8, 10}},
    "lydian": {"offset": 5, "scale": {0, 2, 4, 6, 7, 9, 11}},
    "mixolydian": {"offset": 7, "scale": {0, 2, 4, 5, 7, 9, 10}},
    "aeolian": {"offset": 9, "scale": {0, 2, 3, 5, 7, 8, 10}},
    "locrian": {"offset": 11, "scale": {0, 1, 3, 5, 6, 8, 10}},
}

MODE_ORDER = ["ionian", "dorian", "phrygian", "lydian", "mixolydian", "aeolian", "locrian"]

MAJORISH_TYPES = {"maj", "maj7", "maj9", "add9", "add11", "6th", "maj6", "69", "add4", "maj7#11", "maj7b5", "maj7#5"}
MINORISH_TYPES = {"min", "min7", "min9", "madd9", "madd11", "m11", "min6", "m69", "m7add11", "mM7", "mM7add11"}

COLOR_MAP = {
    "maj": ["maj7", "add9", "maj9", "6th"],
    "maj7": ["maj9", "add9", "maj6", "maj7#11"],
    "maj9": ["add9", "maj7", "maj6"],
    "maj6": ["maj7", "add9", "maj9"],
    "min": ["min7", "madd9", "min9", "min6"],
    "min7": ["min9", "m11", "madd9", "m7add11"],
    "min9": ["m11", "min7", "madd9"],
    "min6": ["min7", "min9", "m69"],
    "dom7": ["dom9", "13th", "dom7b9", "dom7#9"],
    "dom9": ["13th", "dom7alt", "dom7#11"],
    "sus2": ["sus4", "sus9", "sus4add9"],
    "sus4": ["sus9", "7sus4", "sus4add9"],
    "5th": ["5add9", "sus2"],
    "dim": ["dim7", "m7b5"],
    "m7b5": ["dim7", "dim11"],
}


@dataclass
class ChordSpec:
    root_pc: int
    chord_type: str


def load_chords_py(path: Path) -> Dict[str, List[str]]:
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    wanted = {"prog_maj", "prog_min", "prog_modal"}
    out: Dict[str, List[str]] = {}
    for node in tree.body:
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        tgt = node.targets[0]
        if not isinstance(tgt, ast.Name):
            continue
        if tgt.id not in wanted:
            continue
        out[tgt.id] = ast.literal_eval(node.value)
    missing = wanted - set(out)
    if missing:
        raise ValueError(f"Missing progression lists in source: {sorted(missing)}")
    return out


def parse_progression_line(line: str) -> Tuple[List[str], str]:
    left, sep, right = line.partition("=")
    tokens = [tok.strip() for tok in left.strip().split() if tok.strip()]
    mood = right.strip() if sep else "Unknown"
    return tokens, mood


def parse_degree(token: str) -> Tuple[int, str, str]:
    m = re.match(r"^([b#]*)(.*)$", token)
    if not m:
        raise ValueError(f"Bad token: {token}")
    acc = m.group(1)
    rest = m.group(2)

    roman = None
    suffix = ""
    for candidate in ROMAN_TOKENS:
        if rest.startswith(candidate):
            roman = candidate
            suffix = rest[len(candidate) :]
            break
    if roman is None:
        raise ValueError(f"Cannot parse roman numeral token: {token}")

    semitone = ROMAN_TO_SEMITONE[roman.upper()]
    semitone += acc.count("#")
    semitone -= acc.count("b")
    semitone %= 12
    return semitone, roman, suffix


def chord_type_from_token(roman: str, suffix: str) -> str:
    base_dim = roman.lower() == "vii"
    base_minor = roman.islower() and not base_dim

    s = suffix.strip()
    if s == "":
        if base_dim:
            return "dim"
        return "min" if base_minor else "maj"

    case_map = {
        "M": "maj",
        "M6": "maj6",
        "M7": "maj7",
        "M9": "maj9",
        "M-5": "mb5",
        "M7+5": "maj7#5",
        "M7-5": "maj7b5",
    }
    if s in case_map:
        return case_map[s]

    low = s.lower()

    if low.startswith("dom7"):
        rem = low[4:]
        dom_map = {
            "": "dom7",
            "add9": "dom7add9",
            "b9": "dom7b9",
            "#9": "dom7#9",
            "#5": "dom7#5",
            "alt": "dom7alt",
        }
        if rem in dom_map:
            return dom_map[rem]

    if low == "7":
        if base_dim:
            return "dim7"
        if base_minor:
            return "min7"
        return "maj7"
    if low == "9":
        if base_minor:
            return "min9"
        return "maj9"
    if low == "6":
        if base_minor:
            return "min6"
        return "6th"
    if low == "69":
        if base_minor:
            return "m69"
        return "69"

    if low == "m":
        return "min"
    if low == "dim":
        return "dim"

    map_low = {
        "5": "5th",
        "2": "add9",
        "sus2": "sus2",
        "sus4": "sus4",
        "sus4add9": "sus4add9",
        "9sus4": "9sus",
        "7sus4": "7sus4",
        "add2": "add2",
        "add4": "add4",
        "add9": "add9",
        "add11": "add11",
        "11": "11th",
        "7-5": "dom7b5",
        "7+5": "dom7#5",
        "7-9": "dom7b9",
        "7+11": "dom7#11",
        "m6": "min6",
        "m7": "min7",
        "m9": "min9",
        "m11": "m11",
        "m69": "m69",
        "madd4": "madd4",
        "madd9": "madd9",
        "madd11": "madd11",
        "m7-5": "m7b5",
        "m7+5": "dom7#5",
        "m7b9b5": "m7b9b5",
        "m7add11": "m7add11",
        "mm7": "mM7",
        "mm7add11": "mM7add11",
        "dim6": "dim7",
        "dim7": "dim7",
        "aug": "aug",
        "aug7": "aug7",
        "maj7": "maj7",
        "maj9": "maj9",
    }
    if low in map_low:
        t = map_low[low]
        if t == "add9" and base_minor:
            return "madd9"
        if t == "add11" and base_minor:
            return "madd11"
        if t == "add4" and base_minor:
            return "madd4"
        return t

    raise ValueError(f"Unsupported suffix pattern: roman={roman} suffix={suffix}")


def parse_token(token: str) -> ChordSpec:
    degree_pc, roman, suffix = parse_degree(token)
    ctype = chord_type_from_token(roman, suffix)
    return ChordSpec(root_pc=degree_pc, chord_type=ctype)


def make_slot(root_pc: int, chord_type: str, inversion: int = 0, bass: str = "none", octave: int = 0) -> Dict[str, object]:
    return {
        "octave": octave,
        "root": NOTE_NAMES[root_pc % 12],
        "bass": bass,
        "chord_type": chord_type,
        "inversion": inversion,
        "strum": 0,
        "strum_dir": 0,
        "articulation": 0,
        "reverse_art": 0,
    }


def slot_sig(slot: Dict[str, object]) -> Tuple[object, ...]:
    return (slot["root"], slot["chord_type"], slot["inversion"], slot["bass"], slot["octave"])


def add_candidate(cands: List[Dict[str, object]], seen: set, slot: Dict[str, object]) -> None:
    sig = slot_sig(slot)
    if sig in seen:
        return
    seen.add(sig)
    cands.append(slot)


def build_variation_slots(core: List[Dict[str, object]], target_len: int = 16) -> List[Dict[str, object]]:
    slots = list(core)
    seen = {slot_sig(s) for s in slots}
    cands: List[Dict[str, object]] = []

    # Inversions.
    for s in core:
        for inv in (1, 2):
            add_candidate(cands, seen, {**s, "inversion": inv})

    # Color variants.
    for s in core:
        for t in COLOR_MAP.get(str(s["chord_type"]), []):
            add_candidate(cands, seen, {**s, "chord_type": t, "inversion": 0})

    # Slash bass variations for major/minor families.
    for s in core:
        if s["bass"] != "none":
            continue
        root_pc = NOTE_TO_PC[str(s["root"])]
        ctype = str(s["chord_type"])
        if ctype in MAJORISH_TYPES:
            third, fifth = (root_pc + 4) % 12, (root_pc + 7) % 12
            add_candidate(cands, seen, {**s, "bass": NOTE_NAMES[third]})
            add_candidate(cands, seen, {**s, "bass": NOTE_NAMES[fifth]})
        elif ctype in MINORISH_TYPES:
            third, fifth = (root_pc + 3) % 12, (root_pc + 7) % 12
            add_candidate(cands, seen, {**s, "bass": NOTE_NAMES[third]})
            add_candidate(cands, seen, {**s, "bass": NOTE_NAMES[fifth]})

    # Root neighbors keeping same feel.
    for s in core:
        root_pc = NOTE_TO_PC[str(s["root"])]
        add_candidate(cands, seen, {**s, "root": NOTE_NAMES[(root_pc + 5) % 12], "inversion": 0})
        add_candidate(cands, seen, {**s, "root": NOTE_NAMES[(root_pc + 7) % 12], "inversion": 0})

    for c in cands:
        if len(slots) >= target_len:
            break
        slots.append(c)

    # If still short, fallback with simple 5th shells.
    i = 0
    while len(slots) < target_len and core:
        src = core[i % len(core)]
        root_pc = NOTE_TO_PC[str(src["root"])]
        fallback = {**src, "chord_type": "5th", "inversion": 0, "bass": "none", "root": NOTE_NAMES[(root_pc + (i * 2)) % 12]}
        sig = slot_sig(fallback)
        if sig not in seen:
            seen.add(sig)
            slots.append(fallback)
        i += 1

    return slots[:target_len]


def modal_mode_scores(chords: Sequence[ChordSpec]) -> Dict[str, float]:
    if not chords:
        return {m: 0.0 for m in MODE_ORDER}
    roots = {c.root_pc for c in chords}
    first_minorish = chords[0].chord_type in MINORISH_TYPES

    out: Dict[str, float] = {}
    for mode in MODE_ORDER:
        scale = MODE_INFO[mode]["scale"]
        in_scale = sum(1 for r in roots if r in scale)
        out_scale = len(roots) - in_scale
        score = float((in_scale * 2) - (out_scale * 3))
        if first_minorish and mode in {"dorian", "phrygian", "aeolian", "locrian"}:
            score += 1.0
        if (not first_minorish) and mode in {"ionian", "lydian", "mixolydian"}:
            score += 1.0
        out[mode] = score
    return out


def short_name(index: int, mood: str) -> str:
    clean = re.sub(r"\s+", " ", mood.strip())
    name = f"{index:03d} {clean}" if clean else f"{index:03d}"
    return name[:47]


def parse_supported_types(dsp_c_path: Path) -> set:
    text = dsp_c_path.read_text(encoding="utf-8")
    m = re.search(r"static const char \*TYPE_NAMES\[\] = \{(.*?)\};", text, flags=re.S)
    if not m:
        raise ValueError("Could not parse TYPE_NAMES in DSP source")
    return set(re.findall(r'"([^"]+)"', m.group(1)))


def generate_presets(source_lists: Dict[str, List[str]]) -> Dict[str, List[Dict[str, object]]]:
    out: Dict[str, List[Dict[str, object]]] = {
        "fmc_prog_maj_c": [],
        "fmc_prog_min_a": [],
        "fmc_modal_ionian_c": [],
        "fmc_modal_dorian_d": [],
        "fmc_modal_phrygian_e": [],
        "fmc_modal_lydian_f": [],
        "fmc_modal_mixolydian_g": [],
        "fmc_modal_aeolian_a": [],
        "fmc_modal_locrian_b": [],
    }

    def convert_line(line: str, tonic_offset: int) -> Tuple[List[Dict[str, object]], str]:
        tokens, mood = parse_progression_line(line)
        parsed = [parse_token(tok) for tok in tokens]
        core = [make_slot((c.root_pc + tonic_offset) % 12, c.chord_type) for c in parsed]
        full = build_variation_slots(core, 16)
        return full, mood

    # Major bank (C)
    for i, line in enumerate(source_lists["prog_maj"], start=1):
        pads, mood = convert_line(line, tonic_offset=0)
        out["fmc_prog_maj_c"].append({
            "name": short_name(i, mood),
            "bank": "FMC Prog Maj - C",
            "global_octave": 2,
            "global_transpose": 0,
            "pads": pads,
        })

    # Minor bank (A relative to C major)
    for i, line in enumerate(source_lists["prog_min"], start=1):
        pads, mood = convert_line(line, tonic_offset=9)
        out["fmc_prog_min_a"].append({
            "name": short_name(i, mood),
            "bank": "FMC Prog Min - A",
            "global_octave": 2,
            "global_transpose": 0,
            "pads": pads,
        })

    # Modal banks, classified + balanced then offset to each mode tonic.
    modal_groups: Dict[str, List[Tuple[str, str]]] = {m: [] for m in MODE_ORDER}
    modal_counts: Dict[str, int] = {m: 0 for m in MODE_ORDER}
    for line in source_lists["prog_modal"]:
        tokens, mood = parse_progression_line(line)
        parsed = [parse_token(tok) for tok in tokens]
        scores = modal_mode_scores(parsed)
        # Balance pressure keeps all mode banks populated while staying musically close.
        mode = max(MODE_ORDER, key=lambda m: scores[m] - (modal_counts[m] * 0.75))
        modal_counts[mode] += 1
        modal_groups[mode].append((line, mood))

    for mode in MODE_ORDER:
        key = f"fmc_modal_{mode}_{NOTE_NAMES[MODE_INFO[mode]['offset']]}"
        bank_name = f"FMC Modal {mode.title()} - {NOTE_NAMES[MODE_INFO[mode]['offset']].upper()}"
        for i, (line, mood) in enumerate(modal_groups[mode], start=1):
            pads, _ = convert_line(line, tonic_offset=MODE_INFO[mode]["offset"])
            out[key].append({
                "name": short_name(i, mood),
                "bank": bank_name,
                "global_octave": 2,
                "global_transpose": 0,
                "pads": pads,
            })

    return out


def validate_outputs(outputs: Dict[str, List[Dict[str, object]]], supported_types: set) -> List[str]:
    errs: List[str] = []
    total = 0
    for bank_key, presets in outputs.items():
        total += len(presets)
        if not presets:
            # keep empty modal bank as warning-level validation failure to make assignment visible.
            errs.append(f"{bank_key}: no presets generated")
        for p in presets:
            pads = p.get("pads", [])
            if len(pads) != 16:
                errs.append(f"{bank_key}/{p.get('name')}: expected 16 pads, got {len(pads)}")
                continue
            for i, s in enumerate(pads, start=1):
                t = str(s.get("chord_type", ""))
                if t not in supported_types:
                    errs.append(f"{bank_key}/{p.get('name')} pad {i}: unsupported chord_type {t}")
    if total > 256:
        errs.append(f"Total preset count {total} exceeds plugin MAX_PRESETS 256")
    return errs


def write_outputs(outputs: Dict[str, List[Dict[str, object]]], out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    for bank_key, presets in outputs.items():
        path = out_dir / f"{bank_key}.json"
        path.write_text(json.dumps(presets, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate Chord Flow banks from free-midi-chords")
    parser.add_argument("--source", required=True, help="Path to free-midi-chords chords.py")
    parser.add_argument("--out-dir", default="presets/private/free_midi", help="Output directory for generated JSON banks")
    parser.add_argument("--dsp", default="src/dsp/chord_flow_plugin.c", help="Path to DSP C source for chord type validation")
    args = parser.parse_args()

    source_path = Path(args.source)
    out_dir = Path(args.out_dir)
    dsp_path = Path(args.dsp)

    source_lists = load_chords_py(source_path)
    outputs = generate_presets(source_lists)
    supported_types = parse_supported_types(dsp_path)
    errors = validate_outputs(outputs, supported_types)

    if errors:
        print("Validation failed:")
        for e in errors[:200]:
            print(f" - {e}")
        if len(errors) > 200:
            print(f" ... and {len(errors) - 200} more")
        return 1

    write_outputs(outputs, out_dir)

    print("Generated banks:")
    total = 0
    for bank_key, presets in outputs.items():
        print(f" - {bank_key}: {len(presets)} presets")
        total += len(presets)
    print(f"Total presets: {total}")
    print(f"Output dir: {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
