#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

extern midi_fx_api_v1_t* move_midi_fx_init(const host_api_v1_t *host);

static void fail(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

static void write_text_file(const char *path, const char *contents) {
    FILE *f = fopen(path, "w");
    if (!f) fail("failed to open file for write");
    if (fputs(contents, f) == EOF) {
        fclose(f);
        fail("failed to write file");
    }
    fclose(f);
}

static int get_int_param(midi_fx_api_v1_t *api, void *inst, const char *key) {
    char buf[64];
    memset(buf, 0, sizeof(buf));
    if (api->get_param(inst, key, buf, sizeof(buf)) <= 0) fail("get_param int failed");
    return atoi(buf);
}

static void get_str_param(midi_fx_api_v1_t *api, void *inst, const char *key, char *dst, int dst_len) {
    memset(dst, 0, (size_t)dst_len);
    if (api->get_param(inst, key, dst, dst_len) <= 0) fail("get_param str failed");
}

int main(void) {
    host_api_v1_t host;
    midi_fx_api_v1_t *api;
    void *inst;
    char tmp_template[] = "/tmp/chordflow-multi-XXXXXX";
    char *tmp_dir;
    char presets_dir[1024];
    char extra_path[1024];
    char extra2_path[1024];
    char legacy_default_path[1024];
    char preset_name[128];
    char bank_name[128];

    memset(&host, 0, sizeof(host));
    host.api_version = MOVE_PLUGIN_API_VERSION;

    tmp_dir = mkdtemp(tmp_template);
    if (!tmp_dir) fail("mkdtemp failed");

    snprintf(presets_dir, sizeof(presets_dir), "%s/presets", tmp_dir);
    if (mkdir(presets_dir, 0755) != 0) fail("mkdir presets failed");

    snprintf(extra_path, sizeof(extra_path), "%s/more.json", presets_dir);
    write_text_file(
        extra_path,
        "[{\"name\":\"Zeta One\",\"bank\":\"Zeta Bank\",\"global_octave\":2,\"global_transpose\":0,"
        "\"pads\":[{\"octave\":0,\"root\":\"d\",\"bass\":\"none\",\"chord_type\":\"min\",\"inversion\":0,"
        "\"strum\":0,\"strum_dir\":0,\"articulation\":0,\"reverse_art\":0}]}]"
    );
    snprintf(extra2_path, sizeof(extra2_path), "%s/aaa.json", presets_dir);
    write_text_file(
        extra2_path,
        "[{\"name\":\"Alpha One\",\"bank\":\"Alpha Bank\",\"global_octave\":2,\"global_transpose\":0,"
        "\"pads\":[{\"octave\":0,\"root\":\"e\",\"bass\":\"none\",\"chord_type\":\"min\",\"inversion\":0,"
        "\"strum\":0,\"strum_dir\":0,\"articulation\":0,\"reverse_art\":0}]}]"
    );
    /* Simulate stale legacy file left behind by overlay installs.
       The runtime should ignore this reserved file to avoid showing old
       Factory banks after upgrading to FMC-only releases. */
    snprintf(legacy_default_path, sizeof(legacy_default_path), "%s/default.json", presets_dir);
    write_text_file(
        legacy_default_path,
        "[{\"name\":\"Legacy Factory\",\"bank\":\"Factory\",\"global_octave\":2,\"global_transpose\":0,"
        "\"pads\":[{\"octave\":0,\"root\":\"c\",\"bass\":\"none\",\"chord_type\":\"maj\",\"inversion\":0,"
        "\"strum\":0,\"strum_dir\":0,\"articulation\":0,\"reverse_art\":0}]}]"
    );

    api = move_midi_fx_init(&host);
    if (!api || !api->create_instance || !api->destroy_instance || !api->set_param || !api->get_param) {
        fail("API callbacks missing");
    }

    inst = api->create_instance(tmp_dir, NULL);
    if (!inst) fail("create_instance failed");

    if (get_int_param(api, inst, "preset_count") != 2) {
        fprintf(stderr, "FAIL: expected preset_count 2 (legacy default ignored), got %d\n", get_int_param(api, inst, "preset_count"));
        return 1;
    }
    if (get_int_param(api, inst, "bank_count") != 2) {
        fprintf(stderr, "FAIL: expected bank_count 2, got %d\n", get_int_param(api, inst, "bank_count"));
        return 1;
    }

    api->set_param(inst, "preset", "0");
    get_str_param(api, inst, "preset_name", preset_name, sizeof(preset_name));
    if (strcmp(preset_name, "Alpha One") != 0) {
        fprintf(stderr, "FAIL: expected preset_name Alpha One, got %s\n", preset_name);
        return 1;
    }

    api->set_param(inst, "bank", "0");
    get_str_param(api, inst, "bank_name", bank_name, sizeof(bank_name));
    if (strcmp(bank_name, "Alpha Bank") != 0) {
        fprintf(stderr, "FAIL: expected bank_name Alpha Bank, got %s\n", bank_name);
        return 1;
    }

    api->set_param(inst, "bank", "1");
    get_str_param(api, inst, "bank_name", bank_name, sizeof(bank_name));
    if (strcmp(bank_name, "Zeta Bank") != 0) {
        fprintf(stderr, "FAIL: expected bank_name Zeta Bank, got %s\n", bank_name);
        return 1;
    }

    api->destroy_instance(inst);
    printf("PASS: chordflow multi json load\n");
    return 0;
}
