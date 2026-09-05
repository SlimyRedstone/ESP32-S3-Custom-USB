#include "jsoncmd.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* Serialise @p root into @p out, then dispose of it either way. */
static bool emit(cJSON *root, char *out, size_t out_size)
{
    if (root == NULL) {
        return false;
    }

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (text == NULL) {
        return false;
    }

    size_t len = strlen(text);
    if (len + 1 > out_size) {
        fprintf(stderr, "command is too long, max %u bytes\n",
                (unsigned)out_size - 1);
        cJSON_free(text);
        return false;
    }

    memcpy(out, text, len + 1);
    cJSON_free(text);
    return true;
}

/* {"<outer>":{"<key>":"<value>"}} */
static cJSON *nested_string(const char *outer, const char *key, const char *value)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON *inner = cJSON_AddObjectToObject(root, outer);
    if (inner == NULL || cJSON_AddStringToObject(inner, key, value) == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

void jsoncmd_usage(FILE *f)
{
    fprintf(f,
        "usage:\n"
        "  led ABCDEF          set the NeoPixel\n"
        "  message words...    print on the CDC serial port\n"
        "  get led|config      read a value back\n"
        "  {\"get\":\"led\"}       send raw JSON verbatim\n");
}

bool jsoncmd_build(int argc, char **argv, char *out, size_t out_size)
{
    if (argc < 2 || out_size == 0) {
        jsoncmd_usage(stderr);
        return false;
    }

    const char *verb = argv[1];

    /* Raw JSON is parsed rather than copied, so a typo is caught here instead
       of by the firmware, and the result goes out compacted. */
    if (verb[0] == '{') {
        if (argc > 2) {
            fprintf(stderr,
                    "raw JSON must be a single argument -- quote it for your shell\n");
            return false;
        }

        cJSON *root = cJSON_Parse(verb);
        if (root == NULL) {
            fprintf(stderr, "that is not valid JSON\n");
            return false;
        }
        return emit(root, out, out_size);
    }

    if (strcmp(verb, "led") == 0 && argc == 3) {
        return emit(nested_string("set", "led", argv[2]), out, out_size);
    }

    if (strcmp(verb, "get") == 0 && argc == 3) {
        cJSON *root = cJSON_CreateObject();
        if (root && cJSON_AddStringToObject(root, "get", argv[2]) == NULL) {
            cJSON_Delete(root);
            root = NULL;
        }
        return emit(root, out, out_size);
    }

    if (strcmp(verb, "message") == 0 && argc >= 3) {
        /* Join the remaining words so quoting the sentence is optional. */
        size_t need = 1;
        for (int i = 2; i < argc; i++) {
            need += strlen(argv[i]) + 1;
        }

        char *joined = malloc(need);
        if (joined == NULL) {
            return false;
        }
        joined[0] = '\0';

        for (int i = 2; i < argc; i++) {
            if (i > 2) {
                strcat(joined, " ");
            }
            strcat(joined, argv[i]);
        }

        bool ok = emit(nested_string("set", "message", joined), out, out_size);
        free(joined);
        return ok;
    }

    jsoncmd_usage(stderr);
    return false;
}
