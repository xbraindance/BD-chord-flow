#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

extern midi_fx_api_v1_t* move_midi_fx_init(const host_api_v1_t *host);

static void fail(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

static int get_int_param(midi_fx_api_v1_t *api, void *inst, const char *key) {
    char buf[64];
    memset(buf, 0, sizeof(buf));
    if (api->get_param(inst, key, buf, sizeof(buf)) <= 0) fail("get_param failed");
    return atoi(buf);
}

static void expect_int_param(midi_fx_api_v1_t *api, void *inst, const char *key, int expected) {
    int got = get_int_param(api, inst, key);
    if (got != expected) {
        fprintf(stderr, "FAIL: %s expected %d got %d\n", key, expected, got);
        exit(1);
    }
}

static int send_note_on_get_first_note(midi_fx_api_v1_t *api, void *inst, int note, int vel) {
    uint8_t in[3] = {0x90, (uint8_t)note, (uint8_t)vel};
    uint8_t out_msgs[16][3];
    int out_lens[16];
    int count = api->process_midi(inst, in, 3, out_msgs, out_lens, 16);
    if (count <= 0) fail("no midi output for note on");
    return (int)out_msgs[0][1];
}

static void expect_str_param(midi_fx_api_v1_t *api, void *inst, const char *key, const char *expected) {
    char buf[128];
    memset(buf, 0, sizeof(buf));
    if (api->get_param(inst, key, buf, sizeof(buf)) <= 0) fail("get_param string failed");
    if (strcmp(buf, expected) != 0) {
        fprintf(stderr, "FAIL: %s expected \"%s\" got \"%s\"\n", key, expected, buf);
        exit(1);
    }
}

int main(void) {
    host_api_v1_t host;
    midi_fx_api_v1_t *api;
    void *inst;
    int base_note;
    int transposed;
    int semitone_transposed;
    int stacked;
    int before_count;
    int saved_idx;

    memset(&host, 0, sizeof(host));
    host.api_version = MOVE_PLUGIN_API_VERSION;

    api = move_midi_fx_init(&host);
    if (!api || !api->create_instance || !api->destroy_instance || !api->set_param || !api->get_param || !api->process_midi) {
        fail("API callbacks missing");
    }

    inst = api->create_instance(".", NULL);
    if (!inst) fail("create_instance failed");

    expect_str_param(api, inst, "name", "Chord Flow");
    expect_str_param(api, inst, "display_name", "Chord Flow");

    /* New baseline default: +2 octaves */
    expect_int_param(api, inst, "global_octave", 2);
    expect_int_param(api, inst, "global_transpose", 0);

    /* Expanded octave ranges clamp to -6..6 */
    api->set_param(inst, "global_octave", "99");
    expect_int_param(api, inst, "global_octave", 6);
    api->set_param(inst, "global_octave", "-99");
    expect_int_param(api, inst, "global_octave", -6);
    api->set_param(inst, "pad_octave", "99");
    expect_int_param(api, inst, "pad_octave", 6);
    api->set_param(inst, "pad_octave", "-99");
    expect_int_param(api, inst, "pad_octave", -6);
    api->set_param(inst, "global_transpose", "99");
    expect_int_param(api, inst, "global_transpose", 12);
    api->set_param(inst, "global_transpose", "-99");
    expect_int_param(api, inst, "global_transpose", -12);

    api->set_param(inst, "pad", "1");
    api->set_param(inst, "global_octave", "0");
    api->set_param(inst, "global_transpose", "0");
    api->set_param(inst, "pad_octave", "0");
    api->set_param(inst, "root", "c");
    api->set_param(inst, "chord_type", "5th");

    base_note = send_note_on_get_first_note(api, inst, 60, 100);

    api->set_param(inst, "global_transpose", "7");
    semitone_transposed = send_note_on_get_first_note(api, inst, 60, 100);
    if (semitone_transposed - base_note != 7) {
        fprintf(stderr, "FAIL: global_transpose expected +7 semitones got %d\n", semitone_transposed - base_note);
        return 1;
    }
    api->set_param(inst, "global_transpose", "0");

    api->set_param(inst, "global_octave", "1");
    transposed = send_note_on_get_first_note(api, inst, 60, 100);
    if (transposed - base_note != 12) {
        fprintf(stderr, "FAIL: global_octave expected +12 semitones got %d\n", transposed - base_note);
        return 1;
    }

    api->set_param(inst, "pad_octave", "-1");
    stacked = send_note_on_get_first_note(api, inst, 60, 100);
    if (stacked != base_note) {
        fprintf(stderr, "FAIL: stacked octave expected %d got %d\n", base_note, stacked);
        return 1;
    }

    before_count = get_int_param(api, inst, "preset_count");
    api->set_param(inst, "pad", "5");
    api->set_param(inst, "pad_octave", "5");
    api->set_param(inst, "global_octave", "-4");
    api->set_param(inst, "global_transpose", "-3");
    api->set_param(inst, "save", "save");

    saved_idx = get_int_param(api, inst, "preset");
    if (saved_idx != before_count) {
        fprintf(stderr, "FAIL: saved preset index expected %d got %d\n", before_count, saved_idx);
        return 1;
    }

    api->set_param(inst, "global_octave", "0");
    api->set_param(inst, "global_transpose", "0");
    api->set_param(inst, "pad_octave", "0");
    api->set_param(inst, "preset", "0");

    {
        char idx_buf[16];
        snprintf(idx_buf, sizeof(idx_buf), "%d", saved_idx);
        api->set_param(inst, "preset", idx_buf);
    }

    api->set_param(inst, "pad", "5");
    expect_int_param(api, inst, "global_octave", -4);
    expect_int_param(api, inst, "global_transpose", -3);
    expect_int_param(api, inst, "pad_octave", 5);

    /* Reset patch should reset all pads to C major root/no bass defaults. */
    api->set_param(inst, "pad", "1");
    api->set_param(inst, "root", "g");
    api->set_param(inst, "chord_type", "min7");
    api->set_param(inst, "inversion", "2nd");
    api->set_param(inst, "bass", "d");
    api->set_param(inst, "pad", "2");
    api->set_param(inst, "root", "a");
    api->set_param(inst, "chord_type", "dom7");
    api->set_param(inst, "inversion", "1st");
    api->set_param(inst, "bass", "e");
    api->set_param(inst, "reset_patch", "1");

    api->set_param(inst, "pad", "1");
    expect_str_param(api, inst, "root", "c");
    expect_str_param(api, inst, "chord_type", "maj");
    expect_str_param(api, inst, "inversion", "root");
    expect_str_param(api, inst, "bass", "none");

    api->set_param(inst, "pad", "2");
    expect_str_param(api, inst, "root", "c");
    expect_str_param(api, inst, "chord_type", "maj");
    expect_str_param(api, inst, "inversion", "root");
    expect_str_param(api, inst, "bass", "none");

    api->destroy_instance(inst);
    printf("PASS: chordflow octave transpose and persistence\n");
    return 0;
}
