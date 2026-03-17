/**
 * ui.js — Chord Floww
 *
 * Thin UI over dsp.so. DSP handles all chord logic, preset storage,
 * pad slots, strum, articulation. UI handles display + input only.
 *
 * Screens:
 *   'browser'  — preset browser (default)
 *   'edit'     — per-pad chord settings editor
 */

import { shouldFilterMessage, decodeDelta } from '/data/UserData/move-anything/shared/input_filter.mjs';
import { openTextEntry, closeTextEntry, isTextEntryActive, handleTextEntryMidi, drawTextEntry, tickTextEntry } from '/data/UserData/move-anything/shared/text_entry.mjs';

// ─── CC constants ─────────────────────────────────────────────────────────────
const CC_JOG   = 14;
const CC_CLICK = 3;
const CC_BACK  = 51;
const CC_SHIFT = 49;
const CC_UP    = 46;
const CC_DOWN  = 47;
const CC_LEFT  = 62;
const CC_RIGHT = 63;
const PAD_NOTE_MIN = 36;
const PAD_NOTE_MAX = 67;
const PAD_NOTE_FALLBACK_MIN = 68;
const PAD_NOTE_FALLBACK_MAX = 99;

// ─── Display ──────────────────────────────────────────────────────────────────
const SCREEN_W = 128;
const HEADER_Y = 1;
const FOOTER_Y = 56;
const CENTER_Y = 32;
const LIST_TOP = 13;
const LIST_BOT = 52;
const ROW_H    = 9;
const VISIBLE  = Math.floor((LIST_BOT - LIST_TOP) / ROW_H); // 4

// ─── Edit rows ────────────────────────────────────────────────────────────────
const EDIT_KEYS   = ['pad','root','chord_type','inversion','bass','pad_octave','strum','strum_dir','articulation','reverse_art','global_octave','global_transpose','bank','reset_patch','save'];
const EDIT_LABELS = ['Pad','Root','Chord Type','Inversion','Bass','Pad Oct','Strum','Strum Dir','Articulation','Reverse Art','Global Oct','Global Trans','Bank','Reset Patch','Save'];
const EDIT_ENUMS  = {
    root:         ['c','c#','d','d#','e','f','f#','g','g#','a','a#','b'],
    bass:         ['none','c','c#','d','d#','e','f','f#','g','g#','a','a#','b'],
    chord_type:   [
        'maj','min','5th','sus2','sus4','add2','add9','add11',
        '6th','min6','maj7','min7','dom7','maj9','min9','dom9',
        'dim','dim7','m7b5','aug','aug7','sus7','7sus2','7sus4',
        '9sus','sus9','11th','m11','sus11','13th','maj13','min13',
        'dom7add9','dom7b9','dom7#9','dom7#5','dom7alt','maj7#5','maj7b5','maj7#11','maj9#11',
        'madd9','madd11','mb5','mb6','m7b13','m7add13','m11b5','mM13','sus13','add13',
        '11sus','11sus2','13b9','5add9','5b9','6sus2','6sus2b5','6sus4','7#9#5','9#11',
        'dimM7','dim#5','dim11','addb9','aug#9','mb7','mb9','mb13','m6addb13',
        'no3','no5','maj7add6','maj7sus2','maj9no3','maj6'
    ],
    inversion:    ['root','1st','2nd','3rd'],
    strum_dir:    ['up','down'],
    articulation: ['off','on'],
    reverse_art:  ['off','on'],
};

// ─── State ────────────────────────────────────────────────────────────────────
let screen      = 'browser';
let needsRedraw = true;
let shiftHeld   = false;

// Browser
let presetIndex = 0;
let presetCount = 0;
let presetName  = '';
let bankIndex = 0;
let bankCount = 0;
let bankName = '';
let bankPreset = 0;
let bankPresetCount = 0;

// Edit
let editRow  = 0;
let editVals = {};
let editValueMode = false;
let saveStatus = '---';
let saveFlashTicks = 0;
let resetStatus = 'ready';
let resetFlashTicks = 0;
let resetConfirmArmed = false;

// ─── Helpers ──────────────────────────────────────────────────────────────────
function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }
function trunc(s, n) { return s.length > n ? s.substring(0, n - 1) + '~' : s; }
function formatEditValue(key, raw) {
    if (key === 'root') return raw.toUpperCase();
    if (key === 'bass') return raw === 'none' ? raw : raw.toUpperCase();
    if (key === 'bank') {
        const n = (parseInt(raw) || 0) + 1;
        const suffix = bankName ? ` ${trunc(bankName, 9)}` : '';
        return `${n}${suffix}`;
    }
    return raw;
}
function noteToPad(note) {
    if (note >= PAD_NOTE_MIN && note <= PAD_NOTE_MAX) return note - PAD_NOTE_MIN + 1;
    if (note >= PAD_NOTE_FALLBACK_MIN && note <= PAD_NOTE_FALLBACK_MAX) return note - PAD_NOTE_FALLBACK_MIN + 1;
    return 0;
}

