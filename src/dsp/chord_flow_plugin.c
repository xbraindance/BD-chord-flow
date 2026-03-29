/**
 * chord_flow_plugin.c
 *
 * Move Anything MIDI FX — midi_fx_api_v1
 *
 * Architecture:
 *   - 16 pad slots per preset, each with independent chord settings
 *   - Loading a preset copies all 16 pad configs into active_pad_slots[]
 *   - Pressing a pad note (36-67, fallback 68-99) switches current_pad
 *     and plays that pad's chord
 *   - "save" param writes active_pad_slots[] as a new/overwritten preset
 *
 * Params (string key / string value):
 *   preset        int      0-based index — loads all 16 pad slots from that preset
 *   preset_count  int      (read-only)
 *   preset_name   string   (read-only) name of current preset
 *   bank          int      0-based bank index
 *   bank_count    int      (read-only)
 *   bank_name     string   (read-only)
 *   bank_preset   int      (read-only) active preset index inside active bank
 *   bank_preset_count int  (read-only) preset count in active bank
 *   current_pad   int      1-32 active pad for editing
 *   global_octave int      -6..6 octave transpose for all pads in active preset
 *   global_transpose int    -12..12 semitone transpose for all pads in active preset
 *   pad_octave    int      -6..6 octave transpose for current pad
 *   root          enum     c/c#/d/d#/e/f/f#/g/g#/a/a#/b
 *   bass          enum     none/c/c#/d/d#/e/f/f#/g/g#/a/a#/b (slash bass)
 *   chord_type    enum     basic -> advanced ordered chord list
 *   inversion     enum     root/1st/2nd/3rd
 *   strum         int      0-100 (ms between notes)
 *   strum_dir     enum     up/down
 *   articulation  enum     off/on
 *   reverse_art   enum     off/on
 *   save          string   preset name to save as (writes current pad collection)
 *   state         string   (read-only) JSON snapshot of current pad's settings
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>

/* ── API version ──────────────────────────────────────────────────────────── */
#define MIDI_FX_API_VERSION   1
#define MIDI_FX_MAX_OUT       16
#define MAX_PRESETS           256
#define MAX_CHORD_NOTES       8
#define MAX_PRESET_NAME       48
#define MAX_BANK_NAME         32
#define PAD_COUNT             32
#define MAX_PENDING           256
#define PRESETS_SUBDIR        "presets"
#define CUSTOM_CHORDS_SUBDIR  "custom_chords"
#define MAX_CUSTOM_CHORDS     32
#define MAX_CUSTOM_NAME       16
#define USER_PRESETS_FILE     "user.json"
#define DEFAULT_PRESETS_FILE  "default.json"
#define USER_BANK_NAME        "User"
#define FACTORY_BANK_NAME     "Factory"
#define PAD_TRIGGER_BASE_NOTE 36
#define OCT_MIN               -6
#define OCT_MAX               6
#define GLOBAL_OCT_DEFAULT    2
#define TRANSPOSE_MIN         -12
#define TRANSPOSE_MAX         12
#define GLOBAL_TRANSPOSE_DEFAULT 0

/* ── Logging macro per CLAUDE.md standards ────────────────────────────────── */
/* LOG: safe printf-style logging via host->log */
#define LOG(...) do { \
    if (inst && inst->host && inst->host->log) { \
        char _logbuf[256]; \
        snprintf(_logbuf, sizeof(_logbuf), __VA_ARGS__); \
        inst->host->log(_logbuf); \
    } \
} while(0)

/* ── Host API (passed to move_midi_fx_init) ──────────────────────────────── */
typedef struct {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int  (*midi_send_internal)(const uint8_t *msg, int len);
    int  (*midi_send_external)(const uint8_t *msg, int len);
    int  (*get_clock_status)(void);
} host_api_v1_t;

/* ── Plugin API vtable (per midi_fx_api_v1 spec) ─────────────────────────── */
typedef struct midi_fx_api_v1 {
    uint32_t api_version;  /* Must be 1 (MIDI_FX_API_VERSION) */

    void* (*create_instance)(const char *module_dir, const char *config_json);
    void (*destroy_instance)(void *instance);

    int (*process_midi)(void *instance,
                        const uint8_t *in_msg, int in_len,
                        uint8_t out_msgs[][3], int out_lens[],
                        int max_out);

    int (*tick)(void *instance,
                int frames, int sample_rate,
                uint8_t out_msgs[][3], int out_lens[],
                int max_out);

    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
} midi_fx_api_v1_t;

/* ── Enum tables ─────────────────────────────────────────────────────────── */
#define ROOT_COUNT 12
static const char *ROOT_NAMES[ROOT_COUNT] = {
    "c","c#","d","d#","e","f","f#","g","g#","a","a#","b"
};

static const char *TYPE_NAMES[] = {
    /* Basic */
    "maj","min","5th","sus2","sus4","add2","add9","add11",
    "6th","min6","maj7","min7","dom7","maj9","min9","dom9",
    /* Extended */
    "dim","dim7","m7b5","aug","aug7","sus7","7sus2","7sus4",
    "9sus","sus9","11th","m11","sus11","13th","maj13","min13",
    /* Altered / specialized */
    "dom7add9","dom7b9","dom7#9","dom7#5","dom7alt","maj7#5","maj7b5","maj7#11","maj9#11",
    "madd9","madd11","mb5","mb6","m7b9","m7b13","m7add13","m11b5","mM13","sus13","add13",
    /* Extra specialized */
    "11sus","11sus2","13b9","5add9","5b9","6sus2","6sus2b5","6sus4","7#9#5","9#11",
    "dimM7","dim#5","dim11","addb9","aug#9","mb7","mb9","mb13","m6addb13",
    /* Utility */
    "no3","no5","maj7add6","maj7sus2","maj9no3","maj6","6add9"
};
#define TYPE_COUNT ((int)(sizeof(TYPE_NAMES) / sizeof(TYPE_NAMES[0])))

#define BASS_COUNT 13
static const char *BASS_NAMES[BASS_COUNT] = {
    "none","c","c#","d","d#","e","f","f#","g","g#","a","a#","b"
};

#define INV_COUNT 4
static const char *INV_NAMES[INV_COUNT] = { "root","1st","2nd","3rd" };

#define DIR_COUNT 2
static const char *DIR_NAMES[DIR_COUNT] = { "up","down" };

#define ART_COUNT 2
static const char *ART_NAMES[ART_COUNT] = { "off","on" };

/* ── Chord interval tables ───────────────────────────────────────────────── */
typedef struct { int offs[8]; int count; } chord_def_t;

typedef struct {
    chord_def_t def;
    char name[MAX_CUSTOM_NAME];
} custom_chord_t;

