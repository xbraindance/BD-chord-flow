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

static void get_str_param(midi_fx_api_v1_t *api, void *inst, const char *key, char *dst, int dst_len) {
    memset(dst, 0, (size_t)dst_len);
    if (api->get_param(inst, key, dst, dst_len) <= 0) fail("get_param str failed");
}

int main(void) {
    host_api_v1_t host;
    midi_fx_api_v1_t *api;
    void *inst;
    void *inst2;
    char tmp_template[] = "/tmp/chordflow-state-XXXXXX";
    char *tmp_dir;
    char presets_dir[1024];
    char file_path[1024];
    char state[1024];
    char preset_name[128];

    memset(&host, 0, sizeof(host));
    host.api_version = MOVE_PLUGIN_API_VERSION;

    tmp_dir = mkdtemp(tmp_template);
    if (!tmp_dir) fail("mkdtemp failed");

    snprintf(presets_dir, sizeof(presets_dir), "%s/presets", tmp_dir);
    if (mkdir(presets_dir, 0755) != 0) fail("mkdir presets failed");

    snprintf(file_path, sizeof(file_path), "%s/fmc_test.json", presets_dir);
    write_text_file(
        file_path,
        "["
        "{\"name\":\"Alpha\",\"bank\":\"Bank A\",\"global_octave\":2,\"global_transpose\":0,"
        "\"pads\":[{\"octave\":0,\"root\":\"c\",\"bass\":\"none\",\"chord_type\":\"maj\",\"inversion\":0,\"strum\":0,\"strum_dir\":0,\"articulation\":0,\"reverse_art\":0}]},"
        "{\"name\":\"Beta\",\"bank\":\"Bank B\",\"global_octave\":2,\"global_transpose\":0,"
        "\"pads\":[{\"octave\":0,\"root\":\"d\",\"bass\":\"none\",\"chord_type\":\"min\",\"inversion\":0,\"strum\":0,\"strum_dir\":0,\"articulation\":0,\"reverse_art\":0}]}"
        "]"
    );

    api = move_midi_fx_init(&host);
    if (!api || !api->create_instance || !api->destroy_instance || !api->set_param || !api->get_param) {
        fail("API callbacks missing");
    }

    inst = api->create_instance(tmp_dir, NULL);
    if (!inst) fail("create_instance failed");

    api->set_param(inst, "preset", "1");
    get_str_param(api, inst, "preset_name", preset_name, sizeof(preset_name));
    if (strcmp(preset_name, "Beta") != 0) {
        fprintf(stderr, "FAIL: expected preset_name Beta before snapshot, got %s\n", preset_name);
        return 1;
    }

    get_str_param(api, inst, "state", state, sizeof(state));
    api->destroy_instance(inst);

    inst2 = api->create_instance(tmp_dir, NULL);
    if (!inst2) fail("second create_instance failed");

    /* Fresh instance should start from first preset. */
    get_str_param(api, inst2, "preset_name", preset_name, sizeof(preset_name));
    if (strcmp(preset_name, "Alpha") != 0) {
        fprintf(stderr, "FAIL: expected preset_name Alpha on fresh instance, got %s\n", preset_name);
        return 1;
    }

    /* Applying state should restore Beta. */
    api->set_param(inst2, "state", state);
    get_str_param(api, inst2, "preset_name", preset_name, sizeof(preset_name));
    if (strcmp(preset_name, "Beta") != 0) {
        fprintf(stderr, "FAIL: expected preset_name Beta after state restore, got %s\n", preset_name);
        return 1;
    }

    api->destroy_instance(inst2);
    printf("PASS: chordflow state restore\n");
    return 0;
}