// ─── DSP communication ────────────────────────────────────────────────────────
function dspGet(key) {
    return host_module_get_param(key) || '';
}

function dspSet(key, val) {
    host_module_set_param(key, String(val));
}

function refreshPreset() {
    presetCount = parseInt(dspGet('preset_count')) || 0;
    presetIndex = parseInt(dspGet('preset'))       || 0;
    presetName  = dspGet('preset_name');
    bankCount = parseInt(dspGet('bank_count')) || 0;
    bankIndex = parseInt(dspGet('bank')) || 0;
    bankName = dspGet('bank_name');
    bankPreset = parseInt(dspGet('bank_preset')) || 0;
    bankPresetCount = parseInt(dspGet('bank_preset_count')) || 0;
}

function selectPreset(idx) {
    if (idx < 0) idx = presetCount - 1;
    if (idx >= presetCount) idx = 0;
    dspSet('preset', idx);
    refreshPreset();
    needsRedraw = true;
}

function readEditVals(knownPad) {
    // Read all pad params from DSP — DSP returns values for current active pad
    editVals = {};
    for (const k of EDIT_KEYS) {
        if (k === 'save' || k === 'reset_patch') continue;
        editVals[k] = dspGet(k);
    }
    editVals.save = saveStatus;
    editVals.reset_patch = resetStatus;
    // If caller knows the pad number for certain, use it directly
    // (guards against DSP not yet reflecting the switch)
    if (knownPad !== undefined) {
        editVals['pad'] = String(knownPad);
    }
}

function triggerResetPatch() {
    if (!resetConfirmArmed) {
        resetConfirmArmed = true;
        resetStatus = 'confirm';
        editVals.reset_patch = resetStatus;
        needsRedraw = true;
        return;
    }
    dspSet('reset_patch', '1');
    resetConfirmArmed = false;
    resetStatus = 'done';
    resetFlashTicks = 40;
    readEditVals();
    editVals.reset_patch = resetStatus;
    needsRedraw = true;
}

function triggerSave() {
    const fallbackName = `Preset ${Math.max(1, presetCount + 1)}`;
    openTextEntry({
        title: 'Save Preset',
        initialText: fallbackName,
        onConfirm: (text) => {
            const trimmed = (text || '').trim();
            const nameToSave = trimmed.length > 0 ? trimmed : 'save';
            saveStatus = 'saving';
            editVals.save = saveStatus;
            dspSet('save', nameToSave);
            refreshPreset();
            saveStatus = 'saved';
            saveFlashTicks = 40;
            editVals.save = saveStatus;
            needsRedraw = true;
        },
        onCancel: () => {
            needsRedraw = true;
        }
    });
}

function cycleVal(delta) {
    const key = EDIT_KEYS[editRow];
    if (!key) return;
    if (key === 'save') {
        if (delta > 0) triggerSave();
        return;
    }
    if (key === 'reset_patch') {
        if (delta > 0) triggerResetPatch();
        return;
    }

    const cur = editVals[key] || '';

    if (EDIT_ENUMS[key]) {
        // Clamp to ±1: jog sends large relative values (e.g. 2, 4, 16…) which
        // would wrap enums with 12 or 16 options back to the same position.
        const step = delta > 0 ? 1 : -1;
        const opts = EDIT_ENUMS[key];
        let i = opts.indexOf(cur);
        if (i < 0) i = 0;
        i = ((i + step) + opts.length) % opts.length;
        dspSet(key, opts[i]);
        editVals[key] = opts[i];
    } else {
        if (key === 'bank') {
            const maxBank = Math.max(0, (parseInt(dspGet('bank_count')) || 1) - 1);
            const n = clamp((parseInt(cur) || 0) + delta, 0, maxBank);
            dspSet(key, n);
            refreshPreset();
            readEditVals();
            needsRedraw = true;
            return;
        }
        const isOctave = key === 'global_octave' || key === 'pad_octave';
        const isTranspose = key === 'global_transpose';
        const minVal = key === 'pad' ? 1 : (isOctave ? -6 : (isTranspose ? -12 : 0));
        const maxVal = key === 'pad' ? 32 : (isOctave ? 6 : (isTranspose ? 12 : 100));
        const n = clamp((parseInt(cur) || 0) + delta, minVal, maxVal);
        dspSet(key, n);
        editVals[key] = String(n);
        // Pad change: re-read all values from DSP for the new pad.
        // Keep our optimistic pad value in case DSP state lags.
        if (key === 'pad') {
            readEditVals(n);
        }
    }
    needsRedraw = true;
}