static const chord_def_t CHORD_DEFS[] = {
    {{0,4,7},3},               /* maj */
    {{0,3,7},3},               /* min */
    {{0,7},2},                 /* 5th */
    {{0,2,7},3},               /* sus2 */
    {{0,5,7},3},               /* sus4 */
    {{0,2,4,7},4},             /* add2 */
    {{0,4,7,14},4},            /* add9 */
    {{0,4,7,17},4},            /* add11 */
    {{0,4,7,9},4},             /* 6th */
    {{0,3,7,9},4},             /* min6 */
    {{0,4,7,11},4},            /* maj7 */
    {{0,3,7,10},4},            /* min7 */
    {{0,4,7,10},4},            /* dom7 */
    {{0,4,7,11,14},5},         /* maj9 */
    {{0,3,7,10,14},5},         /* min9 */
    {{0,4,7,10,14},5},         /* dom9 */
    {{0,3,6},3},               /* dim */
    {{0,3,6,9},4},             /* dim7 */
    {{0,3,6,10},4},            /* m7b5 */
    {{0,4,8},3},               /* aug */
    {{0,4,8,10},4},            /* aug7 */
    {{0,5,7,10},4},            /* sus7 */
    {{0,2,7,10},4},            /* 7sus2 */
    {{0,5,7,10},4},            /* 7sus4 */
    {{0,5,7,10,14},5},         /* 9sus */
    {{0,2,7,14},4},            /* sus9 */
    {{0,4,7,10,14,17},6},      /* 11th */
    {{0,3,7,10,14,17},6},      /* m11 */
    {{0,5,7,10,17},5},         /* sus11 */
    {{0,4,7,10,14,21},6},      /* 13th */
    {{0,4,7,11,14,21},6},      /* maj13 */
    {{0,3,7,10,14,21},6},      /* min13 */
    {{0,4,7,10,14},5},         /* dom7add9 */
    {{0,4,7,10,13},5},         /* dom7b9 */
    {{0,4,7,10,15},5},         /* dom7#9 */
    {{0,4,8,10},4},            /* dom7#5 */
    {{0,4,8,10,13},5},         /* dom7alt */
    {{0,4,8,11},4},            /* maj7#5 */
    {{0,4,6,11},4},            /* maj7b5 */
    {{0,4,7,11,18},5},         /* maj7#11 */
    {{0,4,7,11,14,18},6},      /* maj9#11 */
    {{0,3,7,14},4},            /* madd9 */
    {{0,3,7,17},4},            /* madd11 */
    {{0,3,6},3},               /* mb5 */
    {{0,3,7,8},4},             /* mb6 */
    {{0,3,7,10,13},5},         /* m7b9 */
    {{0,3,7,10,20},5},         /* m7b13 */
    {{0,3,7,10,21},5},         /* m7add13 */
    {{0,3,6,10,14,17},6},      /* m11b5 */
    {{0,3,7,11,14,21},6},      /* mM13 */
    {{0,5,7,10,21},5},         /* sus13 */
    {{0,4,7,21},4},            /* add13 */
    {{0,5,7,10,17},5},         /* 11sus */
    {{0,2,5,7,10,17},6},       /* 11sus2 */
    {{0,4,7,10,13,21},6},      /* 13b9 */
    {{0,2,7},3},               /* 5add9 */
    {{0,1,7},3},               /* 5b9 */
    {{0,2,7,9},4},             /* 6sus2 */
    {{0,2,6,9},4},             /* 6sus2b5 */
    {{0,5,7,9},4},             /* 6sus4 */
    {{0,4,8,10,15},5},         /* 7#9#5 */
    {{0,4,7,10,14,18},6},      /* 9#11 */
    {{0,3,6,11},4},            /* dimM7 */
    {{0,3,8},3},               /* dim#5 */
    {{0,3,6,10,17},5},         /* dim11 */
    {{0,1,4,7},4},             /* addb9 */
    {{0,4,8,15},4},            /* aug#9 */
    {{0,3,7,10},4},            /* mb7 */
    {{0,1,3,7},4},             /* mb9 */
    {{0,3,7,8},4},             /* mb13 */
    {{0,3,7,8,9},5},           /* m6addb13 */
    {{0,7},2},                 /* no3 */
    {{0,4},2},                 /* no5 */
    {{0,4,7,11,9},5},          /* maj7add6 */
    {{0,2,7,11},4},            /* maj7sus2 */
    {{0,4,7,11},4},            /* maj9no3 */
    {{0,4,7,9},4},             /* maj6 */
    {{0,4,7,9,14},5}           /* 6add9 */
};

typedef char chord_defs_must_match_type_names[
    (sizeof(CHORD_DEFS) / sizeof(CHORD_DEFS[0]) == sizeof(TYPE_NAMES) / sizeof(TYPE_NAMES[0])) ? 1 : -1
];

/* ── Data structures ─────────────────────────────────────────────────────── */
typedef struct {
    int octave;       /* -6..6 */
    int root;         /* 0-11 */
    int bass;         /* -1 none, else 0-11 */
    int type;         /* 0-(TYPE_COUNT-1) */
    int inversion;    /* 0-3  */
    int strum;        /* 0-100 ms */
    int strum_dir;    /* 0=up 1=down */
    int articulation; /* 0=off 1=on */
    int reverse_art;  /* 0=off 1=on */
} pad_slot_t;

typedef struct {
    char      name[MAX_PRESET_NAME];
    char      bank[MAX_BANK_NAME];
    int       global_octave; /* -6..6 */
    int       global_transpose; /* -12..12 */
    pad_slot_t slots[PAD_COUNT];
} preset_t;

typedef struct {
    char name[MAX_BANK_NAME];
    int first_preset;
    int count;
} bank_info_t;

typedef struct {
    uint8_t note;
    uint8_t vel;
    uint8_t ch;
    int     delay_frames;
} pending_note_t;

/* Instance structure with host pointer for logging (per CLAUDE.md) */
typedef struct {
    const host_api_v1_t *host;      /* Host API reference for logging */
    char       module_dir[700];
    preset_t   presets[MAX_PRESETS];
    int        preset_count;
    bank_info_t banks[MAX_PRESETS];
    int        bank_count;
    int        active_bank;
    int        active_preset;       /* index of loaded preset, -1 = none */
    int        active_global_octave;/* -6..6 */
    int        active_global_transpose;/* -12..12 */
    pad_slot_t active_pad_slots[PAD_COUNT];
    int        current_pad;         /* 1-based */
    int8_t     held_out[128][MAX_CHORD_NOTES];
    int        held_out_count[128];
    pending_note_t pending[MAX_PENDING];
    int        pending_count;
    int        sample_rate;
    custom_chord_t custom_chords[MAX_CUSTOM_CHORDS];
    int        custom_chord_count;
    char       custom_chords_dir[800]; /* actual path used to load custom chords */
} expchords_t;

/* ── Forward declarations ────────────────────────────────────────────── */
static const char *type_name(expchords_t *inst, int type_idx);

/* ── Helpers (snake_case per CLAUDE.md) ─────────────────────────────────── */
static int clamp_i(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static int find_enum(const char **names, int count, const char *val) {
    int i;
    for (i = 0; i < count; i++)
        if (strcmp(names[i], val) == 0) return i;
    return -1;
}

static int parse_enum(const char **names, int count, const char *val) {
    int v = find_enum(names, count, val);
    if (v >= 0) return v;
    v = atoi(val);
    return (v >= 0 && v < count) ? v : 0;
}

static int is_blank_str(const char *s) {
    if (!s) return 1;
    while (*s) {
        if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') return 0;
        s++;
    }
    return 1;
}

static void copy_cstr(char *dst, int dst_len, const char *src) {
    if (!dst || dst_len <= 0) return;
    if (!src) src = "";
    strncpy(dst, src, (size_t)dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static int same_bank(const char *a, const char *b) {
    return strncmp(a ? a : "", b ? b : "", MAX_BANK_NAME) == 0;
}

static int has_json_ext(const char *name) {
    size_t n;
    if (!name) return 0;
    n = strlen(name);
    return (n > 5 && strcmp(name + n - 5, ".json") == 0);
}

static void infer_bank_name_from_filename(const char *filename, char *out, int out_len) {
    int i = 0;
    int new_word = 1;
    if (strcmp(filename, DEFAULT_PRESETS_FILE) == 0) {
        copy_cstr(out, out_len, FACTORY_BANK_NAME);
        return;
    }
    if (strcmp(filename, USER_PRESETS_FILE) == 0) {
        copy_cstr(out, out_len, USER_BANK_NAME);
        return;
    }
    while (*filename && *filename != '.' && i < out_len - 1) {
        char c = *filename++;
        if (c == '_' || c == '-') {
            out[i++] = ' ';
            new_word = 1;
            continue;
        }
        if (new_word && isalpha((unsigned char)c)) {
            out[i++] = (char)toupper((unsigned char)c);
            new_word = 0;
        } else {
            out[i++] = c;
            new_word = 0;
        }
    }
    out[i] = '\0';
    if (i == 0) copy_cstr(out, out_len, FACTORY_BANK_NAME);
}

static int find_bank_index_by_name(expchords_t *inst, const char *name) {
    int i;
    for (i = 0; i < inst->bank_count; i++) {
        if (same_bank(inst->banks[i].name, name)) return i;
    }
    return -1;
}

static int bank_first_preset(expchords_t *inst, int bank_idx) {
    if (bank_idx < 0 || bank_idx >= inst->bank_count) return -1;
    return inst->banks[bank_idx].first_preset;
}

static int bank_last_preset(expchords_t *inst, int bank_idx) {
    int first;
    if (bank_idx < 0 || bank_idx >= inst->bank_count) return -1;
    first = inst->banks[bank_idx].first_preset;
    return first + inst->banks[bank_idx].count - 1;
}

static void rebuild_banks(expchords_t *inst) {
    int i;
    inst->bank_count = 0;
    for (i = 0; i < inst->preset_count; i++) {
        preset_t *pr = &inst->presets[i];
        if (inst->bank_count == 0 || !same_bank(inst->banks[inst->bank_count - 1].name, pr->bank)) {
            bank_info_t *bk = &inst->banks[inst->bank_count++];
            copy_cstr(bk->name, MAX_BANK_NAME, pr->bank);
            bk->first_preset = i;
            bk->count = 1;
        } else {
            inst->banks[inst->bank_count - 1].count++;
        }
    }
    if (inst->bank_count == 0) {
        copy_cstr(inst->banks[0].name, MAX_BANK_NAME, FACTORY_BANK_NAME);
        inst->banks[0].first_preset = 0;
        inst->banks[0].count = 0;
        inst->bank_count = 1;
    }
    if (inst->active_bank < 0 || inst->active_bank >= inst->bank_count) inst->active_bank = 0;
}

static void set_active_bank_from_preset(expchords_t *inst, int preset_idx) {
    int i;
    for (i = 0; i < inst->bank_count; i++) {
        int first = inst->banks[i].first_preset;
        int last = first + inst->banks[i].count - 1;
        if (preset_idx >= first && preset_idx <= last) {
            inst->active_bank = i;
            return;
        }
    }
}

static int note_to_pad(int note) {
    if (note >= 36 && note <= 67) return note - 36 + 1;
    if (note >= 68 && note <= 99) return note - 68 + 1;
    return 0;
}

/* ── JSON mini-parser ────────────────────────────────────────────────────── */
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *parse_str(const char *p, char *dst, int dlen) {
    p = skip_ws(p);
    if (*p != '"') return p;
    p++;
    int i = 0;
    while (*p && *p != '"') {
        if (*p == '\\') p++;
        if (i < dlen - 1) dst[i++] = *p;
        p++;
    }
    dst[i] = '\0';
    if (*p == '"') p++;
    return p;
}

static const char *parse_int_val(const char *p, int *out) {
    p = skip_ws(p);
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    *out = 0;
    while (*p >= '0' && *p <= '9') { *out = *out * 10 + (*p - '0'); p++; }
    if (neg) *out = -(*out);
    return p;
}

static const char *json_find_key(const char *json, const char *key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p += strlen(search);
    p = skip_ws(p);
    if (*p != ':') return NULL;
    return p + 1;
}

static int json_get_int(const char *json, const char *key, int def) {
    const char *p = json_find_key(json, key);
    if (!p) return def;
    int v = def;
    parse_int_val(p, &v);
    return v;
}

static int json_get_str(const char *json, const char *key, char *dst, int dlen) {
    const char *p = json_find_key(json, key);
    if (!p) return 0;
    p = skip_ws(p);
    if (*p != '"') return 0;
    parse_str(p, dst, dlen);
    return 1;
}

/* ── Preset JSON loading ─────────────────────────────────────────────────── */
static pad_slot_t parse_slot(const char *obj, expchords_t *inst) {
    pad_slot_t s;
    char tmp[96];
    int max_type = TYPE_COUNT + (inst ? inst->custom_chord_count : 0) - 1;

    /* octave: int */
    s.octave = clamp_i(json_get_int(obj,"octave",0), OCT_MIN, OCT_MAX);

    /* root: try string first ("c","c#",...), fall back to int */
    if (json_get_str(obj, "root", tmp, sizeof(tmp))) {
        int v = find_enum(ROOT_NAMES, ROOT_COUNT, tmp);
        s.root = (v >= 0) ? v : clamp_i(atoi(tmp), 0, ROOT_COUNT-1);
    } else {
        s.root = clamp_i(json_get_int(obj,"root",0), 0, ROOT_COUNT-1);
    }

    /* chord_type: try string first ("maj","min",...), then custom names, fall back to int */
    if (json_get_str(obj, "chord_type", tmp, sizeof(tmp))) {
        int v = find_enum(TYPE_NAMES, TYPE_COUNT, tmp);
        if (v >= 0) {
            s.type = v;
        } else if (inst) {
            /* Check custom chord names */
            int found = 0;
            for (int i = 0; i < inst->custom_chord_count; i++) {
                if (strcmp(tmp, inst->custom_chords[i].name) == 0) {
                    s.type = TYPE_COUNT + i;
                    found = 1;
                    break;
                }
            }
            if (!found) s.type = clamp_i(atoi(tmp), 0, max_type);
        } else {
            s.type = clamp_i(atoi(tmp), 0, TYPE_COUNT-1);
        }
    } else {
        s.type = clamp_i(json_get_int(obj,"chord_type",0), 0, max_type);
    }

    /* inversion: int only */
    s.inversion = clamp_i(json_get_int(obj,"inversion",0), 0, INV_COUNT-1);

    /* bass: try string ("none"/root note), fall back to int */
    if (json_get_str(obj, "bass", tmp, sizeof(tmp))) {
        int v = find_enum(BASS_NAMES, BASS_COUNT, tmp);
        s.bass = (v >= 0) ? (v - 1) : -1;
    } else {
        int v = json_get_int(obj, "bass", -1);
        s.bass = (v >= 0 && v < ROOT_COUNT) ? v : -1;
    }

    /* strum: int */
    s.strum = clamp_i(json_get_int(obj,"strum",0), 0, 100);

    /* strum_dir: try string ("up"/"down"), fall back to int */
    if (json_get_str(obj, "strum_dir", tmp, sizeof(tmp))) {
        int v = find_enum(DIR_NAMES, DIR_COUNT, tmp);
        s.strum_dir = (v >= 0) ? v : clamp_i(atoi(tmp), 0, DIR_COUNT-1);
    } else {
        s.strum_dir = clamp_i(json_get_int(obj,"strum_dir",0), 0, DIR_COUNT-1);
    }

    /* articulation: try string ("off"/"on"), fall back to int */
    if (json_get_str(obj, "articulation", tmp, sizeof(tmp))) {
        int v = find_enum(ART_NAMES, ART_COUNT, tmp);
        s.articulation = (v >= 0) ? v : clamp_i(atoi(tmp), 0, ART_COUNT-1);
    } else {
        s.articulation = clamp_i(json_get_int(obj,"articulation",0), 0, ART_COUNT-1);
    }

    /* reverse_art: try string ("off"/"on"), fall back to int */
    if (json_get_str(obj, "reverse_art", tmp, sizeof(tmp))) {
        int v = find_enum(ART_NAMES, ART_COUNT, tmp);
        s.reverse_art = (v >= 0) ? v : clamp_i(atoi(tmp), 0, ART_COUNT-1);
    } else {
        s.reverse_art = clamp_i(json_get_int(obj,"reverse_art",0), 0, ART_COUNT-1);
    }

    return s;
}

static pad_slot_t default_slot(void) {
    pad_slot_t s;
    memset(&s, 0, sizeof(s));
    s.bass = -1;
    return s;
}

static void reset_active_patch(expchords_t *inst) {
    int i;
    for (i = 0; i < PAD_COUNT; i++) {
        /* default_slot() is C major/root/no bass with neutral timing/articulation */
        inst->active_pad_slots[i] = default_slot();
    }
}

/* ── File I/O with host logging (CLAUDE.md compliant) ───────────────────── */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long end = ftell(f);
    if (end <= 0 || end > 2L * 1024L * 1024L) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    size_t sz = (size_t)end;
    char *buf = malloc(sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t rd = fread(buf, 1, sz, f);
    if (rd != sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

/* ── Minimal MIDI file parser for custom chord definitions ────────────── */
/* Reads a .mid file and extracts note-on events to build a chord_def_t.
 * Only handles what's needed: MThd header, MTrk chunks, note-on events.
 * Returns 1 on success, 0 on failure. */
static int parse_midi_chord(const char *path, chord_def_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    /* Read entire file (cap at 8KB — a single-chord MIDI should be tiny) */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long end = ftell(f);
    if (end <= 14 || end > 8192) { fclose(f); return 0; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }

    size_t sz = (size_t)end;
    unsigned char *buf = malloc(sz);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, sz, f) != sz) { free(buf); fclose(f); return 0; }
    fclose(f);

    /* Verify MThd header: "MThd" + 6-byte length */
    if (sz < 14 || memcmp(buf, "MThd", 4) != 0) { free(buf); return 0; }

    /* Collect unique note-on pitches across all tracks */
    int notes[MAX_CHORD_NOTES];
    int note_count = 0;
    size_t pos = 14; /* skip MThd (8 + 6 bytes) */

    while (pos + 8 < sz) {
        /* Look for MTrk chunk */
        if (memcmp(buf + pos, "MTrk", 4) != 0) { pos++; continue; }
        pos += 4;
        uint32_t trk_len = ((uint32_t)buf[pos] << 24) | ((uint32_t)buf[pos+1] << 16) |
                           ((uint32_t)buf[pos+2] << 8) | (uint32_t)buf[pos+3];
        pos += 4;
        size_t trk_end = pos + trk_len;
        if (trk_end > sz) trk_end = sz;

        unsigned char running_status = 0;
        while (pos < trk_end) {
            /* Skip variable-length delta time */
            while (pos < trk_end && (buf[pos] & 0x80)) pos++;
            if (pos >= trk_end) break;
            pos++; /* last byte of delta */

            if (pos >= trk_end) break;

            unsigned char b = buf[pos];

            /* Meta event: FF type len data */
            if (b == 0xFF) {
                pos++; /* skip FF */
                if (pos >= trk_end) break;
                pos++; /* skip type */
                /* Variable-length length */
                uint32_t mlen = 0;
                while (pos < trk_end && (buf[pos] & 0x80)) {
                    mlen = (mlen << 7) | (buf[pos] & 0x7F); pos++;
                }
                if (pos < trk_end) { mlen = (mlen << 7) | buf[pos]; pos++; }
                pos += mlen;
                continue;
            }

            /* SysEx: F0 ... F7 or F7 ... F7 */
            if (b == 0xF0 || b == 0xF7) {
                pos++;
                uint32_t slen = 0;
                while (pos < trk_end && (buf[pos] & 0x80)) {
                    slen = (slen << 7) | (buf[pos] & 0x7F); pos++;
                }
                if (pos < trk_end) { slen = (slen << 7) | buf[pos]; pos++; }
                pos += slen;
                continue;
            }

            /* Channel message */
            if (b & 0x80) {
                running_status = b;
                pos++;
            }
            /* else: running status, b is already data byte */

            unsigned char status_hi = running_status & 0xF0;
            if (status_hi == 0x90 || status_hi == 0x80) {
                /* Note on/off: 2 data bytes */
                unsigned char note, vel;
                if (b & 0x80) {
                    /* status byte was consumed, read 2 data bytes */
                    if (pos + 1 >= trk_end) break;
                    note = buf[pos]; vel = buf[pos+1]; pos += 2;
                } else {
                    /* running status: b is first data byte */
                    note = b;
                    if (pos + 1 >= trk_end) break;
                    vel = buf[pos+1]; pos += 2;
                }
                /* Note-on with velocity > 0 */
                if (status_hi == 0x90 && vel > 0 && note <= 127) {
                    /* Add if unique and room available */
                    int dup = 0;
                    for (int i = 0; i < note_count; i++) {
                        if (notes[i] == (int)note) { dup = 1; break; }
                    }
                    if (!dup && note_count < MAX_CHORD_NOTES) {
                        notes[note_count++] = (int)note;
                    }
                }
            } else if (status_hi == 0xC0 || status_hi == 0xD0) {
                /* Program change / channel pressure: 1 data byte */
                pos++; /* skip 1 data byte (running status: b was data, pos at b+1; new status: pos at data) */
            } else {
                /* Other 2-byte messages (0xA0, 0xB0, 0xE0) */
                pos += 2; /* skip 2 data bytes */
            }
        }
        pos = trk_end;
    }

    free(buf);

    if (note_count < 1) return 0;

    /* Sort notes ascending */
    for (int i = 0; i < note_count - 1; i++) {
        for (int j = i + 1; j < note_count; j++) {
            if (notes[j] < notes[i]) { int t = notes[i]; notes[i] = notes[j]; notes[j] = t; }
        }
    }

    /* Compute intervals relative to lowest note */
    out->count = note_count;
    for (int i = 0; i < note_count; i++)
        out->offs[i] = notes[i] - notes[0];
    for (int i = note_count; i < 8; i++)
        out->offs[i] = 0;

    return 1;
}

/* ── Load custom chord definitions from MIDI files ───────────────────── */
static void load_custom_chords(expchords_t *inst) {
    char dir_path[800];
    DIR *dir = NULL;

    /* Try primary path: module_dir/custom_chords */
    if (inst->module_dir && inst->module_dir[0]) {
        snprintf(dir_path, sizeof(dir_path), "%s/%s", inst->module_dir, CUSTOM_CHORDS_SUBDIR);
        dir = opendir(dir_path);
    }

    /* Fallback: try absolute path (Schwung location on Move) */
    if (!dir) {
        snprintf(dir_path, sizeof(dir_path), "/data/UserData/schwung/modules/midi_fx/chord-flow/%s", CUSTOM_CHORDS_SUBDIR);
        dir = opendir(dir_path);
    }

    if (!dir) {
        LOG("custom_chords folder not found (module_dir='%s')", inst->module_dir);
        return;
    }

    strncpy(inst->custom_chords_dir, dir_path, sizeof(inst->custom_chords_dir) - 1);

    /* Collect .mid filenames first, then sort */
    char filenames[MAX_CUSTOM_CHORDS][64];
    int file_count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && file_count < MAX_CUSTOM_CHORDS) {
        const char *name = ent->d_name;
        if (name[0] == '.') continue;
        size_t len = strlen(name);
        if (len < 5) continue;
        /* Check .mid extension (case-insensitive) */
        if (!(  (name[len-4] == '.') &&
                (name[len-3] == 'm' || name[len-3] == 'M') &&
                (name[len-2] == 'i' || name[len-2] == 'I') &&
                (name[len-1] == 'd' || name[len-1] == 'D')  )) continue;
        strncpy(filenames[file_count], name, 63);
        filenames[file_count][63] = '\0';
        file_count++;
    }
    closedir(dir);

    /* Sort filenames alphabetically (numeric names like 001, 002 sort correctly) */
    for (int i = 0; i < file_count - 1; i++) {
        for (int j = i + 1; j < file_count; j++) {
            if (strcmp(filenames[j], filenames[i]) < 0) {
                char tmp[64];
                memcpy(tmp, filenames[i], 64);
                memcpy(filenames[i], filenames[j], 64);
                memcpy(filenames[j], tmp, 64);
            }
        }
    }

    inst->custom_chord_count = 0;
    for (int i = 0; i < file_count; i++) {
        char path[800];
        snprintf(path, sizeof(path), "%s/%s", dir_path, filenames[i]);
        chord_def_t def;
        if (parse_midi_chord(path, &def)) {
            custom_chord_t *cc = &inst->custom_chords[inst->custom_chord_count];
            cc->def = def;
            snprintf(cc->name, MAX_CUSTOM_NAME, "C%02d", inst->custom_chord_count + 1);
            inst->custom_chord_count++;
            LOG("loaded custom chord %s from %s (%d notes)", cc->name, filenames[i], def.count);
        }
    }
    LOG("custom chords loaded: %d", inst->custom_chord_count);
}

/* Parse presets JSON:
 *   [{"name":"X", "bank":"Factory", "global_octave":2, "global_transpose":0,
 *     "pads":[{...},{...},...32...]}, ...]
 */
static void load_presets_from_json(expchords_t *inst, const char *json, const char *default_bank) {
    const char *p = skip_ws(json);
    if (*p != '[') return;
    p++;

    while (*p && *p != ']' && inst->preset_count < MAX_PRESETS) {
        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p != '{') { p++; continue; }

        /* Find end of this object */
        const char *start = p;
        int depth = 0;
        while (*p) {
            if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
            p++;
        }
        int len = (int)(p - start);
        char *obj = malloc(len + 1);
        if (!obj) continue;
        memcpy(obj, start, len);
        obj[len] = '\0';

        int has_name;
        preset_t *pr = &inst->presets[inst->preset_count];
        copy_cstr(pr->name, MAX_PRESET_NAME, "preset");
        copy_cstr(pr->bank, MAX_BANK_NAME, default_bank);
        pr->global_octave = clamp_i(json_get_int(obj, "global_octave", GLOBAL_OCT_DEFAULT), OCT_MIN, OCT_MAX);
        pr->global_transpose = clamp_i(json_get_int(obj, "global_transpose", GLOBAL_TRANSPOSE_DEFAULT), TRANSPOSE_MIN, TRANSPOSE_MAX);
        has_name = json_get_str(obj, "name", pr->name, MAX_PRESET_NAME);
        if (!has_name || is_blank_str(pr->name)) {
            free(obj);
            continue;
        }
        json_get_str(obj, "bank", pr->bank, MAX_BANK_NAME);

        /* Check for pads array */
        const char *pads_pos = strstr(obj, "\"pads\"");
        if (pads_pos) {
            /* New format: each pad has its own slot */
            const char *arr = strchr(pads_pos, '[');
            if (arr) {
                arr++;
                int pad_idx = 0;
                while (*arr && *arr != ']' && pad_idx < PAD_COUNT) {
                    arr = skip_ws(arr);
                    if (*arr == ',') { arr++; continue; }
                    if (*arr != '{') { arr++; continue; }
                    const char *ps = arr;
                    int d = 0;
                    while (*arr) {
                        if (*arr == '{') d++;
                        else if (*arr == '}') { d--; if (d==0){arr++;break;} }
                        arr++;
                    }
                    int pl = (int)(arr - ps);
                    char *pobj = malloc(pl + 1);
                    if (pobj) {
                        memcpy(pobj, ps, pl);
                        pobj[pl] = '\0';
                        pr->slots[pad_idx] = parse_slot(pobj, inst);
                        free(pobj);
                    }
                    pad_idx++;
                }
                /* Fill remaining pads with default */
                while (pad_idx < PAD_COUNT)
                    pr->slots[pad_idx++] = default_slot();
            }
        }

        free(obj);
        inst->preset_count++;
    }
}

static void load_presets_from_file(expchords_t *inst, const char *path, const char *default_bank) {
    char *json = read_file(path);
    if (!json) return;
    load_presets_from_json(inst, json, default_bank);
    free(json);
}

static void load_additional_preset_files(expchords_t *inst, const char *presets_dir) {
    DIR *dir;
    struct dirent *ent;
    dir = opendir(presets_dir);
    if (!dir) return;
    while ((ent = readdir(dir)) != NULL) {
        char path[800];
        char bank_name[MAX_BANK_NAME];
        const char *name = ent->d_name;
        if (name[0] == '.') continue;
        if (!has_json_ext(name)) continue;
        if (strcmp(name, DEFAULT_PRESETS_FILE) == 0) continue;
        if (strcmp(name, USER_PRESETS_FILE) == 0) continue;
        snprintf(path, sizeof(path), "%s/%s", presets_dir, name);
        infer_bank_name_from_filename(name, bank_name, sizeof(bank_name));
        load_presets_from_file(inst, path, bank_name);
        if (inst->preset_count >= MAX_PRESETS) break;
    }
    closedir(dir);
}

static void load_presets(expchords_t *inst) {
    char presets_path[800];
    char user_path[800];
    char default_path[800];
    int i;

    inst->preset_count = 0;
    inst->bank_count = 0;
    inst->active_bank = 0;

    snprintf(default_path, sizeof(default_path), "%s/%s/%s",
             inst->module_dir, PRESETS_SUBDIR, DEFAULT_PRESETS_FILE);
    load_presets_from_file(inst, default_path, FACTORY_BANK_NAME);

    snprintf(presets_path, sizeof(presets_path), "%s/%s",
             inst->module_dir, PRESETS_SUBDIR);
    load_additional_preset_files(inst, presets_path);

    snprintf(user_path, sizeof(user_path), "%s/%s/%s",
             inst->module_dir, PRESETS_SUBDIR, USER_PRESETS_FILE);
    load_presets_from_file(inst, user_path, USER_BANK_NAME);

    if (inst->preset_count == 0) {
        /* Hardcoded fallback */
        inst->preset_count = 1;
        copy_cstr(inst->presets[0].name, MAX_PRESET_NAME, "Default");
        copy_cstr(inst->presets[0].bank, MAX_BANK_NAME, FACTORY_BANK_NAME);
        inst->presets[0].global_octave = GLOBAL_OCT_DEFAULT;
        inst->presets[0].global_transpose = GLOBAL_TRANSPOSE_DEFAULT;
        for (i = 0; i < PAD_COUNT; i++)
            inst->presets[0].slots[i] = default_slot();
        LOG("using fallback preset");
    }
    rebuild_banks(inst);
    LOG("preset count: %d, bank count: %d", inst->preset_count, inst->bank_count);
}

/* ── Directory creation (POSIX instead of system()) ──────────────────────── */
static void ensure_presets_dir(expchords_t *inst) {
    char path[800];
    int ret;
    snprintf(path, sizeof(path), "%s/%s", inst->module_dir, PRESETS_SUBDIR);
    ret = mkdir(path, 0755);
    if (ret != 0 && errno != EEXIST) {
        LOG("failed to create presets dir: %s", strerror(errno));
    } else {
        LOG("presets directory ready");
    }
}

static void save_presets(expchords_t *inst) {
    char user_path[800];
    int user_bank_idx;
    int first;
    int last;
    int i;
    ensure_presets_dir(inst);

    snprintf(user_path, sizeof(user_path), "%s/%s/%s",
             inst->module_dir, PRESETS_SUBDIR, USER_PRESETS_FILE);

    FILE *f = fopen(user_path, "w");
    if (!f) {
        LOG("failed to open presets file for writing");
        return;
    }
    
    fprintf(f, "[\n");
    user_bank_idx = find_bank_index_by_name(inst, USER_BANK_NAME);
    first = bank_first_preset(inst, user_bank_idx);
    last = bank_last_preset(inst, user_bank_idx);
    if (first < 0 || last < first) {
        fprintf(f, "]\n");
        fclose(f);
        LOG("no user presets to save");
        return;
    }

    for (i = first; i <= last; i++) {
        preset_t *pr = &inst->presets[i];
        fprintf(f, "  {\"name\":\"%s\",\"bank\":\"%s\",\"global_octave\":%d,\"global_transpose\":%d,\"pads\":[\n",
                pr->name, pr->bank, pr->global_octave, pr->global_transpose);
        for (int j = 0; j < PAD_COUNT; j++) {
            pad_slot_t *s = &pr->slots[j];
            const char *bass_name = BASS_NAMES[(s->bass >= 0 && s->bass < ROOT_COUNT) ? (s->bass + 1) : 0];
            fprintf(f, "    {\"octave\":%d,\"root\":\"%s\",\"bass\":\"%s\",\"chord_type\":\"%s\",\"inversion\":%d,"
                    "\"strum\":%d,\"strum_dir\":%d,"
                    "\"articulation\":%d,\"reverse_art\":%d}%s\n",
                    s->octave, ROOT_NAMES[s->root], bass_name, type_name(inst, s->type), s->inversion,
                    s->strum, s->strum_dir, s->articulation, s->reverse_art,
                    j < PAD_COUNT-1 ? "," : "");
        }
        fprintf(f, "  ]}%s\n", i < last ? "," : "");
    }
    fprintf(f, "]\n");
    
    fclose(f);
    LOG("presets saved successfully");
}

/* ── Load preset into active slots ──────────────────────────────────────── */
static void load_preset_into_slots(expchords_t *inst, int idx) {
    if (idx < 0 || idx >= inst->preset_count) return;
    
    inst->active_preset = idx;
    set_active_bank_from_preset(inst, idx);
    inst->active_global_octave = inst->presets[idx].global_octave;
    inst->active_global_transpose = inst->presets[idx].global_transpose;
    for (int i = 0; i < PAD_COUNT; i++)
        inst->active_pad_slots[i] = inst->presets[idx].slots[i];
    
    LOG("loaded preset %d: %s (%s)", idx, inst->presets[idx].name, inst->presets[idx].bank);
}

/* ── Custom chord type name helper ───────────────────────────────────── */
static const char *type_name(expchords_t *inst, int type_idx) {
    if (type_idx >= 0 && type_idx < TYPE_COUNT) return TYPE_NAMES[type_idx];
    int ci = type_idx - TYPE_COUNT;
    if (ci >= 0 && ci < inst->custom_chord_count)
        return inst->custom_chords[ci].name;
    return TYPE_NAMES[0]; /* fallback to "maj" */
}

/* ── Chord building ──────────────────────────────────────────────────────── */
static int build_chord(expchords_t *inst, pad_slot_t *s, int global_octave, int global_transpose, int input_note,
                       int out_notes[], int max_notes) {
    const chord_def_t *def;
    if (s->type >= 0 && s->type < TYPE_COUNT) {
        def = &CHORD_DEFS[s->type];
    } else {
        int ci = s->type - TYPE_COUNT;
        if (ci >= 0 && ci < inst->custom_chord_count)
            def = &inst->custom_chords[ci].def;
        else
            def = &CHORD_DEFS[0]; /* fallback to maj */
    }
    int count = def->count < max_notes ? def->count : max_notes;
    int notes[MAX_CHORD_NOTES];
    int i;

    for (i = 0; i < count; i++)
        notes[i] = input_note + global_transpose + ((global_octave + s->octave) * 12) + s->root + def->offs[i];

    /* Inversion: raise bottom N notes by octave */
    int inv = s->inversion < count ? s->inversion : count - 1;
    for (i = 0; i < inv; i++) notes[i] += 12;

    /* Sort ascending */
    for (i = 0; i < count - 1; i++) {
        int j;
        for (j = i+1; j < count; j++) {
            if (notes[j] < notes[i]) { int t=notes[i]; notes[i]=notes[j]; notes[j]=t; }
        }
    }

    /* Articulation: add octave above top */
    if (s->articulation && count < max_notes) {
        notes[count] = notes[count-1] + 12;
        count++;
    }

    /* Reverse articulation */
    if (s->reverse_art) {
        for (i = 0; i < count/2; i++) {
            int t = notes[i]; notes[i] = notes[count-1-i]; notes[count-1-i] = t;
        }
    }

    /* Strum direction: down = reverse order */
    if (s->strum_dir) {
        for (i = 0; i < count/2; i++) {
            int t = notes[i]; notes[i] = notes[count-1-i]; notes[count-1-i] = t;
        }
    }

    int out = 0;
    int lowest = 127;
    for (i = 0; i < count; i++)
        if (notes[i] >= 0 && notes[i] <= 127) {
            out_notes[out++] = notes[i];
            if (notes[i] < lowest) lowest = notes[i];
        }

    /* Slash bass: force selected bass note below the chord where possible. */
    if (s->bass >= 0 && s->bass < ROOT_COUNT && max_notes > 0) {
        int bass_note = input_note + global_transpose + ((global_octave + s->octave) * 12) + s->bass;
        while (bass_note >= lowest) bass_note -= 12;
        while (bass_note < 0) bass_note += 12;
        if (bass_note >= 0 && bass_note <= 127) {
            int exists = 0;
            for (i = 0; i < out; i++) {
                if (out_notes[i] == bass_note) { exists = 1; break; }
            }
            if (!exists) {
                if (out < max_notes) {
                    out_notes[out++] = bass_note;
                } else {
                    out_notes[0] = bass_note;
                }
                for (i = 0; i < out - 1; i++) {
                    int j;
                    for (j = i + 1; j < out; j++) {
                        if (out_notes[j] < out_notes[i]) {
                            int t = out_notes[i];
                            out_notes[i] = out_notes[j];
                            out_notes[j] = t;
                        }
                    }
                }
            }
        }
    }
    return out;
}

/* ── Plugin callbacks ────────────────────────────────────────────────────── */

/* Extract a JSON string value for a given key into dst (max dst_len-1 chars).
   Returns 1 on success, 0 if key not found. */
static int json_extract_string(const char *json, const char *key, char *dst, int dst_len) {
    if (!json || !key || !dst || dst_len <= 0) return 0;
    dst[0] = '\0';
    /* Build search pattern: "key":" */
    char pattern[64];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    if (plen <= 0 || plen >= (int)sizeof(pattern)) return 0;

    const char *ptr = strstr(json, pattern);
    if (!ptr) return 0;
    ptr += plen;
    int i = 0;
    while (*ptr && *ptr != '"' && i < dst_len - 1) {
        dst[i++] = *ptr++;
    }
    dst[i] = '\0';
    return 1;
}

/* Extract a JSON integer value for a given key.  Returns the value, or
   fallback if the key is not found. */
static int json_extract_int(const char *json, const char *key, int fallback) {
    if (!json || !key) return fallback;
    char pattern[64];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    if (plen <= 0 || plen >= (int)sizeof(pattern)) return fallback;

    const char *ptr = strstr(json, pattern);
    if (!ptr) return fallback;
    ptr += plen;
    while (*ptr == ' ' || *ptr == '\t') ptr++;
    int val = fallback;
    if (sscanf(ptr, "%d", &val) == 1) return val;
    return fallback;
}

/* Find a preset by name and bank.  Returns the index, or -1 if not found. */
static int find_preset_by_name(expchords_t *inst, const char *name, const char *bank) {
    if (!inst || !name || !name[0]) return -1;
    /* First pass: match both name and bank */
    if (bank && bank[0]) {
        for (int i = 0; i < inst->preset_count; i++) {
            if (strcmp(inst->presets[i].name, name) == 0 &&
                strcmp(inst->presets[i].bank, bank) == 0)
                return i;
        }
    }
    /* Second pass: match name only (bank may have been renamed/moved) */
    for (int i = 0; i < inst->preset_count; i++) {
        if (strcmp(inst->presets[i].name, name) == 0)
            return i;
    }
    return -1;
}

static void *create_instance(const char *module_dir, const char *config_json) {
    expchords_t *inst = calloc(1, sizeof(expchords_t));
    if (!inst) return NULL;

    memset(inst->held_out, -1, sizeof(inst->held_out));
    inst->current_pad  = 1;
    inst->active_preset = -1;
    inst->sample_rate  = 44100;
    if (module_dir)
        strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir)-1);

    load_custom_chords(inst);
    load_presets(inst);

    /* Try to restore preset from saved state, else load first preset */
    int preset_to_load = 0;
    if (config_json && inst->preset_count > 0) {
        /* First try to find by name + bank (robust across preset list changes) */
        char saved_name[MAX_PRESET_NAME];
        char saved_bank[MAX_BANK_NAME];
        if (json_extract_string(config_json, "preset_name", saved_name, sizeof(saved_name)) &&
            saved_name[0]) {
            json_extract_string(config_json, "bank", saved_bank, sizeof(saved_bank));
            int found = find_preset_by_name(inst, saved_name, saved_bank);
            if (found >= 0) {
                preset_to_load = found;
            } else {
                /* Name not found — fall back to saved index */
                int idx = json_extract_int(config_json, "active_preset", 0);
                if (idx < 0) idx = 0;
                if (idx >= inst->preset_count) idx = inst->preset_count - 1;
                preset_to_load = idx;
            }
        } else {
            /* No name saved — fall back to index (legacy state) */
            int idx = json_extract_int(config_json, "active_preset", 0);
            if (idx < 0) idx = 0;
            if (idx >= inst->preset_count) idx = inst->preset_count - 1;
            preset_to_load = idx;
        }
    }

    if (inst->preset_count > 0)
        load_preset_into_slots(inst, preset_to_load);

    LOG("create_instance called, loaded preset %d", preset_to_load);
    return inst;
}