// ─── Drawing ──────────────────────────────────────────────────────────────────
function drawBrowserScreen() {
    clear_screen();
    const header = `CF: ${bankName || '---'}`;
    print(1, HEADER_Y, trunc(header, 22), 1);
    draw_rect(0, 11, SCREEN_W, 1, 1);

    if (presetCount > 0) {
        const num   = `${presetIndex + 1} / ${presetCount}`;
        const numX  = Math.floor((SCREEN_W - num.length * 6) / 2);
        print(numX, CENTER_Y - 8, num, 1);

        const name  = trunc(presetName || '---', 21);
        const nameX = Math.floor((SCREEN_W - name.length * 6) / 2);
        print(nameX, CENTER_Y + 4, name, 1);

        print(4,            CENTER_Y - 2, '<', 1);
        print(SCREEN_W - 8, CENTER_Y - 2, '>', 1);
    } else {
        print(4, CENTER_Y, 'No presets', 1);
    }

    draw_rect(0, 53, SCREEN_W, 1, 1);
        print(1,  FOOTER_Y, 'Clk:load+edit', 1);
        print(84, FOOTER_Y, 'Jog:browse', 1);
    host_flush_display();
    needsRedraw = false;
}

function drawEditScreen() {
    clear_screen();
    const pad = editVals['pad'] || '1';
    print(1, HEADER_Y, trunc(`EX > Pad ${pad}`, 22), 1);
    draw_rect(0, 11, SCREEN_W, 1, 1);

    const scroll = clamp(
        editRow - Math.floor(VISIBLE / 2),
        0, Math.max(0, EDIT_KEYS.length - VISIBLE)
    );

    for (let i = 0; i < VISIBLE; i++) {
        const idx  = scroll + i;
        if (idx >= EDIT_KEYS.length) break;
        const y    = LIST_TOP + i * ROW_H;
        const sel  = (idx === editRow);
        const key  = EDIT_KEYS[idx];
        const lbl  = EDIT_LABELS[idx];
        const val  = formatEditValue(key, String(editVals[key] || '?'));
        const valX = SCREEN_W - val.length * 6 - 3;

        if (sel) {
            fill_rect(0, y, SCREEN_W, ROW_H, 1);
            print(4,    y + 1, lbl, 0);
            print(valX, y + 1, val, 0);
        } else {
            print(4,    y + 1, lbl, 1);
            print(valX, y + 1, val, 1);
        }
    }

    // Scroll dot
    if (EDIT_KEYS.length > 1) {
        const dotY = LIST_TOP + Math.floor(
            editRow * (LIST_BOT - LIST_TOP - 3) / (EDIT_KEYS.length - 1)
        );
        draw_rect(125, dotY, 2, 3, 1);
    }

    draw_rect(0, 53, SCREEN_W, 1, 1);
    if (editValueMode) {
        print(1,  FOOTER_Y, 'Jog:val', 1);
        print(48, FOOTER_Y, 'Clk:done', 1);
    } else {
        print(1,  FOOTER_Y, 'Jog:row', 1);
        print(48, FOOTER_Y, 'Clk:edit', 1);
    }
    print(108, FOOTER_Y, 'Bk', 1);
    host_flush_display();
    needsRedraw = false;
}

function redraw() {
    if (screen === 'edit') { drawEditScreen(); return; }
    drawBrowserScreen();
}

// ─── Input ────────────────────────────────────────────────────────────────────
function handleJogTurn(delta) {
    if (screen === 'browser') {
        selectPreset(presetIndex + delta);
    } else if (screen === 'edit') {
        if (editValueMode) {
            cycleVal(delta);
        } else {
            if (resetConfirmArmed) {
                resetConfirmArmed = false;
                resetStatus = 'ready';
                editVals.reset_patch = resetStatus;
            }
            editRow = clamp(editRow + delta, 0, EDIT_KEYS.length - 1);
            needsRedraw = true;
        }
    }
}