static void destroy_instance(void *instance) {
    free(instance);
}

static int process_midi(void *instance,
                        const uint8_t *in_msg, int in_len,
                        uint8_t out_msgs[][3], int out_lens[], int max_out) {
    expchords_t *inst = (expchords_t*)instance;
    if (!inst || in_len < 3) return 0;

    uint8_t status = in_msg[0] & 0xF0;
    uint8_t ch     = in_msg[0] & 0x0F;
    uint8_t note   = in_msg[1];
    uint8_t vel    = in_msg[2];
    int pad = note_to_pad(note);
    int is_on  = (status == 0x90) && vel > 0;
    int is_off = (status == 0x80) || (status == 0x90 && vel == 0);

    /* Pad press: switch active pad */
    if (is_on) {
        if (pad > 0) {
            inst->current_pad = pad;
            LOG("pad pressed: %d", inst->current_pad);
        }
    }

    if (is_on) {
        /* If this input note already has a chord, don't retrigger it */
        if (inst->held_out_count[note] > 0) {
            LOG("midi on: note=%d already held, ignoring retrigger", note);
            return 0;
        }

        int pad_idx = inst->current_pad - 1;
        pad_slot_t *s = &inst->active_pad_slots[pad_idx];
        int chord_notes[MAX_CHORD_NOTES];
        /* Drum-pad triggers are buttons; keep pitch independent from trigger note. */
        int input_pitch_note = (pad > 0) ? PAD_TRIGGER_BASE_NOTE : note;
        int count = build_chord(inst, s, inst->active_global_octave, inst->active_global_transpose, input_pitch_note, chord_notes, MAX_CHORD_NOTES);
        if (count > MAX_CHORD_NOTES) count = MAX_CHORD_NOTES;

        /* Strum timing */
        int strum_frames = 0;
        if (s->strum > 0 && inst->sample_rate > 0 && count > 1)
            strum_frames = (s->strum * inst->sample_rate) / (1000 * (count - 1));

        int out = 0, i;
        for (i = 0; i < count; i++) {
            int n = chord_notes[i];
            if (strum_frames > 0 && i > 0) {
                /* Always queue strum-delayed notes to pending */
                if (inst->pending_count < MAX_PENDING) {
                    inst->pending[inst->pending_count++] = (pending_note_t){
                        (uint8_t)n, vel, ch, strum_frames * i
                    };
                }
            } else if (out < max_out) {
                /* Send immediate notes if buffer space available */
                out_msgs[out][0] = 0x90 | ch;
                out_msgs[out][1] = (uint8_t)n;
                out_msgs[out][2] = vel;
                out_lens[out++]  = 3;
            } else {
                /* Queue to pending if output buffer full */
                if (inst->pending_count < MAX_PENDING) {
                    inst->pending[inst->pending_count++] = (pending_note_t){
                        (uint8_t)n, vel, ch, 0
                    };
                }
            }
            if (i < MAX_CHORD_NOTES) inst->held_out[note][i] = (int8_t)n;
        }
        inst->held_out_count[note] = count;

        LOG("midi on: note=%d vel=%d chords=%d out=%d pending=%d", note, vel, count, out, inst->pending_count);
        return out;
    }

    if (is_off) {
        int count = inst->held_out_count[note];
        if (count == 0) {
            memcpy(out_msgs[0], in_msg, 3);
            out_lens[0]=3;
            return 1;
        }

        if (count > MAX_CHORD_NOTES) count = MAX_CHORD_NOTES;

        int out = 0, i;
        for (i = 0; i < count && out < max_out; i++) {
            int n = (int)inst->held_out[note][i];
            if (n < 0 || n > 127) continue;
            out_msgs[out][0] = 0x80 | ch;
            out_msgs[out][1] = (uint8_t)n;
            out_msgs[out][2] = 0;
            out_lens[out++]  = 3;
        }

        memset(inst->held_out[note], -1, sizeof(inst->held_out[note]));
        inst->held_out_count[note] = 0;

        LOG("midi off: note=%d chords_off=%d", note, out);
        return out;
    }

    /* Pass through all other MIDI */
    if (in_len <= 3) { 
        memcpy(out_msgs[0], in_msg, in_len); 
        out_lens[0]=in_len; 
        return 1; 
    }
    
    LOG("pass_through: len=%d", in_len);
    return 0;
}

static int tick_fn(void *instance, int frames, int sample_rate,
                   uint8_t out_msgs[][3], int out_lens[], int max_out) {
    expchords_t *inst = (expchords_t*)instance;
    if (!inst || inst->pending_count == 0) return 0;
    
    inst->sample_rate = sample_rate;
    int out = 0, remaining = 0, i;
    
    for (i = 0; i < inst->pending_count; i++) {
        pending_note_t *pn = &inst->pending[i];
        pn->delay_frames -= frames;
        if (pn->delay_frames <= 0 && out < max_out) {
            out_msgs[out][0] = 0x90 | pn->ch;
            out_msgs[out][1] = pn->note;
            out_msgs[out][2] = pn->vel;
            out_lens[out++]  = 3;
        } else {
            inst->pending[remaining++] = *pn;
        }
    }
    
    inst->pending_count = remaining;
    LOG("tick: pending=%d output=%d", inst->pending_count, out);
    return out;
}

static void set_param(void *instance, const char *key, const char *val) {
    expchords_t *inst = (expchords_t*)instance;
    if (!inst || !key || !val) return;

    int pad_idx = inst->current_pad - 1;
    pad_slot_t *s = &inst->active_pad_slots[pad_idx];

    LOG("set_param: key=%s val=%s", key, val);

    if (strcmp(key, "preset") == 0) {
        int idx = atoi(val);
        if (idx >= 0 && idx < inst->preset_count)
            load_preset_into_slots(inst, idx);
        return;
    }

    if (strcmp(key, "bank") == 0) {
        int b = atoi(val);
        int first;
        if (b < 0 || b >= inst->bank_count) return;
        first = bank_first_preset(inst, b);
        if (first >= 0) load_preset_into_slots(inst, first);
        return;
    }
    
    if (strcmp(key, "pad") == 0) {
        int v = atoi(val);
        if (v >= 1 && v <= PAD_COUNT) {
            inst->current_pad = v;
            /* s pointer is now stale — update it so subsequent writes go to correct pad */
            s = &inst->active_pad_slots[inst->current_pad - 1];
        }
        return;
    }
    
    if (strcmp(key, "save") == 0) {
        /* val is preset name to save as; empty/save = auto-name */
        char name[MAX_PRESET_NAME];
        int user_bank_idx;
        int first;
        int last;
        if (strlen(val) == 0 || strcmp(val,"save") == 0 || strcmp(val,"1") == 0)
            snprintf(name, sizeof(name), "Preset %d", inst->preset_count + 1);
        else
            strncpy(name, val, MAX_PRESET_NAME - 1);
        
        name[MAX_PRESET_NAME-1] = '\0';

        /* Find existing user preset by name or append to User bank */
        user_bank_idx = find_bank_index_by_name(inst, USER_BANK_NAME);
        first = bank_first_preset(inst, user_bank_idx);
        last = bank_last_preset(inst, user_bank_idx);

        int target = -1;
        for (int i = first; i >= 0 && i <= last; i++)
            if (strcmp(inst->presets[i].name, name) == 0) {
                target = i; 
                break; 
            }
        
        if (target < 0) {
            if (inst->preset_count >= MAX_PRESETS) return;
            target = (last >= 0) ? (last + 1) : inst->preset_count;
            for (int i = inst->preset_count; i > target; i--)
                inst->presets[i] = inst->presets[i - 1];
            inst->preset_count++;
        }
        
        strncpy(inst->presets[target].name, name, MAX_PRESET_NAME-1);
        copy_cstr(inst->presets[target].bank, MAX_BANK_NAME, USER_BANK_NAME);
        inst->presets[target].global_octave = inst->active_global_octave;
        inst->presets[target].global_transpose = inst->active_global_transpose;
        for (int i = 0; i < PAD_COUNT; i++)
            inst->presets[target].slots[i] = inst->active_pad_slots[i];

        rebuild_banks(inst);
        load_preset_into_slots(inst, target);
        save_presets(inst);
        return;
    }

    if (strcmp(key, "reset_patch") == 0) {
        reset_active_patch(inst);
        LOG("reset_patch: active pad set reset to C major defaults");
        return;
    }

    if (strcmp(key, "state") == 0) {
        /* Restore preset by name+bank (robust), fall back to index */
        char preset_name[MAX_PRESET_NAME];
        char bank_name[MAX_BANK_NAME];
        json_extract_string(val, "preset_name", preset_name, sizeof(preset_name));
        json_extract_string(val, "bank", bank_name, sizeof(bank_name));

        LOG("set_param state: received state with preset_name='%s' bank='%s'", preset_name, bank_name);

        int idx = -1;
        if (preset_name[0] != '\0') {
            idx = find_preset_by_name(inst, preset_name, bank_name);
            LOG("set_param state: find_preset_by_name returned idx=%d", idx);
        }

        if (idx < 0) {
            idx = json_extract_int(val, "active_preset", -1);
            LOG("set_param state: fallback to active_preset index=%d", idx);
            if (idx < 0 || idx >= inst->preset_count) idx = -1;
        }

        if (idx >= 0) {
            load_preset_into_slots(inst, idx);
            LOG("set_param state: loaded preset idx=%d", idx);
        } else {
            LOG("set_param state: no valid preset found, keeping current");
        }

        /* Restore editing state (pad, global tweaks) */
        int pad = json_extract_int(val, "current_pad", 1);
        inst->current_pad = clamp_i(pad, 1, PAD_COUNT);
        inst->active_global_octave = clamp_i(
            json_extract_int(val, "global_octave", inst->active_global_octave),
            OCT_MIN, OCT_MAX);
        inst->active_global_transpose = clamp_i(
            json_extract_int(val, "global_transpose", inst->active_global_transpose),
            TRANSPOSE_MIN, TRANSPOSE_MAX);

        LOG("set_param state: restored pad=%d octave=%d transpose=%d", inst->current_pad, inst->active_global_octave, inst->active_global_transpose);
        return;
    }

    /* Per-pad params */
    if (strcmp(key,"global_octave")==0) inst->active_global_octave = clamp_i(atoi(val), OCT_MIN, OCT_MAX);
    else if (strcmp(key,"global_transpose")==0) inst->active_global_transpose = clamp_i(atoi(val), TRANSPOSE_MIN, TRANSPOSE_MAX);
    else if (strcmp(key,"pad_octave")==0) s->octave      = clamp_i(atoi(val), OCT_MIN, OCT_MAX);
    else if (strcmp(key,"root")==0)     s->root         = parse_enum(ROOT_NAMES,ROOT_COUNT,val);
    else if (strcmp(key,"bass")==0) {
        int v = parse_enum(BASS_NAMES, BASS_COUNT, val);
        s->bass = (v >= 0) ? (v - 1) : -1;
    }
    else if (strcmp(key,"chord_type")==0) {
        int v = find_enum(TYPE_NAMES, TYPE_COUNT, val);
        if (v >= 0) {
            s->type = v;
        } else {
            /* Check custom chord names */
            int found = 0;
            for (int i = 0; i < inst->custom_chord_count; i++) {
                if (strcmp(val, inst->custom_chords[i].name) == 0) {
                    s->type = TYPE_COUNT + i;
                    found = 1;
                    break;
                }
            }
            if (!found) s->type = 0; /* fallback to maj */
        }
    }
    else if (strcmp(key,"inversion")==0)s->inversion    = parse_enum(INV_NAMES,INV_COUNT,val);
    else if (strcmp(key,"strum")==0)    s->strum        = clamp_i(atoi(val),0,100);
    else if (strcmp(key,"strum_dir")==0)s->strum_dir    = parse_enum(DIR_NAMES,DIR_COUNT,val);
    else if (strcmp(key,"articulation")==0) s->articulation = parse_enum(ART_NAMES,ART_COUNT,val);
    else if (strcmp(key,"reverse_art")==0)  s->reverse_art  = parse_enum(ART_NAMES,ART_COUNT,val);
}