function handleClick() {
    if (screen === 'browser') {
        // Only apply preset when selected preset name differs from active name.
        // This preserves unsaved edits when re-entering edit on the same preset.
        const activePresetName = dspGet('preset_name') || '';
        if (presetName !== activePresetName) {
            dspSet('preset', presetIndex);
        }
        saveStatus  = '---';
        saveFlashTicks = 0;
        editValueMode = false;
        readEditVals();
        editRow     = 0;
        screen      = 'edit';
        needsRedraw = true;
    } else if (screen === 'edit') {
        const key = EDIT_KEYS[editRow];
        if (key === 'save') {
            triggerSave();
            return;
        }
        if (key === 'reset_patch') {
            triggerResetPatch();
            return;
        }
        if (editValueMode) {
            editValueMode = false;
        } else {
            editValueMode = true;
        }
        needsRedraw = true;
    }
}

function handleBack() {
    if (screen === 'edit') {
        resetConfirmArmed = false;
        resetStatus = 'ready';
        editValueMode = false;
        screen      = 'browser';
        refreshPreset();
        needsRedraw = true;
    } else {
        host_return_to_menu();
    }
}

function handleUpDown(dir) {
    if (screen === 'edit') {
        if (!editValueMode) {
            editRow = clamp(editRow + dir, 0, EDIT_KEYS.length - 1);
            needsRedraw = true;
        }
    } else {
        selectPreset(presetIndex + dir);
    }
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────
globalThis.init = function() {
    if (isTextEntryActive()) {
        closeTextEntry();
    }
    refreshPreset();
    screen      = 'browser';
    editValueMode = false;
    needsRedraw = true;
};

globalThis.tick = function() {
    if (isTextEntryActive()) {
        if (tickTextEntry()) needsRedraw = true;
        drawTextEntry();
        return;
    }

    // Poll DSP's current_pad while in edit mode. This catches pad switches that
    // come through process_midi pad note handling regardless of whether
    // onMidiMessageInternal also fires for them.
    if (screen === 'edit') {
        const dspPad = dspGet('pad');
        if (dspPad && dspPad !== editVals['pad']) {
            readEditVals(parseInt(dspPad));
            needsRedraw = true;
        }
        if (saveFlashTicks > 0) {
            saveFlashTicks -= 1;
            if (saveFlashTicks === 0 && saveStatus !== '---') {
                saveStatus = '---';
                editVals.save = saveStatus;
                needsRedraw = true;
            }
        }
        if (resetFlashTicks > 0) {
            resetFlashTicks -= 1;
            if (resetFlashTicks === 0 && resetStatus !== 'ready') {
                resetStatus = 'ready';
                editVals.reset_patch = resetStatus;
                needsRedraw = true;
            }
        }
    }
    if (needsRedraw) redraw();
};

globalThis.onMidiMessageInternal = function(data) {
    if (isTextEntryActive()) {
        handleTextEntryMidi(data);
        return;
    }

    if (shouldFilterMessage(data)) return;
    const status = data[0], d1 = data[1], d2 = data[2];

    if (status === 0xB0 && d1 === CC_SHIFT)            { shiftHeld = d2 > 0;           return; }
    if (status === 0xB0 && d1 === CC_BACK  && d2 > 0)  { handleBack();                  return; }
    if (status === 0xB0 && d1 === CC_CLICK && d2 > 0)  { handleClick();                 return; }
    if (status === 0xB0 && d1 === CC_UP    && d2 > 0)  { handleUpDown(-1);              return; }
    if (status === 0xB0 && d1 === CC_DOWN  && d2 > 0)  { handleUpDown(1);               return; }
    if (status === 0xB0 && d1 === CC_LEFT  && d2 > 0)  { handleJogTurn(-1);             return; }
    if (status === 0xB0 && d1 === CC_RIGHT && d2 > 0)  { handleJogTurn(1);              return; }
    if (status === 0xB0 && d1 === CC_JOG)              {
        const delta = decodeDelta(d2);
        if (delta !== 0) handleJogTurn(delta);
        return;
    }

    // Pad press — tell DSP to switch active pad, refresh edit if open
    if ((status & 0xF0) === 0x90 && d2 > 0) {
        const padNum = noteToPad(d1);
        if (padNum <= 0) return;
        dspSet('pad', padNum);        // synchronously update DSP's current_pad
        if (screen === 'edit') {
            readEditVals(padNum);      // pass padNum so header is always correct
            needsRedraw = true;
        }
        return;
    }
};

globalThis.onMidiMessageExternal = function(data) {
    if (isTextEntryActive()) {
        handleTextEntryMidi(data);
        return;
    }

    if (shouldFilterMessage(data)) return;
    const status = data[0], d1 = data[1], d2 = data[2];
    if ((status & 0xF0) === 0x90 && d2 > 0) {
        const padNum = noteToPad(d1);
        if (padNum <= 0) return;
        dspSet('pad', padNum);
        if (screen === 'edit') { readEditVals(padNum); needsRedraw = true; }
    }
};