static int get_param(void *instance, const char *key, char *buf, int buf_len) {
    expchords_t *inst = (expchords_t*)instance;
    if (!inst || !key || !buf || buf_len <= 0) return -1;

    int pad_idx = inst->current_pad - 1;
    pad_slot_t *s = &inst->active_pad_slots[pad_idx];

    if (strcmp(key,"name")==0 || strcmp(key,"display_name")==0)
        return snprintf(buf,buf_len,"Chord Flow");
    if (strcmp(key,"bank_count")==0)
        return snprintf(buf,buf_len,"%d",inst->bank_count);
    if (strcmp(key,"bank")==0)
        return snprintf(buf,buf_len,"%d",inst->active_bank);
    if (strcmp(key,"bank_name")==0) {
        if (inst->active_bank >= 0 && inst->active_bank < inst->bank_count)
            return snprintf(buf,buf_len,"%s",inst->banks[inst->active_bank].name);
        return snprintf(buf,buf_len,"");
    }
    if (strcmp(key,"bank_preset")==0) {
        int first = bank_first_preset(inst, inst->active_bank);
        if (first >= 0 && inst->active_preset >= first)
            return snprintf(buf,buf_len,"%d",inst->active_preset - first);
        return snprintf(buf,buf_len,"0");
    }
    if (strcmp(key,"bank_preset_count")==0) {
        if (inst->active_bank >= 0 && inst->active_bank < inst->bank_count)
            return snprintf(buf,buf_len,"%d",inst->banks[inst->active_bank].count);
        return snprintf(buf,buf_len,"0");
    }
    if (strcmp(key,"custom_chord_count")==0)
        return snprintf(buf,buf_len,"%d",inst->custom_chord_count);
    if (strcmp(key,"custom_chord_names")==0) {
        int pos = 0;
        for (int i = 0; i < inst->custom_chord_count && pos < buf_len - 1; i++) {
            if (i > 0 && pos < buf_len - 1) buf[pos++] = ',';
            int n = snprintf(buf + pos, buf_len - pos, "%s", inst->custom_chords[i].name);
            if (n > 0) pos += n;
        }
        return pos;
    }
    if (strcmp(key,"_debug_module_dir")==0)
        return snprintf(buf,buf_len,"%s",inst->module_dir);
    if (strcmp(key,"_debug_custom_chords_path")==0)
        return snprintf(buf,buf_len,"%s",inst->custom_chords_dir);
    if (strcmp(key,"preset_count")==0)
        return snprintf(buf,buf_len,"%d",inst->preset_count);
    if (strcmp(key,"preset")==0)
        return snprintf(buf,buf_len,"%d",inst->active_preset);
    if (strcmp(key,"preset_name")==0) {
        int idx = inst->active_preset;
        if (idx >= 0 && idx < inst->preset_count)
            return snprintf(buf,buf_len,"%s",inst->presets[idx].name);
        return snprintf(buf,buf_len,"---");
    }
    if (strcmp(key,"preset_list")==0) {
        /* JSON array of preset names for UI browser */
        int pos = 0;
        pos += snprintf(buf+pos, buf_len-pos, "[");
        for (int i = 0; i < inst->preset_count && pos < buf_len-4; i++) {
            if (i > 0) pos += snprintf(buf+pos, buf_len-pos, ",");
            pos += snprintf(buf+pos, buf_len-pos, "\"%s\"", inst->presets[i].name);
        }
        pos += snprintf(buf+pos, buf_len-pos, "]");
        return pos;
    }
    if (strcmp(key,"pad")==0)
        return snprintf(buf,buf_len,"%d",inst->current_pad);
    if (strcmp(key,"global_octave")==0)
        return snprintf(buf,buf_len,"%d",inst->active_global_octave);
    if (strcmp(key,"global_transpose")==0)
        return snprintf(buf,buf_len,"%d",inst->active_global_transpose);
    if (strcmp(key,"pad_octave")==0)
        return snprintf(buf,buf_len,"%d",s->octave);
    if (strcmp(key,"root")==0)
        return snprintf(buf,buf_len,"%s",ROOT_NAMES[s->root]);
    if (strcmp(key,"bass")==0)
        return snprintf(buf,buf_len,"%s", BASS_NAMES[(s->bass >= 0 && s->bass < ROOT_COUNT) ? (s->bass + 1) : 0]);
    if (strcmp(key,"chord_type")==0)
        return snprintf(buf,buf_len,"%s",type_name(inst, s->type));
    if (strcmp(key,"inversion")==0)
        return snprintf(buf,buf_len,"%s",INV_NAMES[s->inversion]);
    if (strcmp(key,"strum")==0)
        return snprintf(buf,buf_len,"%d",s->strum);
    if (strcmp(key,"strum_dir")==0)
        return snprintf(buf,buf_len,"%s",DIR_NAMES[s->strum_dir]);
    if (strcmp(key,"articulation")==0)
        return snprintf(buf,buf_len,"%s",ART_NAMES[s->articulation]);
    if (strcmp(key,"reverse_art")==0)
        return snprintf(buf,buf_len,"%s",ART_NAMES[s->reverse_art]);
    
    /* pad_label: same as preset_name but with trailing spaces encoding the pad
       number — invisible to the user but changes value on pad press so the
       Shadow UI detects the change and redraws all params */
    if (strcmp(key,"pad_label")==0) {
        int idx = inst->active_preset;
        const char *pname = (idx >= 0 && idx < inst->preset_count)
                            ? inst->presets[idx].name : "---";
        /* Append pad number of spaces (1-32) — invisible but unique per pad */
        int n = snprintf(buf, buf_len, "%s", pname);
        int i;
        for (i = 0; i < inst->current_pad && n < buf_len - 1; i++)
            buf[n++] = ' ';
        buf[n] = '\0';
        return n;
    }
    if (strcmp(key,"state")==0) {
        const char *preset_name = (inst->active_preset >= 0 && inst->active_preset < inst->preset_count)
            ? inst->presets[inst->active_preset].name
            : "";
        return snprintf(buf,buf_len,
            "{\"active_preset\":%d,\"preset_name\":\"%s\",\"bank\":\"%s\",\"current_pad\":%d,\"global_octave\":%d,\"global_transpose\":%d,\"pad_octave\":%d,"
            "\"root\":\"%s\",\"bass\":\"%s\",\"chord_type\":\"%s\","
            "\"inversion\":\"%s\",\"strum\":%d,"
            "\"strum_dir\":\"%s\",\"articulation\":\"%s\","
            "\"reverse_art\":\"%s\"}",
            inst->active_preset,
            preset_name,
            (inst->active_bank >= 0 && inst->active_bank < inst->bank_count) ? inst->banks[inst->active_bank].name : "",
            inst->current_pad, inst->active_global_octave, inst->active_global_transpose, s->octave,
            ROOT_NAMES[s->root], BASS_NAMES[(s->bass >= 0 && s->bass < ROOT_COUNT) ? (s->bass + 1) : 0], type_name(inst, s->type),
            INV_NAMES[s->inversion], s->strum,
            DIR_NAMES[s->strum_dir], ART_NAMES[s->articulation],
            ART_NAMES[s->reverse_art]);
    }

    return -1;
}

/* ── Entry point (per midi_fx_api_v1 spec) ──────────────────────────────── */
static midi_fx_api_v1_t g_api = {
    .api_version      = MIDI_FX_API_VERSION,
    .create_instance  = create_instance,
    .destroy_instance = destroy_instance,
    .process_midi     = process_midi,
    .tick             = tick_fn,              /* Per spec: must be named 'tick' */
    .set_param        = set_param,
    .get_param        = get_param,
};

midi_fx_api_v1_t* move_midi_fx_init(const host_api_v1_t *host) {
    (void)host;
    return &g_api;
}
